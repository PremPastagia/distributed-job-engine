#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "test_helpers.hpp"

using namespace je;
using namespace je::test;

namespace {

WorkflowNodeSpec node(const std::string& name, const std::string& type = "noop",
                      Json payload = Json::object()) {
  WorkflowNodeSpec spec;
  spec.name = name;
  spec.request.type = type;
  spec.request.payload = std::move(payload);
  return spec;
}

// Records the order in which workflow nodes execute, and the concurrency observed.
struct OrderRecorder {
  std::mutex m;
  std::vector<std::string> order;
  std::atomic<int> concurrent{0};
  std::atomic<int> max_concurrent{0};

  void install(HandlerRegistry& reg, const std::string& type, int64_t sleep_ms = 0) {
    reg.register_handler(type, [this, sleep_ms](const JobContext& ctx) {
      const int now = concurrent.fetch_add(1) + 1;
      int prev = max_concurrent.load();
      while (now > prev && !max_concurrent.compare_exchange_weak(prev, now)) {
      }
      if (sleep_ms > 0) ctx.sleep_or_cancel(sleep_ms);
      {
        std::lock_guard<std::mutex> lock(m);
        order.push_back(ctx.payload().get_string("node", ctx.job_id()));
      }
      concurrent.fetch_sub(1);
      return JobOutcome::success();
    });
  }
  std::vector<std::string> snapshot() {
    std::lock_guard<std::mutex> lock(m);
    return order;
  }
};

Json node_payload(const std::string& name) {
  Json j = Json::object();
  j.set("node", Json(name));
  return j;
}

}  // namespace

TEST(WorkflowTest, LinearWorkflowRunsInDependencyOrder) {
  Harness h(4);
  OrderRecorder rec;
  rec.install(h.handlers, "step");
  h.start();

  std::vector<WorkflowNodeSpec> nodes;
  for (const std::string& n : {"extract", "chunk", "embed", "index"}) {
    nodes.push_back(node(n, "step", node_payload(n)));
  }
  const std::vector<std::pair<std::string, std::string>> edges = {
      {"extract", "chunk"}, {"chunk", "embed"}, {"embed", "index"}};

  std::string wf_id, err;
  std::vector<std::pair<std::string, std::string>> ids;
  ASSERT_EQ(h.engine->submit_workflow("ingest", nodes, edges, wf_id, ids, err),
            WorkflowStatus::Ok)
      << err;
  ASSERT_EQ(ids.size(), 4u);
  ASSERT_TRUE(h.engine->wait_idle(15000));

  EXPECT_EQ(rec.snapshot(), (std::vector<std::string>{"extract", "chunk", "embed", "index"}));
  EXPECT_EQ(rec.max_concurrent.load(), 1) << "a linear chain has no parallelism to find";

  h.store->flush();
  WorkflowRecord wf;
  ASSERT_TRUE(h.store->get_workflow(wf_id, wf));
  EXPECT_EQ(wf.state, "SUCCEEDED");
  EXPECT_EQ(wf.node_count, 4);
  EXPECT_GT(wf.finished_at_ms, 0);
  for (const Job& j : h.store->list_workflow_jobs(wf_id)) {
    EXPECT_EQ(j.state, JobState::Succeeded) << j.workflow_node;
    EXPECT_EQ(j.pending_deps, 0);
  }
  EXPECT_EQ(h.metrics.workflows_created.load(), 1u);
  EXPECT_EQ(h.metrics.workflows_succeeded.load(), 1u);
}

TEST(WorkflowTest, IndependentBranchesRunInParallel) {
  Harness h(4);
  OrderRecorder rec;
  rec.install(h.handlers, "step", /*sleep_ms=*/120);
  h.start();

  // root -> {a, b, c} -> join : the three middle nodes are independent.
  std::vector<WorkflowNodeSpec> nodes = {node("root", "step", node_payload("root")),
                                         node("a", "step", node_payload("a")),
                                         node("b", "step", node_payload("b")),
                                         node("c", "step", node_payload("c")),
                                         node("join", "step", node_payload("join"))};
  const std::vector<std::pair<std::string, std::string>> edges = {
      {"root", "a"}, {"root", "b"}, {"root", "c"}, {"a", "join"}, {"b", "join"}, {"c", "join"}};

  std::string wf_id, err;
  std::vector<std::pair<std::string, std::string>> ids;
  const int64_t t0 = now_ms();
  ASSERT_EQ(h.engine->submit_workflow("fanout", nodes, edges, wf_id, ids, err),
            WorkflowStatus::Ok);
  ASSERT_TRUE(h.engine->wait_idle(20000));
  const int64_t elapsed = now_ms() - t0;

  const auto order = rec.snapshot();
  ASSERT_EQ(order.size(), 5u);
  EXPECT_EQ(order.front(), "root");
  EXPECT_EQ(order.back(), "join");
  EXPECT_GE(rec.max_concurrent.load(), 2)
      << "independent branches did not overlap, so there is no parallelism";
  // Critical path is 3 nodes x 120ms = 360ms; running all five serially would be 600ms.
  EXPECT_LT(elapsed, 600) << "elapsed " << elapsed << "ms suggests serial execution";
  EXPECT_GE(elapsed, 340) << "the critical path cannot be shorter than three sequential nodes";
}

