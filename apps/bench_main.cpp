// jobengine-bench: in-process benchmark driver.
//
// It exercises the engine directly (no HTTP), so the numbers isolate scheduling and
// execution from network and JSON costs. Every run writes one CSV row per job plus an
// environment header, and prints exact percentiles computed from the raw samples.
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "cli.hpp"
#include "jobengine/engine.hpp"
#include "jobengine/handlers.hpp"
#include "jobengine/metrics.hpp"
#include "jobengine/store.hpp"

namespace {

struct Completion {
  std::string job_id;
  std::string state;
  int priority = 0;
  int attempt = 0;
  int64_t queue_wait_us = 0;
  int64_t exec_us = 0;
  int64_t complete_us = 0;
};

}  // namespace

int main(int argc, char** argv) {
  using namespace je;
  const Args args(argc, argv);
  if (args.has("help")) {
    std::cout <<
        "jobengine-bench - in-process engine benchmark\n\n"
        "  --workers <n>        worker threads (default 4)\n"
        "  --jobs <n>           number of jobs to submit (default 10000)\n"
        "  --submitters <n>     producer threads (default 4)\n"
        "  --type <name>        job type (default noop)\n"
        "  --payload <json>     job payload (default {})\n"
        "  --priorities <spec>  e.g. 0:50,9:50 -> 50% priority 0, 50% priority 9\n"
        "  --max-attempts <n>   retry budget per job (default 1)\n"
        "  --timeout-ms <n>     per-job deadline, 0 = none\n"
        "  --rate <n>           target submissions/second; 0 = saturate (default 0)\n"
        "  --commit-delay-us <n> group-commit batching delay (default 0)\n"
        "  --group-commit <bool> batch writes into shared transactions (default true)\n"
        "  --db <path>          SQLite path (default :memory:)\n"
        "  --queue-capacity <n> admission limit (default 200000)\n"
        "  --out <file.csv>     per-job CSV output\n"
        "  --label <text>       label recorded in the CSV header\n"
        "\n workflow mode:\n"
        "  --workflow <shape>   chain | fanout | diamond (enables workflow mode)\n"
        "  --workflow-nodes <n> nodes per workflow (default 4)\n"
        "  --workflows <n>      number of workflows to submit (default 200)\n";
    return 0;
  }
  apply_log_level(args, "warn");

  const int worker_threads = static_cast<int>(args.integer("workers", 4));
  const int64_t total_jobs = args.integer("jobs", 10000);
  const int submitters = static_cast<int>(args.integer("submitters", 4));
  const std::string type = args.str("type", "noop");
  const std::string payload_text = args.str("payload", "{}");
  const std::string out_path = args.str("out", "");
  const std::string label = args.str("label", type);
  // Rate limiting matters for latency measurement. With an unbounded submit rate the
  // queue saturates and "latency" degenerates into a measure of backlog depth. With
  // --rate, each job has an *intended* submission time and latency is measured from that
  // time, so a submitter that falls behind still reports the delay it caused
  // (the standard guard against coordinated omission).
  const double target_rate = args.real("rate", 0.0);

  Json payload;
  std::string perr;
  if (!Json::parse(payload_text, payload, perr)) {
    std::cerr << "invalid --payload JSON: " << perr << "\n";
    return 2;
  }

  // "0:50,9:50" -> a weighted list of priorities assigned round-robin across submissions.
  std::vector<int> priority_pool;
  {
    const std::string spec = args.str("priorities", "0:100");
    for (const std::string& part : split(spec, ',')) {
      const std::size_t colon = part.find(':');
      if (colon == std::string::npos) continue;
      int64_t p = 0, weight = 0;
      if (!parse_int(part.substr(0, colon), p) || !parse_int(part.substr(colon + 1), weight)) {
        std::cerr << "invalid --priorities spec: " << spec << "\n";
        return 2;
      }
      for (int64_t i = 0; i < weight; ++i) priority_pool.push_back(static_cast<int>(p));
    }
  }
  if (priority_pool.empty()) priority_pool.push_back(0);

  const std::string workflow_shape = args.str("workflow", "");
  const int workflow_nodes = static_cast<int>(args.integer("workflow-nodes", 4));
  const int64_t workflow_count = args.integer("workflows", 200);
  if (!workflow_shape.empty() && workflow_shape != "chain" && workflow_shape != "fanout" &&
      workflow_shape != "diamond") {
    std::cerr << "unknown --workflow shape: " << workflow_shape << "\n";
    return 2;
  }

  StoreConfig store_cfg;
  store_cfg.path = args.str("db", ":memory:");
  store_cfg.reader_connections = 4;
  store_cfg.commit_delay_us = args.integer("commit-delay-us", 0);
  // Exposed so the group-commit improvement can be measured as a controlled A/B from one
  // binary rather than compared against a remembered number.
  store_cfg.group_commit = args.flag("group-commit", true);
  std::string err;
  std::unique_ptr<JobStore> store = JobStore::open(store_cfg, err);
  if (!store) { std::cerr << "cannot open store: " << err << "\n"; return 1; }

  HandlerRegistry handlers;
  HandlerRegistry::register_builtins(handlers);
  if (!handlers.has(type)) { std::cerr << "unknown job type: " << type << "\n"; return 2; }
  Metrics metrics;

  EngineConfig cfg;
  cfg.worker_threads = worker_threads;
  cfg.max_outstanding_jobs = static_cast<std::size_t>(args.integer("queue-capacity", 200000));
  cfg.node_name = "bench";
  JobEngine engine(cfg, store.get(), &handlers, &metrics);

  // ---- workflow mode ---------------------------------------------------------------------
  // Measures end-to-end workflow latency: the interval from submitting a DAG to the moment
  // its last node reaches a terminal state.
  if (!workflow_shape.empty()) {
    std::mutex wf_mutex;
    std::unordered_map<std::string, int> remaining;
    std::unordered_map<std::string, int64_t> started_us;
    // A workflow's nodes can start completing before submit_workflow returns and the
    // bookkeeping below is registered. Those early completions are counted here and
    // subtracted at registration time, otherwise such a workflow would never be seen to
    // finish and its latency would silently go unrecorded.
    std::unordered_map<std::string, int> completed_before_registration;
    std::vector<int64_t> workflow_latency_us;
    std::vector<int64_t> node_queue_wait_us;
    std::atomic<int64_t> nodes_done{0};
    std::atomic<int64_t> nodes_failed{0};

    engine.set_completion_callback([&](const Job& job) {
      nodes_done.fetch_add(1, std::memory_order_relaxed);
      if (job.state != JobState::Succeeded) nodes_failed.fetch_add(1, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lock(wf_mutex);
      node_queue_wait_us.push_back(job.queue_wait_us);
      const auto it = remaining.find(job.workflow_id);
      if (it == remaining.end()) {
        completed_before_registration[job.workflow_id] += 1;
        return;
      }
      if (--it->second == 0) {
        workflow_latency_us.push_back(now_us() - started_us[job.workflow_id]);
      }
    });
    engine.start();

    const int64_t wf_t0 = now_us();
    int64_t submitted_workflows = 0;
    for (int64_t w = 0; w < workflow_count; ++w) {
      std::vector<WorkflowNodeSpec> nodes;
      std::vector<std::pair<std::string, std::string>> edges;
      const auto make_node = [&](const std::string& name) {
        WorkflowNodeSpec spec;
        spec.name = name;
        spec.request.type = type;
        spec.request.payload = payload;
        nodes.push_back(spec);
      };
      if (workflow_shape == "chain") {
        for (int i = 0; i < workflow_nodes; ++i) make_node("n" + num(i));
        for (int i = 1; i < workflow_nodes; ++i) edges.emplace_back("n" + num(i - 1), "n" + num(i));
      } else if (workflow_shape == "fanout") {
        make_node("root");
        for (int i = 1; i < workflow_nodes; ++i) {
          make_node("n" + num(i));
          edges.emplace_back("root", "n" + num(i));
        }
      } else {  // diamond: root -> middle layer -> join
        make_node("root");
        const int middle = std::max(1, workflow_nodes - 2);
        for (int i = 0; i < middle; ++i) {
          make_node("m" + num(i));
          edges.emplace_back("root", "m" + num(i));
        }
        make_node("join");
        for (int i = 0; i < middle; ++i) edges.emplace_back("m" + num(i), "join");
      }

      std::string wf_id, werr;
      std::vector<std::pair<std::string, std::string>> node_ids;
      const int64_t t_submit = now_us();
      const WorkflowStatus st =
          engine.submit_workflow("bench", nodes, edges, wf_id, node_ids, werr);
      if (st != WorkflowStatus::Ok) {
        std::cerr << "workflow submission failed: " << werr << "\n";
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(wf_mutex);
        int outstanding_nodes = static_cast<int>(nodes.size());
        const auto early = completed_before_registration.find(wf_id);
        if (early != completed_before_registration.end()) {
          outstanding_nodes -= early->second;
          completed_before_registration.erase(early);
        }
        started_us[wf_id] = t_submit;
        if (outstanding_nodes <= 0) {
          workflow_latency_us.push_back(now_us() - t_submit);
        } else {
          remaining[wf_id] = outstanding_nodes;
        }
      }
      ++submitted_workflows;
    }

    const bool wf_idle = engine.wait_idle(600000);
    const int64_t wf_t1 = now_us();
    engine.stop(false);
    if (!wf_idle) std::cerr << "WARNING: workflows did not all complete within 600s\n";

    const double wf_wall_s = static_cast<double>(wf_t1 - wf_t0) / 1e6;
    const SummaryStats wf_stats = SummaryStats::from(workflow_latency_us);
    const SummaryStats wf_queue = SummaryStats::from(node_queue_wait_us);

    if (!out_path.empty()) {
      std::ofstream out(out_path);
      if (!out) { std::cerr << "cannot write " << out_path << "\n"; return 1; }
      out << environment_block("# ");
      out << "# label=" << label << "\n";
      out << "# workload=workflow " << workflow_shape << " with " << workflow_nodes
          << " nodes, node type=" << type << ", payload=" << payload.dump() << "\n";
      out << "# worker_threads=" << worker_threads << " workflows=" << submitted_workflows
          << " db=" << store_cfg.path << "\n";
      out << "# measurement: workflow_latency_us is submit-call to the completion of the "
             "workflow's last node, steady clock\n";
      out << "workflow_index,workflow_latency_us\n";
      for (std::size_t i = 0; i < workflow_latency_us.size(); ++i) {
        out << i << ',' << workflow_latency_us[i] << '\n';
      }
    }

    std::cout << "BENCH label=" << label << " mode=workflow shape=" << workflow_shape
              << " nodes_per_workflow=" << workflow_nodes
              << " workers=" << worker_threads
              << " workflows_submitted=" << submitted_workflows
              << " workflows_completed=" << workflow_latency_us.size()
              << " nodes_completed=" << nodes_done.load()
              << " nodes_failed=" << nodes_failed.load() << "\n";
    std::printf("BENCH wall_s=%.4f throughput_workflows_per_s=%.2f throughput_nodes_per_s=%.1f\n",
                wf_wall_s,
                wf_wall_s > 0 ? static_cast<double>(workflow_latency_us.size()) / wf_wall_s : 0.0,
                wf_wall_s > 0 ? static_cast<double>(nodes_done.load()) / wf_wall_s : 0.0);
    const ResourceUsage wf_usage = ResourceUsage::capture();
    std::printf("BENCH cpu_seconds=%.3f cores_used=%.2f peak_rss_mb=%.1f ctx_switches=%lld\n",
                wf_usage.total_cpu_s(), wf_usage.cores_used(wf_wall_s), wf_usage.peak_rss_mb,
                static_cast<long long>(wf_usage.voluntary_ctx_switches +
                                       wf_usage.involuntary_ctx_switches));
    std::cout << wf_stats.to_line("BENCH workflow_latency") << "\n";
    std::cout << wf_queue.to_line("BENCH node_queue_wait") << "\n";
    if (!out_path.empty()) std::cout << "BENCH csv=" << out_path << "\n";
    return 0;
  }

  std::mutex completions_mutex;
  std::vector<Completion> completions;
  completions.reserve(static_cast<std::size_t>(total_jobs));
  engine.set_completion_callback([&](const Job& job) {
    Completion c;
    c.job_id = job.job_id;
    c.state = to_string(job.state);
    c.priority = job.priority;
    c.attempt = job.attempt;
    c.queue_wait_us = job.queue_wait_us;
    c.exec_us = job.exec_us;
    c.complete_us = now_us();
    std::lock_guard<std::mutex> lock(completions_mutex);
    completions.push_back(std::move(c));
  });
  engine.start();

  std::vector<std::vector<std::pair<std::string, int64_t>>> submit_times(
      static_cast<std::size_t>(submitters));
  std::vector<int64_t> submit_latency_us;
  std::mutex submit_latency_mutex;
  std::atomic<int64_t> next_index{0};
  std::atomic<int64_t> rejected{0};

  const int64_t t_start = now_us();
  const int64_t t_start_ref = t_start;
  {
    std::vector<std::thread> producers;
    for (int t = 0; t < submitters; ++t) {
      producers.emplace_back([&, t] {
        std::vector<int64_t> local_latency;
        for (;;) {
          const int64_t i = next_index.fetch_add(1, std::memory_order_relaxed);
          if (i >= total_jobs) break;
          int64_t intended = 0;
          if (target_rate > 0.0) {
            intended = t_start_ref + static_cast<int64_t>(
                                         (static_cast<double>(i) / target_rate) * 1e6);
            while (now_us() < intended) {
              const int64_t remaining = intended - now_us();
              if (remaining > 2000) {
                std::this_thread::sleep_for(std::chrono::microseconds(remaining - 1000));
              } else {
                std::this_thread::yield();
              }
            }
          }
          SubmitRequest req;
          req.type = type;
          req.payload = payload;
          req.priority = priority_pool[static_cast<std::size_t>(i) % priority_pool.size()];
          req.retry.max_attempts = static_cast<int>(args.integer("max-attempts", 1));
          req.timeout_ms = args.integer("timeout-ms", 0);
          std::string job_id, serr;
          const int64_t t0 = now_us();
          const SubmitStatus st = engine.submit(req, job_id, serr);
          const int64_t t1 = now_us();
          if (st != SubmitStatus::Ok) {
            rejected.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
          local_latency.push_back(t1 - t0);
          submit_times[static_cast<std::size_t>(t)].emplace_back(
              job_id, target_rate > 0.0 ? intended : t0);
        }
        std::lock_guard<std::mutex> lock(submit_latency_mutex);
        submit_latency_us.insert(submit_latency_us.end(), local_latency.begin(),
                                 local_latency.end());
      });
    }
    for (auto& p : producers) p.join();
  }
  const int64_t t_submitted = now_us();

  const bool idle = engine.wait_idle(600000);
  const int64_t t_done = now_us();
  engine.stop(false);
  if (!idle) {
    std::cerr << "WARNING: engine did not reach idle within 600s\n";
  }

  // Join completion records with their submit timestamps to get end-to-end latency.
  std::unordered_map<std::string, int64_t> submitted;
  for (const auto& v : submit_times) {
    for (const auto& kv : v) submitted[kv.first] = kv.second;
  }
  std::vector<int64_t> e2e_us, queue_us, exec_us;
  e2e_us.reserve(completions.size());
  std::size_t succeeded = 0, failed = 0, other = 0;
  int64_t attempts_total = 0;
  for (const Completion& c : completions) {
    const auto it = submitted.find(c.job_id);
    if (it != submitted.end()) e2e_us.push_back(c.complete_us - it->second);
    queue_us.push_back(c.queue_wait_us);
    exec_us.push_back(c.exec_us);
    attempts_total += c.attempt;
    if (c.state == "SUCCEEDED") ++succeeded;
    else if (c.state == "FAILED" || c.state == "TIMED_OUT") ++failed;
    else ++other;
  }

  const double wall_s = static_cast<double>(t_done - t_start) / 1e6;
  const double submit_s = static_cast<double>(t_submitted - t_start) / 1e6;
  const double throughput = wall_s > 0 ? static_cast<double>(completions.size()) / wall_s : 0.0;
  const double submit_rate = submit_s > 0 ? static_cast<double>(submitted.size()) / submit_s : 0.0;

  if (!out_path.empty()) {
    std::ofstream out(out_path);
    if (!out) { std::cerr << "cannot write " << out_path << "\n"; return 1; }
    out << environment_block("# ");
    out << "# label=" << label << "\n";
    out << "# workload=in-process engine, type=" << type << ", payload=" << payload.dump() << "\n";
    out << "# worker_threads=" << worker_threads << " submitter_threads=" << submitters
        << " jobs_requested=" << total_jobs << " priorities=" << args.str("priorities", "0:100")
        << " target_rate_per_s=" << target_rate
        << " commit_delay_us=" << store_cfg.commit_delay_us
        << " group_commit=" << (store_cfg.group_commit ? "true" : "false")
        << " max_attempts=" << args.integer("max-attempts", 1)
        << " timeout_ms=" << args.integer("timeout-ms", 0) << " db=" << store_cfg.path << "\n";
    out << "# measurement: queue_wait_us and exec_us are engine-measured per attempt; "
           "e2e_us runs from the intended submission time (rate mode) or the submit call "
           "(saturating mode) to the completion callback, steady clock\n";
    out << "job_id,state,priority,attempt,queue_wait_us,exec_us,e2e_us\n";
    for (const Completion& c : completions) {
      const auto it = submitted.find(c.job_id);
      const int64_t e2e = it == submitted.end() ? -1 : c.complete_us - it->second;
      out << c.job_id << ',' << c.state << ',' << c.priority << ',' << c.attempt << ','
          << c.queue_wait_us << ',' << c.exec_us << ',' << e2e << '\n';
    }
  }

  const SummaryStats e2e = SummaryStats::from(e2e_us);
  const SummaryStats queue = SummaryStats::from(queue_us);
  const SummaryStats exec = SummaryStats::from(exec_us);
  const SummaryStats submit_lat = SummaryStats::from(submit_latency_us);

  std::cout << "BENCH label=" << label << " type=" << type << " workers=" << worker_threads
            << " submitters=" << submitters << " submitted=" << submitted.size()
            << " completed=" << completions.size() << " rejected=" << rejected.load()
            << " succeeded=" << succeeded << " failed=" << failed << " other=" << other
            << " attempts_total=" << attempts_total << "\n";
  std::printf("BENCH wall_s=%.4f throughput_jobs_per_s=%.1f submit_rate_jobs_per_s=%.1f\n",
              wall_s, throughput, submit_rate);
  const ResourceUsage usage = ResourceUsage::capture();
  std::printf("BENCH cpu_seconds=%.3f cores_used=%.2f peak_rss_mb=%.1f ctx_switches=%lld\n",
              usage.total_cpu_s(), usage.cores_used(wall_s), usage.peak_rss_mb,
              static_cast<long long>(usage.voluntary_ctx_switches +
                                     usage.involuntary_ctx_switches));
  const uint64_t batches = store->batches_committed();
  const uint64_t writes = store->writes_committed();
  std::printf("BENCH store_batches=%llu store_writes=%llu avg_rows_per_transaction=%.2f\n",
              static_cast<unsigned long long>(batches),
              static_cast<unsigned long long>(writes),
              batches ? static_cast<double>(writes) / static_cast<double>(batches) : 0.0);
  std::cout << e2e.to_line("BENCH e2e") << "\n";
  std::cout << queue.to_line("BENCH queue_wait") << "\n";
  std::cout << exec.to_line("BENCH exec") << "\n";
  std::cout << submit_lat.to_line("BENCH submit_call") << "\n";
  if (!out_path.empty()) std::cout << "BENCH csv=" << out_path << "\n";
  return 0;
}
