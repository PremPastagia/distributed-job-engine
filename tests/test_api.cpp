#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "test_helpers.hpp"

using namespace je;
using namespace je::test;

namespace {

std::string submit_body(const std::string& type, const std::string& extra = "") {
  return std::string("{\"type\":\"") + type + "\"" + (extra.empty() ? "" : "," + extra) + "}";
}

}  // namespace

TEST(ApiTest, HealthReportsTheEngineState) {
  ServerHarness s(3);
  auto c = s.client();
  const HttpClientResponse res = c.get("/health");
  ASSERT_TRUE(res.ok) << res.error;
  EXPECT_EQ(res.status, 200);
  const Json j = body_json(res);
  EXPECT_EQ(j.get_string("status", ""), "ok");
  EXPECT_EQ(j.get_int("worker_threads", -1), 3);
  EXPECT_GE(j.get_int("uptime_ms", -1), 0);
  EXPECT_EQ(j.get_int("queue_depth", -1), 0);
  ASSERT_NE(j.find("job_types"), nullptr);
  EXPECT_GT(j.find("job_types")->size(), 0u) << "built-in handlers should be advertised";
}

TEST(ApiTest, SubmitsAJobAndReadsItBack) {
  ServerHarness s(2);
  auto c = s.client();
  const HttpClientResponse created =
      c.post("/jobs", submit_body("echo", "\"payload\":{\"x\":1},\"priority\":4"));
  ASSERT_TRUE(created.ok) << created.error;
  ASSERT_EQ(created.status, 201);
  const std::string id = body_json(created).get_string("job_id", "");
  ASSERT_FALSE(id.empty());

  ASSERT_TRUE(wait_until([&] {
    HttpClient poll("127.0.0.1", s.port());
    return body_json(poll.get("/jobs/" + id)).get_string("state", "") == "SUCCEEDED";
  }, 5000));

  const HttpClientResponse got = c.get("/jobs/" + id);
  ASSERT_EQ(got.status, 200);
  const Json j = body_json(got);
  EXPECT_EQ(j.get_string("job_id", ""), id);
  EXPECT_EQ(j.get_string("type", ""), "echo");
  EXPECT_EQ(j.get_int("priority", -1), 4);
  EXPECT_EQ(j.get_int("attempt", -1), 1);
  EXPECT_EQ(j.find("result")->find("echo")->get_int("x", 0), 1);
}

TEST(ApiTest, RejectsInvalidSubmissionsWithUsefulErrors) {
  ServerHarness s(1);
  auto c = s.client();
  struct Case {
    std::string body;
    int status;
    std::string message_fragment;
  };
  const std::vector<Case> cases = {
      {"", 400, "required"},
      {"not json", 400, "invalid JSON"},
      {"[1,2,3]", 400, "must be a JSON object"},
      {"{}", 400, "'type' is required"},
      {submit_body("no_such_type"), 400, "unknown job type"},
      {submit_body("noop", "\"payload\":5"), 400, "'payload' must be an object"},
      {submit_body("noop", "\"max_attempts\":0"), 400, "max_attempts"},
      {submit_body("noop", "\"max_attempts\":1000"), 400, "max_attempts"},
      {submit_body("noop", "\"timeout_ms\":-1"), 400, "timeout_ms"},
      {submit_body("noop", "\"delay_ms\":-5"), 400, "delay_ms"},
      {submit_body("noop", "\"backoff_multiplier\":0.5"), 400, "backoff_multiplier"},
      {submit_body("noop", "\"backoff_base_ms\":-1"), 400, "negative"},
  };
  for (const Case& tc : cases) {
    const HttpClientResponse res = c.post("/jobs", tc.body);
    ASSERT_TRUE(res.ok) << tc.body << ": " << res.error;
    EXPECT_EQ(res.status, tc.status) << "body: " << tc.body;
    EXPECT_NE(body_json(res).get_string("error", "").find(tc.message_fragment),
              std::string::npos)
        << "body: " << tc.body << " -> " << res.body;
  }
  s.engine_harness.store->flush();
  EXPECT_EQ(s.store()->count_jobs(std::nullopt), 0) << "a rejected request must create no job";
}

