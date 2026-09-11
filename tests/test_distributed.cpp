#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "jobengine/remote_worker.hpp"
#include "test_helpers.hpp"

using namespace je;
using namespace je::test;

namespace {

std::string register_worker(HttpClient& c, const std::string& name, int capacity = 2) {
  Json body = Json::object();
  body.set("name", Json(name));
  body.set("capacity", Json(static_cast<int64_t>(capacity)));
  const HttpClientResponse res = c.post("/internal/workers/register", body.dump());
  EXPECT_TRUE(res.ok) << res.error;
  EXPECT_EQ(res.status, 200);
  return body_json(res).get_string("worker_id", "");
}

Json claim(HttpClient& c, const std::string& worker_id, int max_jobs = 1, int64_t wait_ms = 200) {
  Json body = Json::object();
  body.set("worker_id", Json(worker_id));
  body.set("max_jobs", Json(static_cast<int64_t>(max_jobs)));
  body.set("wait_ms", Json(wait_ms));
  return body_json(c.post("/internal/claim", body.dump()));
}

Json complete(HttpClient& c, const std::string& worker_id, const std::string& job_id,
              int attempt, const std::string& token, const std::string& status,
              const std::string& error = "") {
  Json body = Json::object();
  body.set("worker_id", Json(worker_id));
  body.set("job_id", Json(job_id));
  body.set("attempt", Json(static_cast<int64_t>(attempt)));
  body.set("lease_token", Json(token));
  body.set("status", Json(status));
  body.set("result", Json::object());
  body.set("error", Json(error));
  return body_json(c.post("/internal/complete", body.dump()));
}

}  // namespace

TEST(DistributedTest, WorkerRegistersAndIsCountedAlive) {
  ServerHarness s(0);
  auto c = s.client();
  const std::string id = register_worker(c, "w1", 4);
  ASSERT_FALSE(id.empty());
  EXPECT_EQ(s.engine()->alive_workers(), 1u);
  EXPECT_EQ(body_json(c.get("/health")).get_int("remote_workers_alive", -1), 1);

  Json hb = Json::object();
  hb.set("worker_id", Json(id));
  const HttpClientResponse res = c.post("/internal/workers/heartbeat", hb.dump());
  EXPECT_EQ(res.status, 200);
  EXPECT_TRUE(body_json(res).get_bool("ok", false));

  hb.set("worker_id", Json(std::string("k_unknown")));
  EXPECT_EQ(c.post("/internal/workers/heartbeat", hb.dump()).status, 404);
  EXPECT_EQ(s.store()->list_workers().size(), 1u);
}

TEST(DistributedTest, ClaimLeasesAJobAndCompleteRecordsIt) {
  ServerHarness s(0);  // no embedded workers: every job must go to the remote worker
  auto c = s.client();
  const std::string worker = register_worker(c, "w1");
  const std::string job_id =
      body_json(c.post("/jobs", R"({"type":"noop"})")).get_string("job_id", "");
  ASSERT_FALSE(job_id.empty());

  const Json claimed = claim(c, worker, 1, 500);
  ASSERT_EQ(claimed.get_int("count", 0), 1) << "the queued job was not offered to the worker";
  const Json& j = claimed.find("jobs")->at(0);
  EXPECT_EQ(j.get_string("job_id", ""), job_id);
  EXPECT_EQ(j.get_string("type", ""), "noop");
  EXPECT_EQ(j.get_int("attempt", 0), 1);
  const std::string token = j.get_string("lease_token", "");
  EXPECT_FALSE(token.empty());
  EXPECT_NE(token, "0");
  EXPECT_GT(j.get_int("lease_expires_ms", 0), 0);

  EXPECT_EQ(body_json(c.get("/jobs/" + job_id)).get_string("state", ""), "RUNNING");

  const Json done = complete(c, worker, job_id, 1, token, "SUCCEEDED");
  EXPECT_TRUE(done.get_bool("accepted", false)) << done.dump();
  const Json after = body_json(c.get("/jobs/" + job_id));
  EXPECT_EQ(after.get_string("state", ""), "SUCCEEDED");
  EXPECT_EQ(after.get_string("lease_owner", ""), worker);
}

TEST(DistributedTest, ClaimReturnsNothingWhenTheQueueIsEmpty) {
  ServerHarness s(0);
  auto c = s.client();
  const std::string worker = register_worker(c, "w1");
  const int64_t t0 = now_ms();
  const Json claimed = claim(c, worker, 4, 150);
  EXPECT_EQ(claimed.get_int("count", -1), 0);
  EXPECT_GE(now_ms() - t0, 140) << "the long poll returned before its wait window elapsed";
}

