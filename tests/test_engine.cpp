#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "test_helpers.hpp"

using namespace je;
using namespace je::test;

TEST(EngineTest, ExecutesASubmittedJobAndRecordsTheResult) {
  Harness h(2);
  h.start();
  const std::string id = h.submit("echo", Json::parse_or_null(R"({"hello":"world"})"));
  ASSERT_FALSE(id.empty());
  ASSERT_TRUE(h.wait_terminal(id));

  Job job;
  ASSERT_TRUE(h.engine->get_job(id, job));
  EXPECT_EQ(job.state, JobState::Succeeded);
  EXPECT_EQ(job.attempt, 1);
  EXPECT_EQ(job.result.find("echo")->get_string("hello", ""), "world");
  EXPECT_TRUE(job.error.empty());
  EXPECT_GT(job.started_at_wall_ms, 0);
  EXPECT_GT(job.finished_at_wall_ms, 0);
  EXPECT_EQ(h.metrics.jobs_succeeded.load(), 1u);
  EXPECT_EQ(h.metrics.jobs_submitted.load(), 1u);
}

TEST(EngineTest, RejectsAnUnknownJobTypeWithoutCreatingARow) {
  Harness h(1);
  h.start();
  SubmitRequest req;
  req.type = "no_such_handler";
  std::string id, err;
  EXPECT_EQ(h.engine->submit(req, id, err), SubmitStatus::UnknownType);
  EXPECT_TRUE(id.empty());
  EXPECT_NE(err.find("unknown job type"), std::string::npos);
  EXPECT_EQ(h.store->count_jobs(std::nullopt), 0) << "a rejected submission must leave no row";
  EXPECT_EQ(h.engine->outstanding(), 0u);
}

TEST(EngineTest, AHandlerThatThrowsBecomesAFailedJobNotACrash) {
  Harness h(1);
  h.start();
  const std::string id = h.submit("throw");
  ASSERT_FALSE(id.empty());
  ASSERT_TRUE(h.wait_terminal(id));
  Job job;
  ASSERT_TRUE(h.engine->get_job(id, job));
  EXPECT_EQ(job.state, JobState::Failed);
  EXPECT_NE(job.error.find("handler threw"), std::string::npos);
}

TEST(EngineTest, AdmissionControlRejectsBeyondCapacity) {
  EngineConfig cfg;
  cfg.max_outstanding_jobs = 5;
  Harness h(0, cfg);  // no workers: nothing drains, so the limit is reached deterministically
  h.start();
  int accepted = 0, rejected = 0;
  for (int i = 0; i < 20; ++i) {
    SubmitRequest req;
    req.type = "noop";
    std::string id, err;
    if (h.engine->submit(req, id, err) == SubmitStatus::Ok) ++accepted;
    else ++rejected;
  }
  EXPECT_EQ(accepted, 5);
  EXPECT_EQ(rejected, 15);
  EXPECT_EQ(h.engine->outstanding(), 5u);
  EXPECT_EQ(h.metrics.jobs_rejected_queue_full.load(), 15u);
  EXPECT_EQ(h.store->count_jobs(std::nullopt), 5) << "rejected submissions must leave no rows";
}

TEST(EngineTest, HigherPriorityJobsRunFirst) {
  // One worker, so execution order is fully determined by queue order. The jobs are all
  // queued before the worker starts, otherwise the first arrival would just run at once.
  EngineConfig cfg;
  Harness h(0, cfg);
  h.start();
  std::vector<std::pair<std::string, int>> ids;
  const std::vector<int> priorities = {0, 5, 1, 9, 5, 0, 9};
  for (std::size_t i = 0; i < priorities.size(); ++i) {
    const std::string id = h.submit("noop", Json::object(), priorities[i]);
    ASSERT_FALSE(id.empty());
    ids.emplace_back(id, priorities[i]);
  }
  h.engine->stop(false);

  // Restart with a single worker and a recording handler to capture the execution order.
  std::vector<std::string> order;
  std::mutex order_mutex;
  h.handlers.register_handler("noop", [&](const JobContext& ctx) {
    std::lock_guard<std::mutex> lock(order_mutex);
    order.push_back(ctx.job_id());
    return JobOutcome::success();
  });
  EngineConfig single;
  single.worker_threads = 1;
  single.node_name = "test2";
  JobEngine engine2(single, h.store.get(), &h.handlers, &h.metrics);
  ASSERT_EQ(engine2.recover(), static_cast<int>(priorities.size()));
  engine2.start();
  ASSERT_TRUE(engine2.wait_idle(10000));
  engine2.stop(false);

  ASSERT_EQ(order.size(), priorities.size());
  std::vector<int> observed;
  for (const std::string& id : order) {
    for (const auto& kv : ids) {
      if (kv.first == id) observed.push_back(kv.second);
    }
  }
  // Priorities must come out in non-increasing order.
  for (std::size_t i = 1; i < observed.size(); ++i) {
    EXPECT_GE(observed[i - 1], observed[i])
        << "priority order violated at index " << i;
  }
  EXPECT_EQ(observed.front(), 9);
  EXPECT_EQ(observed.back(), 0);
}