TEST(WorkflowTest, DiamondFanInWaitsForEveryParent) {
  Harness h(4);
  OrderRecorder rec;
  rec.install(h.handlers, "step", 40);
  h.start();
  std::vector<WorkflowNodeSpec> nodes = {node("a", "step", node_payload("a")),
                                         node("b", "step", node_payload("b")),
                                         node("c", "step", node_payload("c")),
                                         node("d", "step", node_payload("d"))};
  const std::vector<std::pair<std::string, std::string>> edges = {
      {"a", "b"}, {"a", "c"}, {"b", "d"}, {"c", "d"}};
  std::string wf_id, err;
  std::vector<std::pair<std::string, std::string>> ids;
  ASSERT_EQ(h.engine->submit_workflow("diamond", nodes, edges, wf_id, ids, err),
            WorkflowStatus::Ok);
  ASSERT_TRUE(h.engine->wait_idle(15000));

  const auto order = rec.snapshot();
  ASSERT_EQ(order.size(), 4u);
  EXPECT_EQ(order.front(), "a");
  EXPECT_EQ(order.back(), "d") << "the join node ran before both of its parents finished";
}

TEST(WorkflowTest, CyclesAreRejectedBeforeAnythingIsPersisted) {
  Harness h(2);
  h.start();
  const std::vector<WorkflowNodeSpec> nodes = {node("a"), node("b"), node("c")};
  const std::vector<std::pair<std::string, std::string>> edges = {
      {"a", "b"}, {"b", "c"}, {"c", "a"}};
  std::string wf_id, err;
  std::vector<std::pair<std::string, std::string>> ids;
  EXPECT_EQ(h.engine->submit_workflow("cyclic", nodes, edges, wf_id, ids, err),
            WorkflowStatus::Cycle);
  EXPECT_NE(err.find("cycle"), std::string::npos);
  EXPECT_TRUE(wf_id.empty());
  h.store->flush();
  EXPECT_EQ(h.store->count_jobs(std::nullopt), 0) << "a rejected workflow must persist nothing";
  EXPECT_EQ(h.store->list_workflows(10).size(), 0u);
  EXPECT_EQ(h.engine->outstanding(), 0u) << "admission slots must be released on rejection";
}

TEST(WorkflowTest, SelfEdgesAndDanglingReferencesAreRejected) {
  Harness h(2);
  h.start();
  std::string wf_id, err;
  std::vector<std::pair<std::string, std::string>> ids;

  EXPECT_EQ(h.engine->submit_workflow("self", {node("a")}, {{"a", "a"}}, wf_id, ids, err),
            WorkflowStatus::SelfEdge);
  EXPECT_EQ(h.engine->submit_workflow("dangling", {node("a")}, {{"a", "ghost"}}, wf_id, ids, err),
            WorkflowStatus::UnknownNodeRef);
  EXPECT_EQ(h.engine->submit_workflow("dangling2", {node("a")}, {{"ghost", "a"}}, wf_id, ids, err),
            WorkflowStatus::UnknownNodeRef);
  EXPECT_EQ(h.engine->submit_workflow("dup", {node("a"), node("a")}, {}, wf_id, ids, err),
            WorkflowStatus::DuplicateNode);
  EXPECT_EQ(h.engine->submit_workflow("empty", {}, {}, wf_id, ids, err), WorkflowStatus::Empty);

  WorkflowNodeSpec bad = node("a", "no_such_type");
  EXPECT_EQ(h.engine->submit_workflow("badtype", {bad}, {}, wf_id, ids, err),
            WorkflowStatus::UnknownType);

  h.store->flush();
  EXPECT_EQ(h.store->count_jobs(std::nullopt), 0);
  EXPECT_EQ(h.engine->outstanding(), 0u);
}

