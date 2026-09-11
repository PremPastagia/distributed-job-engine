#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "jobengine/store.hpp"
#include "jobengine/util.hpp"

using namespace je;

namespace {

std::unique_ptr<JobStore> open_memory(bool group_commit = true) {
  StoreConfig cfg;
  cfg.path = ":memory:";
  cfg.reader_connections = 2;
  cfg.group_commit = group_commit;
  std::string err;
  auto s = JobStore::open(cfg, err);
  EXPECT_NE(s, nullptr) << err;
  return s;
}

Job make_job(const std::string& id, JobState state = JobState::Queued, int priority = 0) {
  Job j;
  j.job_id = id;
  j.type = "noop";
  j.payload = Json::parse_or_null(R"({"k":"v"})");
  j.priority = priority;
  j.state = state;
  j.retry.max_attempts = 3;
  j.timeout_ms = 1234;
  j.created_at_ms = wall_ms();
  j.seq = 1;
  return j;
}

struct TempDb {
  std::string path;
  TempDb() : path("/tmp/jobengine_test_" + new_id('t') + ".db") {}
  ~TempDb() {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
  }
};

}  // namespace

TEST(StoreTest, InsertAndReadBackEveryField) {
  auto store = open_memory();
  Job j = make_job("j_1", JobState::Running, 7);
  j.attempt = 2;
  j.error = "boom";
  j.workflow_id = "w_1";
  j.workflow_node = "extract";
  j.lease_owner = "worker-a";
  j.lease_token = 0xABCDEF0123456789ULL;
  j.queue_wait_us = 4321;
  j.exec_us = 8765;
  j.result = Json::parse_or_null(R"({"r":1})");
  std::string err;
  ASSERT_TRUE(store->insert_job(j, err)) << err;

  Job got;
  ASSERT_TRUE(store->get_job("j_1", got));
  EXPECT_EQ(got.job_id, j.job_id);
  EXPECT_EQ(got.type, j.type);
  EXPECT_EQ(got.priority, 7);
  EXPECT_EQ(got.state, JobState::Running);
  EXPECT_EQ(got.attempt, 2);
  EXPECT_EQ(got.retry.max_attempts, 3);
  EXPECT_EQ(got.timeout_ms, 1234);
  EXPECT_EQ(got.error, "boom");
  EXPECT_EQ(got.workflow_id, "w_1");
  EXPECT_EQ(got.workflow_node, "extract");
  EXPECT_EQ(got.lease_owner, "worker-a");
  EXPECT_EQ(got.lease_token, j.lease_token) << "a 64-bit lease token must survive storage";
  EXPECT_EQ(got.queue_wait_us, 4321);
  EXPECT_EQ(got.exec_us, 8765);
  EXPECT_EQ(got.payload.get_string("k", ""), "v");
  EXPECT_EQ(got.result.get_int("r", 0), 1);
}

TEST(StoreTest, MissingJobIsReportedNotFabricated) {
  auto store = open_memory();
  Job got;
  EXPECT_FALSE(store->get_job("does-not-exist", got));
}

TEST(StoreTest, UpdateReplacesTheRow) {
  auto store = open_memory();
  Job j = make_job("j_2");
  std::string err;
  ASSERT_TRUE(store->insert_job(j, err));
  j.state = JobState::Succeeded;
  j.attempt = 1;
  j.result = Json::parse_or_null(R"({"done":true})");
  ASSERT_TRUE(store->update_job(j, err));

  Job got;
  ASSERT_TRUE(store->get_job("j_2", got));
  EXPECT_EQ(got.state, JobState::Succeeded);
  EXPECT_TRUE(got.result.get_bool("done", false));
  EXPECT_EQ(store->count_jobs(std::nullopt), 1) << "update must not create a second row";
}

