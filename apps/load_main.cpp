// jobengine-load: HTTP load generator for the coordinator's REST API.
//
// Each connection is a thread with its own keep-alive client. Every request's latency is
// recorded individually, so percentiles come from raw samples rather than from a histogram.
#include <atomic>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "cli.hpp"
#include "jobengine/http.hpp"
#include "jobengine/json.hpp"

namespace {

struct Sample {
  int64_t t_offset_us = 0;
  int conn = 0;
  int status = 0;
  int64_t latency_us = 0;
  char endpoint = 's';  // s=submit, g=get, h=health, l=list
};

}  // namespace

int main(int argc, char** argv) {
  using namespace je;
  const Args args(argc, argv);
  if (args.has("help")) {
    std::cout <<
        "jobengine-load - HTTP load generator\n\n"
        "  --host <addr>         coordinator host (default 127.0.0.1)\n"
        "  --port <n>            coordinator port (default 8080)\n"
        "  --connections <n>     concurrent keep-alive connections (default 8)\n"
        "  --requests <n>        total requests to issue (default 10000)\n"
        "  --mode <m>            submit | status | health | mixed (default submit)\n"
        "  --type <name>         job type for submit mode (default noop)\n"
        "  --payload <json>      job payload (default {})\n"
        "  --priority <n>        job priority (default 0)\n"
        "  --out <file.csv>      per-request CSV output\n"
        "  --label <text>        label recorded in the CSV header\n";
    return 0;
  }
  apply_log_level(args, "warn");

  const std::string host = args.str("host", "127.0.0.1");
  const int port = static_cast<int>(args.integer("port", 8080));
  const int connections = static_cast<int>(args.integer("connections", 8));
  const int64_t requests = args.integer("requests", 10000);
  const std::string mode = args.str("mode", "submit");
  const std::string type = args.str("type", "noop");
  const std::string payload = args.str("payload", "{}");
  const int priority = static_cast<int>(args.integer("priority", 0));
  const std::string out_path = args.str("out", "");
  const std::string label = args.str("label", mode);

  if (mode != "submit" && mode != "status" && mode != "health" && mode != "mixed") {
    std::cerr << "unknown --mode: " << mode << "\n";
    return 2;
  }

  // status/mixed mode needs an existing job to look up; create one up front so the
  // measured GET path is a hit rather than a 404.
  std::string probe_job_id;
  if (mode == "status" || mode == "mixed") {
    HttpClient c(host, port, 10000);
    Json body = Json::object();
    body.set("type", Json(std::string("noop")));
    const HttpClientResponse r = c.post("/jobs", body.dump());
    if (!r.ok || r.status != 201) {
      std::cerr << "cannot create a probe job (" << r.error << " status=" << r.status << ")\n";
      return 1;
    }
    probe_job_id = Json::parse_or_null(r.body).get_string("job_id", "");
    if (probe_job_id.empty()) { std::cerr << "probe job has no id\n"; return 1; }
  }

  std::string submit_body;
  {
    Json j = Json::object();
    j.set("type", Json(type));
    j.set("payload", Json::parse_or_null(payload));
    j.set("priority", Json(static_cast<int64_t>(priority)));
    submit_body = j.dump();
  }

  std::atomic<int64_t> issued{0};
  std::atomic<int64_t> transport_errors{0};
  std::mutex samples_mutex;
  std::vector<Sample> samples;
  samples.reserve(static_cast<std::size_t>(requests));

  const int64_t t_start = now_us();
  std::vector<std::thread> threads;
  for (int c = 0; c < connections; ++c) {
    threads.emplace_back([&, c] {
      HttpClient client(host, port, 30000);
      std::vector<Sample> local;
      local.reserve(static_cast<std::size_t>(requests / connections + 8));
      for (;;) {
        const int64_t i = issued.fetch_add(1, std::memory_order_relaxed);
        if (i >= requests) break;

        char endpoint = 's';
        HttpClientResponse res;
        const int64_t t0 = now_us();
        if (mode == "submit") {
          res = client.post("/jobs", submit_body);
        } else if (mode == "status") {
          endpoint = 'g';
          res = client.get("/jobs/" + probe_job_id);
        } else if (mode == "health") {
          endpoint = 'h';
          res = client.get("/health");
        } else {  // mixed: 50% submit, 50% status lookup
          if (i % 2 == 0) { res = client.post("/jobs", submit_body); }
          else { endpoint = 'g'; res = client.get("/jobs/" + probe_job_id); }
        }
        const int64_t t1 = now_us();

        Sample s;
        s.t_offset_us = t0 - t_start;
        s.conn = c;
        s.status = res.ok ? res.status : 0;
        s.latency_us = t1 - t0;
        s.endpoint = endpoint;
        if (!res.ok) transport_errors.fetch_add(1, std::memory_order_relaxed);
        local.push_back(s);
      }
      std::lock_guard<std::mutex> lock(samples_mutex);
      samples.insert(samples.end(), local.begin(), local.end());
    });
  }
  for (auto& t : threads) t.join();
  const int64_t t_end = now_us();

  std::vector<int64_t> all_latency, ok_latency;
  std::size_t ok_count = 0, client_err = 0, server_err = 0;
  for (const Sample& s : samples) {
    all_latency.push_back(s.latency_us);
    if (s.status >= 200 && s.status < 300) { ++ok_count; ok_latency.push_back(s.latency_us); }
    else if (s.status >= 500 || s.status == 0) ++server_err;
    else if (s.status >= 400) ++client_err;
  }

  if (!out_path.empty()) {
    std::ofstream out(out_path);
    if (!out) { std::cerr << "cannot write " << out_path << "\n"; return 1; }
    out << environment_block("# ");
    out << "# label=" << label << "\n";
    out << "# target=http://" << host << ":" << port << " mode=" << mode
        << " connections=" << connections << " requests=" << requests << " type=" << type
        << " payload=" << payload << "\n";
    out << "# measurement: latency_us is the client-side duration of one request/response "
           "on a keep-alive connection, steady clock\n";
    out << "t_offset_us,conn,endpoint,status,latency_us\n";
    for (const Sample& s : samples) {
      out << s.t_offset_us << ',' << s.conn << ',' << s.endpoint << ',' << s.status << ','
          << s.latency_us << '\n';
    }
  }

  const double wall_s = static_cast<double>(t_end - t_start) / 1e6;
  const double rps = wall_s > 0 ? static_cast<double>(samples.size()) / wall_s : 0.0;
  const SummaryStats all = SummaryStats::from(all_latency);
  const SummaryStats ok = SummaryStats::from(ok_latency);

  std::cout << "LOAD label=" << label << " mode=" << mode << " connections=" << connections
            << " requests=" << samples.size() << " ok=" << ok_count
            << " client_errors=" << client_err << " server_errors=" << server_err
            << " transport_errors=" << transport_errors.load() << "\n";
  std::printf("LOAD wall_s=%.4f throughput_rps=%.1f error_rate=%.6f\n", wall_s, rps,
              samples.empty() ? 0.0
                              : static_cast<double>(samples.size() - ok_count) /
                                    static_cast<double>(samples.size()));
  const ResourceUsage usage = ResourceUsage::capture();
  std::printf("LOAD cpu_seconds=%.3f cores_used=%.2f peak_rss_mb=%.1f\n",
              usage.total_cpu_s(), usage.cores_used(wall_s), usage.peak_rss_mb);
  std::cout << all.to_line("LOAD latency_all") << "\n";
  std::cout << ok.to_line("LOAD latency_2xx") << "\n";
  if (!out_path.empty()) std::cout << "LOAD csv=" << out_path << "\n";
  return 0;
}