TEST(ApiTest, UnknownTypeErrorListsTheKnownTypes) {
  ServerHarness s(1);
  auto c = s.client();
  const HttpClientResponse res = c.post("/jobs", submit_body("nope"));
  ASSERT_EQ(res.status, 400);
  const Json j = body_json(res);
  ASSERT_NE(j.find("known_types"), nullptr);
  EXPECT_GT(j.find("known_types")->size(), 5u);
}

TEST(ApiTest, MissingJobReturns404) {
  ServerHarness s(1);
  auto c = s.client();
  const HttpClientResponse res = c.get("/jobs/j_DOES_NOT_EXIST");
  ASSERT_TRUE(res.ok);
  EXPECT_EQ(res.status, 404);
  EXPECT_NE(body_json(res).get_string("error", "").find("not found"), std::string::npos);
  EXPECT_EQ(c.get("/jobs/j_X/attempts").status, 404);
}

TEST(ApiTest, UnknownEndpointsAndWrongMethodsAreDistinguished) {
  ServerHarness s(1);
  auto c = s.client();
  const HttpClientResponse missing = c.get("/no/such/thing");
  EXPECT_EQ(missing.status, 404);
  EXPECT_NE(body_json(missing).get_string("error", "").find("no such endpoint"),
            std::string::npos);

  const HttpClientResponse wrong_method = c.post("/health", "{}");
  EXPECT_EQ(wrong_method.status, 405);
  EXPECT_NE(body_json(wrong_method).get_string("error", "").find("not allowed"),
            std::string::npos);
}

TEST(ApiTest, ListsJobsWithFilteringAndPagination) {
  ServerHarness s(0);  // no workers: every job stays QUEUED, so the filter is deterministic
  auto c = s.client();
  for (int i = 0; i < 7; ++i) {
    ASSERT_EQ(c.post("/jobs", submit_body("noop")).status, 201);
  }
  const Json all = body_json(c.get("/jobs"));
  EXPECT_EQ(all.get_int("count", -1), 7);
  EXPECT_EQ(all.get_int("total", -1), 7);

  const Json queued = body_json(c.get("/jobs?state=QUEUED"));
  EXPECT_EQ(queued.get_int("count", -1), 7);
  const Json done = body_json(c.get("/jobs?state=SUCCEEDED"));
  EXPECT_EQ(done.get_int("count", -1), 0);

  const Json page = body_json(c.get("/jobs?limit=3&offset=0"));
  EXPECT_EQ(page.get_int("count", -1), 3);
  EXPECT_EQ(page.get_int("total", -1), 7) << "total must count all matches, not just the page";

  EXPECT_EQ(c.get("/jobs?state=NOT_A_STATE").status, 400);
  EXPECT_EQ(c.get("/jobs?limit=0").status, 400);
  EXPECT_EQ(c.get("/jobs?limit=99999").status, 400);
  EXPECT_EQ(c.get("/jobs?offset=-1").status, 400);
}

TEST(ApiTest, CancelsAQueuedJobAndRejectsCancellingATerminalOne) {
  ServerHarness s(0);
  auto c = s.client();
  const std::string id = body_json(c.post("/jobs", submit_body("noop"))).get_string("job_id", "");
  ASSERT_FALSE(id.empty());

  const HttpClientResponse cancelled = c.post("/jobs/" + id + "/cancel", "");
  ASSERT_EQ(cancelled.status, 200) << cancelled.body;
  EXPECT_EQ(body_json(cancelled).get_string("state", ""), "CANCELLED");

  const HttpClientResponse again = c.post("/jobs/" + id + "/cancel", "");
  EXPECT_EQ(again.status, 409) << "cancelling a terminal job must conflict, not succeed twice";
  EXPECT_EQ(c.post("/jobs/j_MISSING/cancel", "").status, 404);
}

