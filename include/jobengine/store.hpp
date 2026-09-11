#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

#include "jobengine/job.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace je {

struct WorkflowRecord {
  std::string workflow_id;
  std::string name;
  std::string state;  // RUNNING | SUCCEEDED | FAILED | CANCELLED
  int64_t created_at_ms = 0;
  int64_t finished_at_ms = 0;
  int node_count = 0;
};

struct WorkerRecord {
  std::string worker_id;
  std::string name;
  int capacity = 1;
  std::string state;  // ALIVE | DEAD
  int64_t registered_ms = 0;
  int64_t last_heartbeat_ms = 0;
  int64_t jobs_completed = 0;
};

struct AttemptRecord {
  std::string job_id;
  int attempt = 0;
  std::string status;
  int64_t started_ms = 0;
  int64_t finished_ms = 0;
  int64_t duration_ms = 0;
  std::string worker;
  std::string error;
};

struct StoreConfig {
  std::string path = "jobengine.db";  // ":memory:" is supported for tests
  bool synchronous_full = false;      // D-06: NORMAL by default in WAL mode
  int reader_connections = 4;         // WAL permits concurrent readers alongside one writer

  // Group commit (docs/DESIGN_DECISIONS.md D-14). SQLite runs every bare statement in its
  // own implicit transaction, so the original design paid four commits per job. With
  // group commit enabled, a single flusher thread drains whatever has accumulated and
  // writes it as ONE transaction:
  //   * insert_job / insert_jobs block until their batch is committed, so a client is
  //     still only acknowledged after the row is durable;
  //   * update_job / append_attempt return immediately and are read back through an
  //     in-memory cache, so a state transition is never visible as stale to a reader,
  //     but IS lost on a hard crash before the flush (at-least-once already covers this).
  bool group_commit = true;
  std::size_t max_batch = 1024;
  // Optional commit delay: after being woken, the flusher waits this long before taking
  // the batch, trading a little write latency for a larger transaction. 0 means "commit
  // as soon as there is work", which self-tunes because whatever arrives during one
  // commit becomes the next batch.
  int64_t commit_delay_us = 0;
  // High-water mark on buffered, not-yet-committed rows. Without a bound, a submit rate
  // above the commit rate would let the write buffer and its read-through cache grow
  // until the process ran out of memory. Past this mark a transition write waits briefly
  // for the flusher to catch up, which turns an unbounded memory growth into ordinary
  // backpressure on the producer.
  std::size_t max_buffered_rows = 20000;
};

// SQLite-backed metadata store.
//
// Threading: one dedicated writer connection behind a mutex (SQLite allows exactly one
// writer, so serialising here turns SQLITE_BUSY contention into ordinary lock waiting),
// plus a checkout pool of read-only connections so status lookups do not queue behind a
// write transaction. Each connection owns its own prepared-statement cache, because a
// prepared statement must not be stepped by two threads at once.
// A job row tagged with the monotonic version at which it was queued for writing.
struct VersionedJob {
  uint64_t version = 0;
  uint64_t insert_seq = 0;  // non-zero only for inserts, so waiters know when to wake
  Job job;
};

class JobStore {
 public:
  static std::unique_ptr<JobStore> open(const StoreConfig& cfg, std::string& err);
  ~JobStore();

  JobStore(const JobStore&) = delete;
  JobStore& operator=(const JobStore&) = delete;

  // --- jobs ---
  bool insert_job(const Job& job, std::string& err);
  bool insert_jobs(const std::vector<Job>& jobs, std::string& err);  // one transaction
  bool update_job(const Job& job, std::string& err);
  bool get_job(const std::string& job_id, Job& out) const;
  std::vector<Job> list_jobs(const std::optional<JobState>& state, int limit, int offset) const;
  std::vector<Job> list_workflow_jobs(const std::string& workflow_id) const;
  std::vector<Job> load_non_terminal_jobs() const;   // restart recovery
  int64_t count_jobs(const std::optional<JobState>& state) const;
  std::unordered_map<std::string, int64_t> count_by_state() const;

  // --- attempts ---
  bool append_attempt(const AttemptRecord& rec, std::string& err);
  std::vector<AttemptRecord> list_attempts(const std::string& job_id) const;