TEST(StoreTest, GroupCommitMakesAWriteVisibleImmediately) {
  auto store = open_memory(true);
  Job j = make_job("j_rt");
  std::string err;
  ASSERT_TRUE(store->insert_job(j, err));
  // Transition writes are buffered, so this exercises the read-through cache: the new
  // state must be visible to a reader before the batch reaches the database.
  for (int i = 0; i < 200; ++i) {
    j.attempt = i;
    j.state = (i % 2 == 0) ? JobState::Running : JobState::Retrying;
    ASSERT_TRUE(store->update_job(j, err));
    Job got;
    ASSERT_TRUE(store->get_job("j_rt", got));
    EXPECT_EQ(got.attempt, i);
    EXPECT_EQ(got.state, j.state);
  }
  store->flush();
  Job final_row;
  ASSERT_TRUE(store->get_job("j_rt", final_row));
  EXPECT_EQ(final_row.attempt, 199);
}

TEST(StoreTest, ListingsFilterAndPaginate) {
  auto store = open_memory();
  std::string err;
  for (int i = 0; i < 10; ++i) {
    Job j = make_job("j_" + std::to_string(i),
                     i % 2 == 0 ? JobState::Queued : JobState::Succeeded);
    j.created_at_ms = 1000 + i;
    ASSERT_TRUE(store->insert_job(j, err));
  }
  EXPECT_EQ(store->count_jobs(std::nullopt), 10);
  EXPECT_EQ(store->count_jobs(JobState::Queued), 5);
  EXPECT_EQ(store->count_jobs(JobState::Succeeded), 5);
  EXPECT_EQ(store->count_jobs(JobState::Failed), 0);

  const auto queued = store->list_jobs(JobState::Queued, 50, 0);
  EXPECT_EQ(queued.size(), 5u);
  for (const Job& j : queued) EXPECT_EQ(j.state, JobState::Queued);

  const auto page1 = store->list_jobs(std::nullopt, 3, 0);
  const auto page2 = store->list_jobs(std::nullopt, 3, 3);
  ASSERT_EQ(page1.size(), 3u);
  ASSERT_EQ(page2.size(), 3u);
  EXPECT_NE(page1.front().job_id, page2.front().job_id);
  // Newest first.
  EXPECT_GT(page1.front().created_at_ms, page1.back().created_at_ms);

  const auto counts = store->count_by_state();
  EXPECT_EQ(counts.at("QUEUED"), 5);
  EXPECT_EQ(counts.at("SUCCEEDED"), 5);
}

TEST(StoreTest, AttemptsArePerAttemptAndIdempotent) {
  auto store = open_memory();
  std::string err;
  ASSERT_TRUE(store->insert_job(make_job("j_a"), err));
  for (int a = 1; a <= 3; ++a) {
    AttemptRecord rec;
    rec.job_id = "j_a";
    rec.attempt = a;
    rec.status = a < 3 ? "FAILED" : "SUCCEEDED";
    rec.started_ms = 100 * a;
    rec.finished_ms = 100 * a + 50;
    rec.duration_ms = 50;
    rec.worker = "w1";
    rec.error = a < 3 ? "nope" : "";
    ASSERT_TRUE(store->append_attempt(rec, err));
  }
  // Re-delivering the same attempt must not create a fourth row.
  AttemptRecord dup;
  dup.job_id = "j_a";
  dup.attempt = 2;
  dup.status = "FAILED";
  dup.duration_ms = 50;
  ASSERT_TRUE(store->append_attempt(dup, err));

  const auto attempts = store->list_attempts("j_a");
  ASSERT_EQ(attempts.size(), 3u);
  EXPECT_EQ(attempts[0].attempt, 1);
  EXPECT_EQ(attempts[2].status, "SUCCEEDED");
  EXPECT_EQ(attempts[0].worker, "w1");
}