TEST(EngineTest, FifoWithinAPriorityLevel) {
  Harness h(0);
  h.start();
  std::vector<std::string> submitted;
  for (int i = 0; i < 8; ++i) submitted.push_back(h.submit("noop", Json::object(), 3));
  h.engine->stop(false);

  std::vector<std::string> order;
  std::mutex m;
  h.handlers.register_handler("noop", [&](const JobContext& ctx) {
    std::lock_guard<std::mutex> lock(m);
    order.push_back(ctx.job_id());
    return JobOutcome::success();
  });
  EngineConfig cfg;
  cfg.worker_threads = 1;
  JobEngine engine2(cfg, h.store.get(), &h.handlers, &h.metrics);
  engine2.recover();
  engine2.start();
  ASSERT_TRUE(engine2.wait_idle(10000));
  engine2.stop(false);
  EXPECT_EQ(order, submitted) << "equal priorities must run in submission order";
}

TEST(EngineTest, DelayedJobsDoNotRunBeforeTheirTime) {
  Harness h(2);
  h.start();
  const int64_t t0 = now_ms();
  const std::string id = h.submit("noop", Json::object(), 0, 1, 0, /*delay_ms=*/200);
  ASSERT_FALSE(id.empty());
  EXPECT_EQ(h.state_of(id), JobState::Scheduled);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  EXPECT_FALSE(is_terminal(h.state_of(id))) << "the job ran before its delay elapsed";
  ASSERT_TRUE(h.wait_terminal(id, 3000));
  EXPECT_GE(now_ms() - t0, 200);
  EXPECT_EQ(h.state_of(id), JobState::Succeeded);
}

// The central correctness property of the engine: under many producers and many workers,
// every submitted job runs exactly once and reaches a terminal state.
TEST(EngineTest, NoJobIsLostOrExecutedTwiceUnderConcurrency) {
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 250;
  constexpr int kTotal = kProducers * kPerProducer;

  Harness h(6);
  std::mutex m;
  std::vector<std::string> executions;
  h.handlers.register_handler("counted", [&](const JobContext& ctx) {
    std::lock_guard<std::mutex> lock(m);
    executions.push_back(ctx.job_id());
    return JobOutcome::success();
  });
  h.start();

  std::vector<std::vector<std::string>> submitted(kProducers);
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < kPerProducer; ++i) {
        SubmitRequest req;
        req.type = "counted";
        req.priority = i % 4;
        std::string id, err;
        ASSERT_EQ(h.engine->submit(req, id, err), SubmitStatus::Ok) << err;
        submitted[static_cast<std::size_t>(p)].push_back(id);
      }
    });
  }
  for (auto& t : producers) t.join();
  ASSERT_TRUE(h.engine->wait_idle(30000)) << "engine never drained";

  std::set<std::string> submitted_ids;
  for (const auto& v : submitted) {
    for (const std::string& id : v) submitted_ids.insert(id);
  }
  ASSERT_EQ(submitted_ids.size(), static_cast<std::size_t>(kTotal));

  std::set<std::string> executed_ids;
  for (const std::string& id : executions) {
    EXPECT_TRUE(executed_ids.insert(id).second) << "job executed twice: " << id;
  }
  EXPECT_EQ(executions.size(), static_cast<std::size_t>(kTotal)) << "jobs were lost";
  EXPECT_EQ(executed_ids, submitted_ids);

  h.store->flush();
  EXPECT_EQ(h.store->count_jobs(JobState::Succeeded), kTotal);
  EXPECT_EQ(h.metrics.jobs_succeeded.load(), static_cast<uint64_t>(kTotal));
  EXPECT_EQ(h.engine->outstanding(), 0u);
  EXPECT_EQ(h.engine->queue_depth(), 0u);
}

TEST(EngineTest, GracefulShutdownWithJobsInFlightJoinsEveryThread) {
  Harness h(4);
  std::atomic<int> started{0};
  std::atomic<int> finished{0};
  h.handlers.register_handler("slowish", [&](const JobContext& ctx) {
    started.fetch_add(1);
    ctx.sleep_or_cancel(150);
    finished.fetch_add(1);
    return JobOutcome::success();
  });
  h.start();
  for (int i = 0; i < 40; ++i) ASSERT_FALSE(h.submit("slowish").empty());
  // Wait until work is genuinely in flight before shutting down.
  ASSERT_TRUE(wait_until([&] { return started.load() >= 4; }, 3000));

  const int64_t t0 = now_ms();
  h.engine->stop(/*drain=*/false);
  const int64_t elapsed = now_ms() - t0;

  EXPECT_LT(elapsed, 3000) << "shutdown did not complete within its bound";
  EXPECT_EQ(started.load(), finished.load())
      << "a handler was abandoned mid-execution instead of being allowed to return";
  // Jobs that never started stay QUEUED and are recoverable; nothing is left RUNNING.
  h.store->flush();
  EXPECT_EQ(h.store->count_jobs(JobState::Running), 0)
      << "a job was left RUNNING after shutdown";
  const int64_t accounted = h.store->count_jobs(JobState::Succeeded) +
                            h.store->count_jobs(JobState::Queued) +
                            h.store->count_jobs(JobState::Scheduled);
  EXPECT_EQ(accounted, 40) << "jobs went missing across shutdown";
}