TEST(DistributedTest, ClaimValidatesItsArguments) {
  ServerHarness s(0);
  auto c = s.client();
  const std::string worker = register_worker(c, "w1");
  Json body = Json::object();
  body.set("worker_id", Json(worker));
  body.set("max_jobs", Json(static_cast<int64_t>(0)));
  EXPECT_EQ(c.post("/internal/claim", body.dump()).status, 400);
  body.set("max_jobs", Json(static_cast<int64_t>(1)));
  body.set("wait_ms", Json(static_cast<int64_t>(-1)));
  EXPECT_EQ(c.post("/internal/claim", body.dump()).status, 400);

  Json bad = Json::object();
  bad.set("worker_id", Json(worker));
  bad.set("job_id", Json(std::string("j_x")));
  bad.set("status", Json(std::string("NOT_A_STATUS")));
  EXPECT_EQ(c.post("/internal/complete", bad.dump()).status, 400);
}

TEST(DistributedTest, AResultCarryingAStaleLeaseTokenIsRejected) {
  ServerHarness s(0);
  auto c = s.client();
  const std::string worker = register_worker(c, "w1");
  const std::string job_id =
      body_json(c.post("/jobs", R"({"type":"noop"})")).get_string("job_id", "");
  const Json claimed = claim(c, worker, 1, 500);
  ASSERT_EQ(claimed.get_int("count", 0), 1);
  const std::string real_token = claimed.find("jobs")->at(0).get_string("lease_token", "");

  // Wrong token, wrong worker and wrong attempt must each be refused.
  EXPECT_FALSE(complete(c, worker, job_id, 1, "999999", "SUCCEEDED").get_bool("accepted", true));
  EXPECT_FALSE(complete(c, "k_other", job_id, 1, real_token, "SUCCEEDED").get_bool("accepted", true));
  EXPECT_FALSE(complete(c, worker, job_id, 2, real_token, "SUCCEEDED").get_bool("accepted", true));
  EXPECT_EQ(body_json(c.get("/jobs/" + job_id)).get_string("state", ""), "RUNNING")
      << "a rejected result must not change the job";

  // The correct token still works, and only once.
  EXPECT_TRUE(complete(c, worker, job_id, 1, real_token, "SUCCEEDED").get_bool("accepted", false));
  const Json second = complete(c, worker, job_id, 1, real_token, "FAILED", "late");
  EXPECT_FALSE(second.get_bool("accepted", true)) << "a duplicate result must be refused";
  EXPECT_EQ(second.get_string("reason", ""), "stale_lease");
  EXPECT_EQ(body_json(c.get("/jobs/" + job_id)).get_string("state", ""), "SUCCEEDED")
      << "the duplicate must not overwrite the recorded result";
  EXPECT_GE(s.metrics().stale_results_rejected.load(), 4u);
}

TEST(DistributedTest, AnExpiredLeaseIsReclaimedAndReassigned) {
  EngineConfig cfg;
  cfg.lease_duration_ms = 150;            // expire quickly for the test
  cfg.worker_heartbeat_timeout_ms = 100000;  // isolate lease expiry from liveness detection
  cfg.watchdog_interval_ms = 10;
  ServerHarness s(0, cfg);
  auto c = s.client();
  const std::string worker_a = register_worker(c, "a");
  const std::string worker_b = register_worker(c, "b");
  const std::string job_id =
      body_json(c.post("/jobs", R"({"type":"noop","max_attempts":3})")).get_string("job_id", "");

  const Json first = claim(c, worker_a, 1, 500);
  ASSERT_EQ(first.get_int("count", 0), 1);
  const std::string token_a = first.find("jobs")->at(0).get_string("lease_token", "");

  // Worker A goes silent. Its lease must expire and the job must return to the queue.
  ASSERT_TRUE(wait_until([&] {
    return body_json(c.get("/jobs/" + job_id)).get_string("state", "") == "QUEUED";
  }, 5000)) << "the lease never expired";
  EXPECT_GE(s.metrics().leases_expired.load(), 1u);
  EXPECT_GE(s.metrics().jobs_reassigned.load(), 1u);

  // Worker B picks it up with a fresh attempt and a different token.
  const Json second = claim(c, worker_b, 1, 1000);
  ASSERT_EQ(second.get_int("count", 0), 1);
  const std::string token_b = second.find("jobs")->at(0).get_string("lease_token", "");
  EXPECT_NE(token_a, token_b);
  EXPECT_EQ(second.find("jobs")->at(0).get_int("attempt", 0), 2)
      << "a reassignment consumes an attempt, which bounds repeated reassignment";

  // Worker A finally reports: its result is fenced out.
  EXPECT_FALSE(complete(c, worker_a, job_id, 1, token_a, "SUCCEEDED").get_bool("accepted", true));
  EXPECT_TRUE(complete(c, worker_b, job_id, 2, token_b, "SUCCEEDED").get_bool("accepted", false));
  EXPECT_EQ(body_json(c.get("/jobs/" + job_id)).get_string("state", ""), "SUCCEEDED");
}