// Regression test for a flush-accounting bug: a batch containing only attempt rows was not
// counted as in flight, so flush() could return while that transaction was still open and a
// reader would see no attempts at all. Writing attempts without touching the job row makes
// that the only content of the batch.
TEST(StoreTest, FlushWaitsForAttemptOnlyBatches) {
  for (int round = 0; round < 40; ++round) {
    auto store = open_memory(true);
    std::string err;
    ASSERT_TRUE(store->insert_job(make_job("j_flush"), err));
    for (int a = 1; a <= 3; ++a) {
      AttemptRecord rec;
      rec.job_id = "j_flush";
      rec.attempt = a;
      rec.status = "FAILED";
      rec.duration_ms = 1;
      ASSERT_TRUE(store->append_attempt(rec, err));
    }
    // list_attempts flushes first, so it must observe every attempt already written.
    ASSERT_EQ(store->list_attempts("j_flush").size(), 3u)
        << "round " << round << ": flush returned before the attempt batch was committed";
  }
}

TEST(StoreTest, WorkflowInsertIsAtomicAcrossTables) {
  auto store = open_memory();
  WorkflowRecord wf;
  wf.workflow_id = "w_1";
  wf.name = "ingest";
  wf.state = "RUNNING";
  wf.created_at_ms = wall_ms();
  wf.node_count = 3;
  std::vector<Job> nodes = {make_job("j_a"), make_job("j_b"), make_job("j_c")};
  for (Job& n : nodes) n.workflow_id = "w_1";
  const std::vector<std::pair<std::string, std::string>> edges = {{"j_a", "j_b"},
                                                                  {"j_b", "j_c"}};
  std::string err;
  ASSERT_TRUE(store->insert_workflow(wf, nodes, edges, err)) << err;

  WorkflowRecord got;
  ASSERT_TRUE(store->get_workflow("w_1", got));
  EXPECT_EQ(got.name, "ingest");
  EXPECT_EQ(got.node_count, 3);
  EXPECT_EQ(store->list_workflow_jobs("w_1").size(), 3u);
  EXPECT_EQ(store->load_dependencies("w_1").size(), 2u);
  EXPECT_EQ(store->load_all_dependencies().size(), 2u);

  got.state = "SUCCEEDED";
  got.finished_at_ms = wall_ms();
  ASSERT_TRUE(store->update_workflow(got, err));
  WorkflowRecord again;
  ASSERT_TRUE(store->get_workflow("w_1", again));
  EXPECT_EQ(again.state, "SUCCEEDED");
  EXPECT_EQ(store->list_workflows(10).size(), 1u);
}

TEST(StoreTest, WorkflowInsertRollsBackOnConflict) {
  auto store = open_memory();
  std::string err;
  ASSERT_TRUE(store->insert_job(make_job("j_dup"), err));

  WorkflowRecord wf;
  wf.workflow_id = "w_2";
  wf.name = "bad";
  wf.state = "RUNNING";
  wf.created_at_ms = wall_ms();
  wf.node_count = 2;
  // The second node collides with the row already present, so the whole insert must fail.
  const std::vector<Job> nodes = {make_job("j_ok"), make_job("j_dup")};
  ASSERT_FALSE(store->insert_workflow(wf, nodes, {}, err));
  WorkflowRecord got;
  EXPECT_FALSE(store->get_workflow("w_2", got)) << "the workflow row must have rolled back";
  Job ignored;
  EXPECT_FALSE(store->get_job("j_ok", ignored)) << "the partial node insert must have rolled back";
}

TEST(StoreTest, RecoveryListOnlyReturnsNonTerminalJobs) {
  auto store = open_memory();
  std::string err;
  const std::vector<JobState> states = {
      JobState::Pending, JobState::Queued,  JobState::Scheduled, JobState::Running,
      JobState::Retrying, JobState::Succeeded, JobState::Failed, JobState::TimedOut,
      JobState::Cancelled, JobState::Skipped};
  for (std::size_t i = 0; i < states.size(); ++i) {
    Job j = make_job("j_" + std::to_string(i), states[i], static_cast<int>(i));
    ASSERT_TRUE(store->insert_job(j, err));
  }
  const auto pending = store->load_non_terminal_jobs();
  EXPECT_EQ(pending.size(), 5u);
  for (const Job& j : pending) EXPECT_FALSE(is_terminal(j.state)) << to_string(j.state);
  // Recovery order is the scheduling order: priority descending, then seq.
  for (std::size_t i = 1; i < pending.size(); ++i) {
    EXPECT_GE(pending[i - 1].priority, pending[i].priority);
  }
}