TEST(WorkflowTest, AFailedNodeSkipsEveryDescendant) {
  Harness h(4);
  std::atomic<int> ran_after{0};
  h.handlers.register_handler("after", [&](const JobContext&) {
    ran_after.fetch_add(1);
    return JobOutcome::success();
  });
  h.start();

  // a -> bad -> c -> d, plus a sibling branch e that must still succeed.
  std::vector<WorkflowNodeSpec> nodes = {node("a"), node("bad", "fail"), node("c", "after"),
                                         node("d", "after"), node("e")};
  const std::vector<std::pair<std::string, std::string>> edges = {
      {"a", "bad"}, {"bad", "c"}, {"c", "d"}, {"a", "e"}};
  std::string wf_id, err;
  std::vector<std::pair<std::string, std::string>> ids;
  ASSERT_EQ(h.engine->submit_workflow("partial", nodes, edges, wf_id, ids, err),
            WorkflowStatus::Ok);
  ASSERT_TRUE(h.engine->wait_idle(15000));
  h.store->flush();

  std::unordered_map<std::string, JobState> by_node;
  for (const Job& j : h.store->list_workflow_jobs(wf_id)) by_node[j.workflow_node] = j.state;
  ASSERT_EQ(by_node.size(), 5u);
  EXPECT_EQ(by_node["a"], JobState::Succeeded);
  EXPECT_EQ(by_node["bad"], JobState::Failed);
  EXPECT_EQ(by_node["c"], JobState::Skipped);
  EXPECT_EQ(by_node["d"], JobState::Skipped) << "skipping must be transitive";
  EXPECT_EQ(by_node["e"], JobState::Succeeded) << "an unrelated branch must not be skipped";
  EXPECT_EQ(ran_after.load(), 0) << "a descendant of a failed node must never execute";

  WorkflowRecord wf;
  ASSERT_TRUE(h.store->get_workflow(wf_id, wf));
  EXPECT_EQ(wf.state, "FAILED");
  EXPECT_EQ(h.metrics.jobs_skipped.load(), 2u);
  EXPECT_EQ(h.metrics.workflows_failed.load(), 1u);
  EXPECT_EQ(h.engine->outstanding(), 0u) << "skipped jobs must release their admission slots";
}

TEST(WorkflowTest, ANodeThatSucceedsOnRetryDoesNotFailTheWorkflow) {
  Harness h(4);
  h.start();
  std::vector<WorkflowNodeSpec> nodes = {node("a"), node("flaky", "fail"), node("c")};
  nodes[1].request.payload = Json::parse_or_null(R"({"fail_until_attempt":2})");
  nodes[1].request.retry.max_attempts = 3;
  nodes[1].request.retry.backoff_base_ms = 10;
  nodes[1].request.retry.backoff_multiplier = 1.0;
  const std::vector<std::pair<std::string, std::string>> edges = {{"a", "flaky"},
                                                                  {"flaky", "c"}};
  std::string wf_id, err;
  std::vector<std::pair<std::string, std::string>> ids;
  ASSERT_EQ(h.engine->submit_workflow("retrying", nodes, edges, wf_id, ids, err),
            WorkflowStatus::Ok);
  ASSERT_TRUE(h.engine->wait_idle(15000));
  h.store->flush();

  WorkflowRecord wf;
  ASSERT_TRUE(h.store->get_workflow(wf_id, wf));
  EXPECT_EQ(wf.state, "SUCCEEDED");
  for (const Job& j : h.store->list_workflow_jobs(wf_id)) {
    EXPECT_EQ(j.state, JobState::Succeeded) << j.workflow_node;
    if (j.workflow_node == "flaky") EXPECT_EQ(j.attempt, 2);
  }
}