  // --- workflows ---
  bool insert_workflow(const WorkflowRecord& wf, const std::vector<Job>& nodes,
                       const std::vector<std::pair<std::string, std::string>>& edges,
                       std::string& err);
  bool update_workflow(const WorkflowRecord& wf, std::string& err);
  bool get_workflow(const std::string& workflow_id, WorkflowRecord& out) const;
  std::vector<WorkflowRecord> list_workflows(int limit) const;
  std::vector<std::pair<std::string, std::string>> load_dependencies(
      const std::string& workflow_id) const;
  std::vector<std::pair<std::string, std::string>> load_all_dependencies() const;

  // --- workers ---
  bool upsert_worker(const WorkerRecord& w, std::string& err);
  bool touch_worker(const std::string& worker_id, int64_t heartbeat_ms, std::string& err);
  bool set_worker_state(const std::string& worker_id, const std::string& state, std::string& err);
  std::vector<WorkerRecord> list_workers() const;

  // --- maintenance ---
  bool checkpoint(std::string& err);
  // Blocks until every buffered write has been committed. Called before any query that
  // must be exact across all jobs (listings, counts) and during shutdown.
  void flush();
  uint64_t batches_committed() const { return batches_committed_.load(); }
  uint64_t writes_committed() const { return writes_committed_.load(); }
  const StoreConfig& config() const { return cfg_; }

 private:
  struct Conn {
    sqlite3* db = nullptr;
    std::unordered_map<std::string, sqlite3_stmt*> cache;
    ~Conn();
    sqlite3_stmt* prepare(const std::string& sql, std::string& err);
  };

  // RAII checkout of a reader connection; blocks if every reader is busy.
  class Reader {
   public:
    Reader(const JobStore* store);
    ~Reader();
    Conn* operator->() const { return conn_; }
    Conn* get() const { return conn_; }

   private:
    const JobStore* store_;
    Conn* conn_;
    std::size_t index_;
  };
  friend class Reader;

  JobStore() = default;
  bool init_schema(std::string& err);
  static bool exec(sqlite3* db, const char* sql, std::string& err);
  static Job read_job_row(sqlite3_stmt* st);
  bool write_job_row(const Job& job, bool insert, std::string& err);
  bool write_job_row_locked(Conn& c, const Job& job, bool insert, std::string& err);

  void flusher_loop();
  void start_flusher();
  void stop_flusher();
  bool append_attempt_locked(const AttemptRecord& rec, std::string& err);
  bool append_attempt_direct(const AttemptRecord& rec, std::string& err);
  bool commit_batch(const std::vector<VersionedJob>& inserts,
                    const std::vector<VersionedJob>& updates,
                    const std::vector<AttemptRecord>& attempts, std::string& err);
  bool has_pending_locked() const {
    return !pending_inserts_.empty() || !pending_updates_.empty() || !pending_attempts_.empty();
  }

  StoreConfig cfg_;
  mutable Conn writer_;
  mutable std::mutex writer_mutex_;
  mutable std::vector<std::unique_ptr<Conn>> readers_;
  mutable std::vector<bool> reader_busy_;
  mutable std::mutex pool_mutex_;
  mutable std::condition_variable pool_cv_;

  // --- group-commit state ---
  std::thread flusher_;
  mutable std::mutex wb_mutex_;
  std::condition_variable wb_work_cv_;  // wakes the flusher
  std::condition_variable wb_done_cv_;  // wakes callers waiting for their rows to commit
  std::vector<VersionedJob> pending_inserts_;
  std::vector<VersionedJob> pending_updates_;  // arrival order; the newest write wins
  std::vector<AttemptRecord> pending_attempts_;
  // Read-through cache: job_id -> newest row not yet visible in the database. An entry is
  // dropped only when the exact version that was written is still the newest one, so a
  // write that lands during a flush is never discarded.
  std::unordered_map<std::string, VersionedJob> cache_;
  uint64_t version_counter_ = 0;
  uint64_t insert_seq_ = 0;            // increments once per insert_jobs() call
  uint64_t committed_insert_seq_ = 0;  // every insert batch up to here is durable
  std::size_t in_flight_ = 0;          // rows inside the transaction being committed
  std::string last_flush_error_;
  bool flusher_stop_ = false;
  std::atomic<uint64_t> batches_committed_{0};
  std::atomic<uint64_t> writes_committed_{0};
};

}  // namespace je