TEST(ApiTest, ReportsAttemptHistoryForARetriedJob) {
  ServerHarness s(2);
  auto c = s.client();
  const std::string id =
      body_json(c.post("/jobs",
                       submit_body("fail",
                                   "\"payload\":{\"fail_until_attempt\":3},\"max_attempts\":4,"
                                   "\"backoff_base_ms\":5,\"backoff_multiplier\":1.0")))
          .get_string("job_id", "");
  ASSERT_FALSE(id.empty());
  ASSERT_TRUE(wait_until([&] {
    HttpClient poll("127.0.0.1", s.port());
    return body_json(poll.get("/jobs/" + id)).get_string("state", "") == "SUCCEEDED";
  }, 10000));

  const Json attempts = body_json(c.get("/jobs/" + id + "/attempts"));
  ASSERT_NE(attempts.find("attempts"), nullptr);
  ASSERT_EQ(attempts.find("attempts")->size(), 3u);
  EXPECT_EQ(attempts.find("attempts")->at(0).get_string("status", ""), "FAILED");
  EXPECT_EQ(attempts.find("attempts")->at(2).get_string("status", ""), "SUCCEEDED");
}

TEST(ApiTest, BackpressureIsReportedAs503) {
  EngineConfig cfg;
  cfg.max_outstanding_jobs = 4;
  ServerHarness s(0, cfg);
  auto c = s.client();
  int created = 0, rejected = 0;
  for (int i = 0; i < 12; ++i) {
    const int status = c.post("/jobs", submit_body("noop")).status;
    if (status == 201) ++created;
    else if (status == 503) ++rejected;
    else FAIL() << "unexpected status " << status;
  }
  EXPECT_EQ(created, 4);
  EXPECT_EQ(rejected, 8);
}

TEST(ApiTest, CreatesAndReportsAWorkflow) {
  ServerHarness s(4);
  auto c = s.client();
  const std::string body = R"({
    "name":"ingest",
    "nodes":[{"name":"a","type":"noop"},{"name":"b","type":"noop"},{"name":"c","type":"noop"}],
    "edges":[{"from":"a","to":"b"},{"from":"b","to":"c"}]
  })";
  const HttpClientResponse created = c.post("/workflows", body);
  ASSERT_EQ(created.status, 201) << created.body;
  const Json cj = body_json(created);
  const std::string wf_id = cj.get_string("workflow_id", "");
  ASSERT_FALSE(wf_id.empty());
  ASSERT_NE(cj.find("jobs"), nullptr);
  EXPECT_EQ(cj.find("jobs")->size(), 3u);

  ASSERT_TRUE(wait_until([&] {
    HttpClient poll("127.0.0.1", s.port());
    return body_json(poll.get("/workflows/" + wf_id)).get_string("state", "") == "SUCCEEDED";
  }, 15000));

  const Json wf = body_json(c.get("/workflows/" + wf_id));
  EXPECT_EQ(wf.get_string("name", ""), "ingest");
  EXPECT_EQ(wf.get_int("node_count", -1), 3);
  ASSERT_NE(wf.find("nodes"), nullptr);
  EXPECT_EQ(wf.find("nodes")->size(), 3u);

  EXPECT_EQ(body_json(c.get("/workflows")).get_int("count", -1), 1);
  EXPECT_EQ(c.get("/workflows/w_MISSING").status, 404);
}

TEST(ApiTest, RejectsInvalidWorkflowsWithAReason) {
  ServerHarness s(2);
  auto c = s.client();
  struct Case { std::string body; std::string reason; };
  const std::vector<Case> cases = {
      {R"({"nodes":[]})", ""},
      {R"({"nodes":[{"name":"a","type":"noop"}],"edges":[{"from":"a","to":"a"}]})", "self_edge"},
      {R"({"nodes":[{"name":"a","type":"noop"}],"edges":[{"from":"a","to":"ghost"}]})",
       "unknown_node_reference"},
      {R"({"nodes":[{"name":"a","type":"noop"},{"name":"a","type":"noop"}]})",
       "duplicate_node_name"},
      {R"({"nodes":[{"name":"a","type":"noop"},{"name":"b","type":"noop"}],
          "edges":[{"from":"a","to":"b"},{"from":"b","to":"a"}]})", "cycle_detected"},
      {R"({"nodes":[{"name":"a","type":"ghost_type"}]})", "unknown_type"},
      {R"({"nodes":[{"type":"noop"}]})", ""},
  };
  for (const Case& tc : cases) {
    const HttpClientResponse res = c.post("/workflows", tc.body);
    ASSERT_TRUE(res.ok);
    EXPECT_EQ(res.status, 400) << tc.body;
    if (!tc.reason.empty()) {
      EXPECT_EQ(body_json(res).get_string("reason", ""), tc.reason) << res.body;
    }
  }
  s.engine_harness.store->flush();
  EXPECT_EQ(s.store()->count_jobs(std::nullopt), 0);
}