TEST(WorkflowTest, CancellingAWorkflowStopsEveryUnfinishedNode) {
  Harness h(2);
  h.handlers.register_handler("slow", [](const JobContext& ctx) {
    ctx.sleep_or_cancel(400);
    return JobOutcome::success();
  });
  h.start();
  std::vector<WorkflowNodeSpec> nodes = {node("a", "slow"), node("b", "slow"),
                                         node("c", "slow")};
  const std::vector<std::pair<std::string, std::string>> edges = {{"a", "b"}, {"b", "c"}};
  std::string wf_id, err;
  std::vector<std::pair<std::string, std::string>> ids;
  ASSERT_EQ(h.engine->submit_workflow("cancelme", nodes, edges, wf_id, ids, err),
            WorkflowStatus::Ok);
  ASSERT_TRUE(wait_until([&] { return h.engine->running_count() > 0; }, 3000));

  EXPECT_EQ(h.engine->cancel_workflow(wf_id, err), CancelStatus::Ok);
  ASSERT_TRUE(h.engine->wait_idle(10000));
  h.store->flush();

  WorkflowRecord wf;
  ASSERT_TRUE(h.store->get_workflow(wf_id, wf));
  EXPECT_EQ(wf.state, "CANCELLED");
  // A cancelled workflow must not also be counted as a failure: its nodes reach terminal
  // states as a result of the cancellation, and the last one must not relabel it.
  EXPECT_EQ(h.metrics.workflows_failed.load(), 0u)
      << "a cancelled workflow was also recorded as failed";
  EXPECT_EQ(h.metrics.workflows_succeeded.load(), 0u);
  for (const Job& j : h.store->list_workflow_jobs(wf_id)) {
    EXPECT_TRUE(is_terminal(j.state)) << j.workflow_node << " is " << to_string(j.state);
    EXPECT_NE(j.state, JobState::Succeeded) << j.workflow_node << " should not have completed";
  }
  EXPECT_EQ(h.engine->outstanding(), 0u);
  EXPECT_EQ(h.engine->cancel_workflow(wf_id, err), CancelStatus::AlreadyTerminal);
  EXPECT_EQ(h.engine->cancel_workflow("w_missing", err), CancelStatus::NotFound);
}

TEST(WorkflowTest, WorkflowNodesRespectPriorityWithinTheReadySet) {
  Harness h(0);
  // The node type must exist before submission: the engine validates types up front.
  h.handlers.register_handler("step", [](const JobContext&) { return JobOutcome::success(); });
  h.start();
  std::vector<WorkflowNodeSpec> nodes = {node("low", "step", node_payload("low")),
                                         node("high", "step", node_payload("high")),
                                         node("mid", "step", node_payload("mid"))};
  nodes[0].request.priority = 0;
  nodes[1].request.priority = 9;
  nodes[2].request.priority = 5;
  std::string wf_id, err;
  std::vector<std::pair<std::string, std::string>> ids;
  ASSERT_EQ(h.engine->submit_workflow("prio", nodes, {}, wf_id, ids, err), WorkflowStatus::Ok);
  h.engine->stop(false);

  OrderRecorder rec;
  rec.install(h.handlers, "step");
  EngineConfig cfg;
  cfg.worker_threads = 1;
  JobEngine engine2(cfg, h.store.get(), &h.handlers, &h.metrics);
  engine2.recover();
  engine2.start();
  ASSERT_TRUE(engine2.wait_idle(10000));
  engine2.stop(false);
  EXPECT_EQ(rec.snapshot(), (std::vector<std::string>{"high", "mid", "low"}));
}

TEST(WorkflowTest, LargeFanOutCompletesEveryNode) {
  Harness h(6);
  h.start();
  std::vector<WorkflowNodeSpec> nodes = {node("root")};
  std::vector<std::pair<std::string, std::string>> edges;
  constexpr int kLeaves = 60;
  for (int i = 0; i < kLeaves; ++i) {
    const std::string n = "leaf" + std::to_string(i);
    nodes.push_back(node(n));
    edges.emplace_back("root", n);
  }
  std::string wf_id, err;
  std::vector<std::pair<std::string, std::string>> ids;
  ASSERT_EQ(h.engine->submit_workflow("fan", nodes, edges, wf_id, ids, err),
            WorkflowStatus::Ok);
  ASSERT_TRUE(h.engine->wait_idle(30000));
  h.store->flush();
  const auto jobs = h.store->list_workflow_jobs(wf_id);
  ASSERT_EQ(jobs.size(), static_cast<std::size_t>(kLeaves + 1));
  for (const Job& j : jobs) EXPECT_EQ(j.state, JobState::Succeeded) << j.workflow_node;
  WorkflowRecord wf;
  ASSERT_TRUE(h.store->get_workflow(wf_id, wf));
  EXPECT_EQ(wf.state, "SUCCEEDED");
}