TEST(DistributedTest, ReassignmentStopsWhenAttemptsAreExhausted) {
  EngineConfig cfg;
  cfg.lease_duration_ms = 80;
  cfg.worker_heartbeat_timeout_ms = 100000;
  cfg.watchdog_interval_ms = 10;
  ServerHarness s(0, cfg);
  auto c = s.client();
  const std::string worker = register_worker(c, "a");
  const std::string job_id =
      body_json(c.post("/jobs", R"({"type":"noop","max_attempts":2})")).get_string("job_id", "");

  // Claim twice without ever reporting; the second expiry exhausts the attempt budget.
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(wait_until([&] { return claim(c, worker, 1, 200).get_int("count", 0) == 1; },
                           5000))
        << "claim " << i << " never succeeded";
  }
  ASSERT_TRUE(wait_until([&] {
    return body_json(c.get("/jobs/" + job_id)).get_string("state", "") == "FAILED";
  }, 5000)) << "an endlessly abandoned job must eventually stop being reassigned";
  const Json j = body_json(c.get("/jobs/" + job_id));
  EXPECT_NE(j.get_string("error", "").find("lease expired"), std::string::npos);
}

TEST(DistributedTest, AWorkerThatStopsHeartbeatingIsMarkedDeadAndItsWorkReturns) {
  EngineConfig cfg;
  cfg.worker_heartbeat_timeout_ms = 200;
  cfg.lease_duration_ms = 60000;  // long lease, so only liveness detection can reclaim
  cfg.watchdog_interval_ms = 10;
  ServerHarness s(0, cfg);
  auto c = s.client();
  const std::string worker = register_worker(c, "doomed");
  const std::string job_id =
      body_json(c.post("/jobs", R"({"type":"noop","max_attempts":3})")).get_string("job_id", "");
  ASSERT_EQ(claim(c, worker, 1, 500).get_int("count", 0), 1);
  EXPECT_EQ(body_json(c.get("/jobs/" + job_id)).get_string("state", ""), "RUNNING");

  ASSERT_TRUE(wait_until([&] { return s.engine()->alive_workers() == 0; }, 5000))
      << "a silent worker was never marked dead";
  EXPECT_GE(s.metrics().workers_marked_dead.load(), 1u);
  ASSERT_TRUE(wait_until([&] {
    return body_json(c.get("/jobs/" + job_id)).get_string("state", "") == "QUEUED";
  }, 5000)) << "a dead worker's job was not returned to the queue";

  s.engine_harness.store->flush();
  const auto workers = s.store()->list_workers();
  ASSERT_EQ(workers.size(), 1u);
  EXPECT_EQ(workers[0].state, "DEAD");
}

TEST(DistributedTest, HeartbeatDeliversCancellationRequestsToTheWorker) {
  EngineConfig cfg;
  cfg.worker_heartbeat_timeout_ms = 60000;
  cfg.lease_duration_ms = 60000;
  ServerHarness s(0, cfg);
  auto c = s.client();
  const std::string worker = register_worker(c, "w1");
  const std::string job_id =
      body_json(c.post("/jobs", R"({"type":"noop"})")).get_string("job_id", "");
  ASSERT_EQ(claim(c, worker, 1, 500).get_int("count", 0), 1);

  ASSERT_EQ(c.post("/jobs/" + job_id + "/cancel", "").status, 200);
  Json hb = Json::object();
  hb.set("worker_id", Json(worker));
  const Json reply = body_json(c.post("/internal/workers/heartbeat", hb.dump()));
  ASSERT_NE(reply.find("cancel"), nullptr);
  ASSERT_EQ(reply.find("cancel")->size(), 1u)
      << "the coordinator must tell the worker to stop the job";
  EXPECT_EQ(reply.find("cancel")->at(0).as_string(), job_id);
}