TEST(EngineTest, DrainingShutdownFinishesQueuedWork) {
  Harness h(4);
  h.start();
  for (int i = 0; i < 30; ++i) ASSERT_FALSE(h.submit("sleep", Json::parse_or_null(R"({"ms":5})")).empty());
  h.engine->stop(/*drain=*/true);
  h.store->flush();
  EXPECT_EQ(h.store->count_jobs(JobState::Succeeded), 30);
  EXPECT_EQ(h.store->count_jobs(JobState::Queued), 0);
}

TEST(EngineTest, RecoveryRequeuesInterruptedJobsAfterRestart) {
  StoreConfig sc;
  sc.path = ":memory:";
  std::string err;
  auto store = JobStore::open(sc, err);
  ASSERT_NE(store, nullptr);
  HandlerRegistry handlers;
  HandlerRegistry::register_builtins(handlers);
  Metrics metrics;

  // Simulate the state a crash would leave behind: one RUNNING, one QUEUED, one done.
  Job running;
  running.job_id = "j_running";
  running.type = "noop";
  running.state = JobState::Running;
  running.attempt = 1;
  running.retry.max_attempts = 3;
  running.created_at_ms = wall_ms();
  running.lease_owner = "dead-worker";
  running.lease_token = 12345;
  ASSERT_TRUE(store->insert_job(running, err));

  Job queued = running;
  queued.job_id = "j_queued";
  queued.state = JobState::Queued;
  queued.attempt = 0;
  queued.lease_token = 0;
  ASSERT_TRUE(store->insert_job(queued, err));

  Job done = running;
  done.job_id = "j_done";
  done.state = JobState::Succeeded;
  ASSERT_TRUE(store->insert_job(done, err));

  EngineConfig cfg;
  cfg.worker_threads = 2;
  JobEngine engine(cfg, store.get(), &handlers, &metrics);
  EXPECT_EQ(engine.recover(), 2) << "only non-terminal jobs should be recovered";
  engine.start();
  ASSERT_TRUE(engine.wait_idle(10000));
  engine.stop(false);
  store->flush();

  Job got;
  ASSERT_TRUE(store->get_job("j_running", got));
  EXPECT_EQ(got.state, JobState::Succeeded);
  EXPECT_EQ(got.attempt, 2) << "the interrupted attempt must still count (at-least-once)";
  EXPECT_EQ(got.lease_token, 0u) << "a recovered job must not keep a dead worker's lease";
  ASSERT_TRUE(store->get_job("j_queued", got));
  EXPECT_EQ(got.state, JobState::Succeeded);
  ASSERT_TRUE(store->get_job("j_done", got));
  EXPECT_EQ(got.state, JobState::Succeeded);
  EXPECT_EQ(got.attempt, 1) << "a terminal job must not be re-executed";
}

TEST(EngineTest, MetricsTrackTheLifecycle) {
  Harness h(2);
  h.start();
  ASSERT_FALSE(h.submit("noop").empty());
  const std::string failing = h.submit("fail");
  ASSERT_FALSE(failing.empty());
  ASSERT_TRUE(h.engine->wait_idle(5000));
  EXPECT_EQ(h.metrics.jobs_submitted.load(), 2u);
  EXPECT_EQ(h.metrics.jobs_started.load(), 2u);
  EXPECT_EQ(h.metrics.attempts_started.load(), 2u);
  EXPECT_EQ(h.metrics.jobs_succeeded.load(), 1u);
  EXPECT_EQ(h.metrics.jobs_failed.load(), 1u);
  EXPECT_GT(h.metrics.exec.count(), 0u);
}

TEST(EngineTest, CompletionCallbackFiresExactlyOncePerJob) {
  Harness h(4);
  std::atomic<int> callbacks{0};
  std::mutex m;
  std::set<std::string> seen;
  h.engine->set_completion_callback([&](const Job& j) {
    callbacks.fetch_add(1);
    std::lock_guard<std::mutex> lock(m);
    EXPECT_TRUE(seen.insert(j.job_id).second) << "completion reported twice for " << j.job_id;
    EXPECT_TRUE(is_terminal(j.state));
  });
  h.start();
  for (int i = 0; i < 200; ++i) ASSERT_FALSE(h.submit("noop").empty());
  ASSERT_TRUE(h.engine->wait_idle(20000));
  EXPECT_EQ(callbacks.load(), 200);
  EXPECT_EQ(seen.size(), 200u);
}