TEST(StoreTest, WorkerRecordsRegisterHeartbeatAndDie) {
  auto store = open_memory();
  std::string err;
  WorkerRecord w;
  w.worker_id = "k_1";
  w.name = "worker-a";
  w.capacity = 4;
  w.state = "ALIVE";
  w.registered_ms = 1000;
  w.last_heartbeat_ms = 1000;
  ASSERT_TRUE(store->upsert_worker(w, err));
  ASSERT_TRUE(store->touch_worker("k_1", 2000, err));
  auto workers = store->list_workers();
  ASSERT_EQ(workers.size(), 1u);
  EXPECT_EQ(workers[0].last_heartbeat_ms, 2000);
  EXPECT_EQ(workers[0].state, "ALIVE");

  ASSERT_TRUE(store->set_worker_state("k_1", "DEAD", err));
  workers = store->list_workers();
  EXPECT_EQ(workers[0].state, "DEAD");
  EXPECT_EQ(workers[0].capacity, 4);
}

TEST(StoreTest, DataSurvivesReopeningTheFile) {
  TempDb db;
  {
    StoreConfig cfg;
    cfg.path = db.path;
    std::string err;
    auto store = JobStore::open(cfg, err);
    ASSERT_NE(store, nullptr) << err;
    Job j = make_job("j_persist", JobState::Running, 4);
    j.attempt = 2;
    ASSERT_TRUE(store->insert_job(j, err));
    AttemptRecord rec;
    rec.job_id = "j_persist";
    rec.attempt = 1;
    rec.status = "FAILED";
    rec.duration_ms = 12;
    ASSERT_TRUE(store->append_attempt(rec, err));
    ASSERT_TRUE(store->checkpoint(err));
  }
  {
    StoreConfig cfg;
    cfg.path = db.path;
    std::string err;
    auto store = JobStore::open(cfg, err);
    ASSERT_NE(store, nullptr) << err;
    Job got;
    ASSERT_TRUE(store->get_job("j_persist", got)) << "the row did not survive reopening";
    EXPECT_EQ(got.state, JobState::Running);
    EXPECT_EQ(got.attempt, 2);
    EXPECT_EQ(got.priority, 4);
    EXPECT_EQ(store->list_attempts("j_persist").size(), 1u);
    EXPECT_EQ(store->load_non_terminal_jobs().size(), 1u);
  }
}

TEST(StoreTest, ConcurrentWritersAndReadersStayConsistent) {
  auto store = open_memory();
  constexpr int kThreads = 6;
  constexpr int kPerThread = 300;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        const std::string id = "t" + std::to_string(t) + "_" + std::to_string(i);
        Job j = make_job(id);
        std::string err;
        if (!store->insert_job(j, err)) { failures.fetch_add(1); continue; }
        j.state = JobState::Running;
        j.attempt = 1;
        if (!store->update_job(j, err)) { failures.fetch_add(1); continue; }
        Job got;
        if (!store->get_job(id, got) || got.state != JobState::Running) {
          failures.fetch_add(1);
        }
        j.state = JobState::Succeeded;
        if (!store->update_job(j, err)) failures.fetch_add(1);
      }
    });
  }
  for (auto& th : threads) th.join();
  store->flush();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(store->count_jobs(std::nullopt), kThreads * kPerThread);
  EXPECT_EQ(store->count_jobs(JobState::Succeeded), kThreads * kPerThread);
}

TEST(StoreTest, TwoInMemoryStoresDoNotShareData) {
  auto a = open_memory();
  auto b = open_memory();
  std::string err;
  ASSERT_TRUE(a->insert_job(make_job("only_in_a"), err));
  Job got;
  EXPECT_TRUE(a->get_job("only_in_a", got));
  EXPECT_FALSE(b->get_job("only_in_a", got))
      << "each :memory: store must be an independent database";
  EXPECT_EQ(b->count_jobs(std::nullopt), 0);
}