// End-to-end with the real RemoteWorker class driving the protocol.
TEST(DistributedTest, RealWorkerProcessesLogicCompletesEveryJob) {
  ServerHarness s(0);  // all execution happens in the worker
  HandlerRegistry worker_handlers;
  HandlerRegistry::register_builtins(worker_handlers);

  RemoteWorkerConfig wc;
  wc.host = "127.0.0.1";
  wc.port = s.port();
  wc.name = "remote-1";
  wc.threads = 3;
  wc.poll_wait_ms = 100;
  wc.heartbeat_ms = 200;
  RemoteWorker worker(wc, &worker_handlers);
  std::string err;
  ASSERT_TRUE(worker.start(err)) << err;

  auto c = s.client();
  constexpr int kJobs = 60;
  std::vector<std::string> ids;
  for (int i = 0; i < kJobs; ++i) {
    const std::string id =
        body_json(c.post("/jobs", R"({"type":"echo","payload":{"i":1}})")).get_string("job_id", "");
    ASSERT_FALSE(id.empty());
    ids.push_back(id);
  }

  ASSERT_TRUE(s.engine()->wait_idle(30000)) << "the coordinator never drained";
  worker.stop();
  s.engine_harness.store->flush();

  EXPECT_EQ(s.store()->count_jobs(JobState::Succeeded), kJobs);
  EXPECT_EQ(worker.jobs_completed(), static_cast<uint64_t>(kJobs));
  EXPECT_EQ(worker.results_rejected(), 0u);
  for (const std::string& id : ids) {
    Job job;
    ASSERT_TRUE(s.store()->get_job(id, job));
    EXPECT_EQ(job.state, JobState::Succeeded);
    EXPECT_FALSE(job.lease_owner.empty()) << "the executing worker should be recorded";
  }
}

TEST(DistributedTest, TwoWorkerProcessesShareTheWorkload) {
  ServerHarness s(0);
  HandlerRegistry ha, hb;
  HandlerRegistry::register_builtins(ha);
  HandlerRegistry::register_builtins(hb);

  RemoteWorkerConfig ca;
  ca.host = "127.0.0.1";
  ca.port = s.port();
  ca.name = "alpha";
  ca.threads = 2;
  ca.poll_wait_ms = 100;
  ca.heartbeat_ms = 200;
  RemoteWorkerConfig cb = ca;
  cb.name = "beta";

  RemoteWorker wa(ca, &ha), wb(cb, &hb);
  std::string err;
  ASSERT_TRUE(wa.start(err)) << err;
  ASSERT_TRUE(wb.start(err)) << err;
  EXPECT_EQ(s.engine()->alive_workers(), 2u);

  auto c = s.client();
  constexpr int kJobs = 80;
  for (int i = 0; i < kJobs; ++i) {
    ASSERT_EQ(c.post("/jobs", R"({"type":"sleep","payload":{"ms":5}})").status, 201);
  }
  ASSERT_TRUE(s.engine()->wait_idle(30000));
  wa.stop();
  wb.stop();
  s.engine_harness.store->flush();

  EXPECT_EQ(s.store()->count_jobs(JobState::Succeeded), kJobs);
  EXPECT_EQ(wa.jobs_completed() + wb.jobs_completed(), static_cast<uint64_t>(kJobs));
  // Both processes must have done real work, or "distributed" would be a claim about one
  // worker and one idle spectator.
  EXPECT_GT(wa.jobs_completed(), 0u);
  EXPECT_GT(wb.jobs_completed(), 0u);

  std::set<std::string> owners;
  for (const Job& j : s.store()->list_jobs(JobState::Succeeded, 200, 0)) {
    owners.insert(j.lease_owner);
  }
  EXPECT_EQ(owners.size(), 2u) << "work was not actually spread across both worker processes";
}

TEST(DistributedTest, EmbeddedAndRemoteWorkersCoexist) {
  ServerHarness s(2);  // two embedded threads plus one remote worker process
  HandlerRegistry remote_handlers;
  HandlerRegistry::register_builtins(remote_handlers);
  RemoteWorkerConfig wc;
  wc.host = "127.0.0.1";
  wc.port = s.port();
  wc.name = "remote";
  wc.threads = 2;
  wc.poll_wait_ms = 50;
  wc.heartbeat_ms = 200;
  RemoteWorker worker(wc, &remote_handlers);
  std::string err;
  ASSERT_TRUE(worker.start(err)) << err;

  auto c = s.client();
  constexpr int kJobs = 100;
  for (int i = 0; i < kJobs; ++i) {
    ASSERT_EQ(c.post("/jobs", R"({"type":"sleep","payload":{"ms":3}})").status, 201);
  }
  ASSERT_TRUE(s.engine()->wait_idle(30000));
  worker.stop();
  s.engine_harness.store->flush();
  EXPECT_EQ(s.store()->count_jobs(JobState::Succeeded), kJobs)
      << "some jobs were executed twice or lost when both worker kinds were active";
}