TEST(ApiTest, CancelsAWorkflow) {
  ServerHarness s(0);
  auto c = s.client();
  const std::string body =
      R"({"name":"x","nodes":[{"name":"a","type":"noop"},{"name":"b","type":"noop"}],
          "edges":[{"from":"a","to":"b"}]})";
  const std::string wf_id = body_json(c.post("/workflows", body)).get_string("workflow_id", "");
  ASSERT_FALSE(wf_id.empty());
  EXPECT_EQ(c.post("/workflows/" + wf_id + "/cancel", "").status, 200);
  EXPECT_EQ(body_json(c.get("/workflows/" + wf_id)).get_string("state", ""), "CANCELLED");
  EXPECT_EQ(c.post("/workflows/" + wf_id + "/cancel", "").status, 409);
  EXPECT_EQ(c.post("/workflows/w_MISSING/cancel", "").status, 404);
}

TEST(ApiTest, MetricsAndRoutesAreExposed) {
  ServerHarness s(2);
  auto c = s.client();
  ASSERT_EQ(c.post("/jobs", submit_body("noop")).status, 201);
  ASSERT_TRUE(s.engine()->wait_idle(5000));

  const Json m = body_json(c.get("/metrics"));
  ASSERT_NE(m.find("counters"), nullptr);
  EXPECT_GE(m.find("counters")->get_int("jobs_submitted", 0), 1);
  EXPECT_GE(m.find("counters")->get_int("api_requests", 0), 1);
  ASSERT_NE(m.find("latency"), nullptr);
  ASSERT_NE(m.find("jobs_by_state"), nullptr);
  EXPECT_EQ(m.find("jobs_by_state")->get_int("SUCCEEDED", 0), 1);

  const Json routes = body_json(c.get("/routes"));
  ASSERT_NE(routes.find("routes"), nullptr);
  EXPECT_GE(routes.find("routes")->size(), 10u);
}

TEST(ApiTest, ConcurrentSubmissionsAllSucceedWithUniqueIds) {
  ServerHarness s(4, EngineConfig(), 8);
  constexpr int kThreads = 8;
  constexpr int kPerThread = 40;
  std::mutex m;
  std::set<std::string> ids;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      HttpClient c("127.0.0.1", s.port(), 20000);
      for (int i = 0; i < kPerThread; ++i) {
        const HttpClientResponse res = c.post("/jobs", submit_body("noop"));
        if (!res.ok || res.status != 201) { failures.fetch_add(1); continue; }
        const std::string id = body_json(res).get_string("job_id", "");
        std::lock_guard<std::mutex> lock(m);
        if (!ids.insert(id).second) failures.fetch_add(1);
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(ids.size(), static_cast<std::size_t>(kThreads * kPerThread));
  ASSERT_TRUE(s.engine()->wait_idle(30000));
  s.engine_harness.store->flush();
  EXPECT_EQ(s.store()->count_jobs(JobState::Succeeded), kThreads * kPerThread);
}

TEST(ApiTest, RepeatedIdenticalSubmissionsCreateDistinctJobs) {
  // The API has no idempotency key, so two identical requests are two jobs. Asserting it
  // keeps the documented semantics honest rather than implying de-duplication.
  ServerHarness s(0);
  auto c = s.client();
  const std::string a = body_json(c.post("/jobs", submit_body("noop"))).get_string("job_id", "");
  const std::string b = body_json(c.post("/jobs", submit_body("noop"))).get_string("job_id", "");
  EXPECT_NE(a, b);
  EXPECT_EQ(body_json(c.get("/jobs")).get_int("total", -1), 2);
}
