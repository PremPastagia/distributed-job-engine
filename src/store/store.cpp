#include "jobengine/store.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "jobengine/util.hpp"

namespace je {
namespace {

const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS jobs (
  job_id           TEXT PRIMARY KEY,
  type             TEXT NOT NULL,
  payload          TEXT NOT NULL DEFAULT '{}',
  priority         INTEGER NOT NULL DEFAULT 0,
  state            TEXT NOT NULL,
  attempt          INTEGER NOT NULL DEFAULT 0,
  max_attempts     INTEGER NOT NULL DEFAULT 1,
  timeout_ms       INTEGER NOT NULL DEFAULT 0,
  backoff_base_ms  INTEGER NOT NULL DEFAULT 100,
  backoff_mult     REAL    NOT NULL DEFAULT 2.0,
  backoff_max_ms   INTEGER NOT NULL DEFAULT 30000,
  backoff_jitter   INTEGER NOT NULL DEFAULT 0,
  created_at_ms    INTEGER NOT NULL,
  started_at_ms    INTEGER NOT NULL DEFAULT 0,
  finished_at_ms   INTEGER NOT NULL DEFAULT 0,
  ready_at_ms      INTEGER NOT NULL DEFAULT 0,
  queue_wait_us    INTEGER NOT NULL DEFAULT 0,
  exec_us          INTEGER NOT NULL DEFAULT 0,
  result           TEXT,
  error            TEXT,
  workflow_id      TEXT,
  workflow_node    TEXT,
  pending_deps     INTEGER NOT NULL DEFAULT 0,
  lease_token      INTEGER NOT NULL DEFAULT 0,
  lease_owner      TEXT,
  lease_expires_ms INTEGER NOT NULL DEFAULT 0,
  cancel_requested INTEGER NOT NULL DEFAULT 0,
  seq              INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_jobs_state    ON jobs(state);
CREATE INDEX IF NOT EXISTS idx_jobs_created  ON jobs(created_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_jobs_workflow ON jobs(workflow_id);

CREATE TABLE IF NOT EXISTS job_attempts (
  job_id      TEXT NOT NULL,
  attempt     INTEGER NOT NULL,
  status      TEXT NOT NULL,
  started_ms  INTEGER NOT NULL,
  finished_ms INTEGER NOT NULL,
  duration_ms INTEGER NOT NULL,
  worker      TEXT,
  error       TEXT,
  PRIMARY KEY (job_id, attempt)
);

CREATE TABLE IF NOT EXISTS workflows (
  workflow_id    TEXT PRIMARY KEY,
  name           TEXT NOT NULL,
  state          TEXT NOT NULL,
  created_at_ms  INTEGER NOT NULL,
  finished_at_ms INTEGER NOT NULL DEFAULT 0,
  node_count     INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS job_dependencies (
  workflow_id   TEXT NOT NULL,
  parent_job_id TEXT NOT NULL,
  child_job_id  TEXT NOT NULL,
  PRIMARY KEY (workflow_id, parent_job_id, child_job_id)
);
CREATE INDEX IF NOT EXISTS idx_deps_parent ON job_dependencies(parent_job_id);

CREATE TABLE IF NOT EXISTS workers (
  worker_id         TEXT PRIMARY KEY,
  name              TEXT NOT NULL,
  capacity          INTEGER NOT NULL,
  state             TEXT NOT NULL,
  registered_ms     INTEGER NOT NULL,
  last_heartbeat_ms INTEGER NOT NULL,
  jobs_completed    INTEGER NOT NULL DEFAULT 0
);
)SQL";

const char* kJobColumns =
    "job_id,type,payload,priority,state,attempt,max_attempts,timeout_ms,backoff_base_ms,"
    "backoff_mult,backoff_max_ms,backoff_jitter,created_at_ms,started_at_ms,finished_at_ms,"
    "ready_at_ms,queue_wait_us,exec_us,result,error,workflow_id,workflow_node,pending_deps,"
    "lease_token,lease_owner,lease_expires_ms,cancel_requested,seq";

void bind_text(sqlite3_stmt* st, int i, const std::string& s) {
  sqlite3_bind_text(st, i, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
}
void bind_text_or_null(sqlite3_stmt* st, int i, const std::string& s) {
  if (s.empty()) sqlite3_bind_null(st, i);
  else bind_text(st, i, s);
}
// SQLite returns SQLITE_BUSY when another connection holds a write lock, and
// SQLITE_LOCKED when a shared-cache table lock is in the way. busy_timeout covers the
// first but not the second, so both are retried here with a short bounded backoff.
int step_with_retry(sqlite3_stmt* st, int max_attempts = 200) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    const int rc = sqlite3_step(st);
    if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) return rc;
    sqlite3_reset(st);
    std::this_thread::sleep_for(std::chrono::microseconds(200 + 50 * attempt));
  }
  return sqlite3_step(st);
}

std::string col_text(sqlite3_stmt* st, int i) {
  const unsigned char* p = sqlite3_column_text(st, i);
  if (!p) return std::string();
  return std::string(reinterpret_cast<const char*>(p),
                     static_cast<std::size_t>(sqlite3_column_bytes(st, i)));
}

}  // namespace

// ---- Conn -----------------------------------------------------------------------------

JobStore::Conn::~Conn() {
  for (auto& kv : cache) sqlite3_finalize(kv.second);
  cache.clear();
  if (db) sqlite3_close(db);
}

sqlite3_stmt* JobStore::Conn::prepare(const std::string& sql, std::string& err) {
  const auto it = cache.find(sql);
  if (it != cache.end()) {
    sqlite3_reset(it->second);
    sqlite3_clear_bindings(it->second);
    return it->second;
  }
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    err = std::string("prepare failed: ") + sqlite3_errmsg(db) + " | sql: " + sql;
    return nullptr;
  }
  cache.emplace(sql, st);
  return st;
}

// ---- Reader checkout --------------------------------------------------------------------

JobStore::Reader::Reader(const JobStore* store) : store_(store), conn_(nullptr), index_(0) {
  std::unique_lock<std::mutex> lock(store_->pool_mutex_);
  store_->pool_cv_.wait(lock, [&] {
    for (std::size_t i = 0; i < store_->reader_busy_.size(); ++i) {
      if (!store_->reader_busy_[i]) { index_ = i; return true; }
    }
    return false;
  });
  store_->reader_busy_[index_] = true;
  conn_ = store_->readers_[index_].get();
}

JobStore::Reader::~Reader() {
  {
    std::lock_guard<std::mutex> lock(store_->pool_mutex_);
    store_->reader_busy_[index_] = false;
  }
  store_->pool_cv_.notify_one();
}

// ---- open / schema -----------------------------------------------------------------------

bool JobStore::exec(sqlite3* db, const char* sql, std::string& err) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    char* msg = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &msg);
    if (rc == SQLITE_OK) {
      if (msg) sqlite3_free(msg);
      return true;
    }
    if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) {
      err = msg ? msg : "sqlite exec failed";
      if (msg) sqlite3_free(msg);
      return false;
    }
    if (msg) sqlite3_free(msg);
    std::this_thread::sleep_for(std::chrono::microseconds(200 + 50 * attempt));
  }
  err = "sqlite exec still locked after retrying";
  return false;
}

std::unique_ptr<JobStore> JobStore::open(const StoreConfig& cfg, std::string& err) {
  std::unique_ptr<JobStore> store(new JobStore());
  store->cfg_ = cfg;

  const bool in_memory = cfg.path == ":memory:";
  // A shared-cache in-memory database lets the reader pool see the writer's data; without
  // it every connection would get its own private empty database. The name must be unique
  // per JobStore, or two stores opened as ":memory:" in one process would silently share
  // one database - which would make every test observe the previous test's rows.
  static std::atomic<uint64_t> memory_db_counter{0};
  const std::string uri =
      in_memory ? ("file:jobengine_mem_" +
                   std::to_string(memory_db_counter.fetch_add(1, std::memory_order_relaxed)) +
                   "?mode=memory&cache=shared")
                : ("file:" + cfg.path);
  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI |
                    SQLITE_OPEN_FULLMUTEX;

  if (sqlite3_open_v2(uri.c_str(), &store->writer_.db, flags, nullptr) != SQLITE_OK) {
    err = std::string("cannot open database: ") +
          (store->writer_.db ? sqlite3_errmsg(store->writer_.db) : "unknown");
    return nullptr;
  }
  sqlite3_busy_timeout(store->writer_.db, 5000);
  if (!in_memory) {
    if (!exec(store->writer_.db, "PRAGMA journal_mode=WAL;", err)) return nullptr;
  }
  const char* sync = cfg.synchronous_full ? "PRAGMA synchronous=FULL;"
                                          : "PRAGMA synchronous=NORMAL;";
  if (!exec(store->writer_.db, sync, err)) return nullptr;
  if (!exec(store->writer_.db, "PRAGMA foreign_keys=ON;", err)) return nullptr;
  if (!store->init_schema(err)) return nullptr;

  const int n = cfg.reader_connections > 0 ? cfg.reader_connections : 1;
  for (int i = 0; i < n; ++i) {
    auto c = std::make_unique<Conn>();
    const int rflags = (in_memory ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY) |
                       SQLITE_OPEN_URI | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(uri.c_str(), &c->db, rflags, nullptr) != SQLITE_OK) {
      err = std::string("cannot open reader connection: ") +
            (c->db ? sqlite3_errmsg(c->db) : "unknown");
      return nullptr;
    }
    sqlite3_busy_timeout(c->db, 5000);
    if (in_memory) {
      // A shared-cache in-memory database uses table-level locks, where a reader's read
      // lock makes the writer fail with SQLITE_LOCKED rather than waiting. Reading
      // uncommitted data removes the read lock. This applies ONLY to ":memory:" stores
      // (tests and benchmarks); a file-backed store uses WAL, where readers never block
      // the writer and this pragma is not set.
      std::string ignored;
      exec(c->db, "PRAGMA read_uncommitted=1;", ignored);
    }
    store->readers_.push_back(std::move(c));
    store->reader_busy_.push_back(false);
  }
  store->start_flusher();
  return store;
}

JobStore::~JobStore() {
  // Everything buffered must reach the database before the connections close.
  flush();
  stop_flusher();
}

bool JobStore::init_schema(std::string& err) { return exec(writer_.db, kSchema, err); }

bool JobStore::checkpoint(std::string& err) {
  flush();
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (cfg_.path == ":memory:") return true;
  return exec(writer_.db, "PRAGMA wal_checkpoint(TRUNCATE);", err);
}

// ---- job rows -----------------------------------------------------------------------------

bool JobStore::write_job_row_locked(Conn& c, const Job& job, bool insert, std::string& err) {
  static const std::string kInsert =
      std::string("INSERT INTO jobs (") + kJobColumns +
      ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,"
      "?21,?22,?23,?24,?25,?26,?27,?28)";
  static const std::string kUpdate =
      "UPDATE jobs SET type=?2,payload=?3,priority=?4,state=?5,attempt=?6,max_attempts=?7,"
      "timeout_ms=?8,backoff_base_ms=?9,backoff_mult=?10,backoff_max_ms=?11,"
      "backoff_jitter=?12,created_at_ms=?13,started_at_ms=?14,finished_at_ms=?15,"
      "ready_at_ms=?16,queue_wait_us=?17,exec_us=?18,result=?19,error=?20,workflow_id=?21,"
      "workflow_node=?22,pending_deps=?23,lease_token=?24,lease_owner=?25,"
      "lease_expires_ms=?26,cancel_requested=?27,seq=?28 WHERE job_id=?1";

  sqlite3_stmt* st = c.prepare(insert ? kInsert : kUpdate, err);
  if (!st) return false;

  const std::string payload_text = job.payload.dump();
  const std::string result_text = job.result.is_null() ? std::string() : job.result.dump();

  bind_text(st, 1, job.job_id);
  bind_text(st, 2, job.type);
  bind_text(st, 3, payload_text);
  sqlite3_bind_int64(st, 4, job.priority);
  bind_text(st, 5, to_string(job.state));
  sqlite3_bind_int64(st, 6, job.attempt);
  sqlite3_bind_int64(st, 7, job.retry.max_attempts);
  sqlite3_bind_int64(st, 8, job.timeout_ms);
  sqlite3_bind_int64(st, 9, job.retry.backoff_base_ms);
  sqlite3_bind_double(st, 10, job.retry.backoff_multiplier);
  sqlite3_bind_int64(st, 11, job.retry.backoff_max_ms);
  sqlite3_bind_int64(st, 12, job.retry.jitter ? 1 : 0);
  sqlite3_bind_int64(st, 13, job.created_at_ms);
  sqlite3_bind_int64(st, 14, job.started_at_wall_ms);
  sqlite3_bind_int64(st, 15, job.finished_at_wall_ms);
  sqlite3_bind_int64(st, 16, job.ready_at_wall_ms);
  sqlite3_bind_int64(st, 17, job.queue_wait_us);
  sqlite3_bind_int64(st, 18, job.exec_us);
  bind_text_or_null(st, 19, result_text);
  bind_text_or_null(st, 20, job.error);
  bind_text_or_null(st, 21, job.workflow_id);
  bind_text_or_null(st, 22, job.workflow_node);
  sqlite3_bind_int64(st, 23, job.pending_deps);
  sqlite3_bind_int64(st, 24, static_cast<sqlite3_int64>(job.lease_token));
  bind_text_or_null(st, 25, job.lease_owner);
  sqlite3_bind_int64(st, 26, job.lease_expires_ms);
  sqlite3_bind_int64(st, 27, job.cancel_requested ? 1 : 0);
  sqlite3_bind_int64(st, 28, job.seq);

  if (step_with_retry(st) != SQLITE_DONE) {
    err = std::string("job write failed: ") + sqlite3_errmsg(c.db);
    sqlite3_reset(st);
    return false;
  }
  sqlite3_reset(st);
  return true;
}

bool JobStore::write_job_row(const Job& job, bool insert, std::string& err) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  return write_job_row_locked(writer_, job, insert, err);
}

bool JobStore::insert_job(const Job& job, std::string& err) {
  if (!cfg_.group_commit) return write_job_row(job, true, err);
  return insert_jobs(std::vector<Job>{job}, err);
}

bool JobStore::update_job(const Job& job, std::string& err) {
  if (!cfg_.group_commit) return write_job_row(job, false, err);
  {
    std::unique_lock<std::mutex> lock(wb_mutex_);
    // Bound the buffer. The wait is capped so a stalled flusher degrades throughput
    // rather than blocking the engine forever.
    if (cfg_.max_buffered_rows > 0) {
      const auto buffered = [this] {
        return pending_inserts_.size() + pending_updates_.size() + pending_attempts_.size();
      };
      if (buffered() >= cfg_.max_buffered_rows) {
        wb_work_cv_.notify_one();
        wb_done_cv_.wait_for(lock, std::chrono::milliseconds(50), [&] {
          return flusher_stop_ || buffered() < cfg_.max_buffered_rows;
        });
      }
    }
    VersionedJob v;
    v.version = ++version_counter_;
    v.job = job;
    pending_updates_.push_back(v);
    cache_[job.job_id] = v;
  }
  // A state transition does not block its caller: the row is immediately visible through
  // the read-through cache, and losing it in a hard crash is exactly the at-least-once
  // window that restart recovery already handles.
  wb_work_cv_.notify_one();
  (void)err;
  return true;
}

bool JobStore::insert_jobs(const std::vector<Job>& jobs, std::string& err) {
  if (jobs.empty()) return true;
  if (!cfg_.group_commit) {
    std::lock_guard<std::mutex> lock(writer_mutex_);
    if (!exec(writer_.db, "BEGIN IMMEDIATE", err)) return false;
    for (const Job& j : jobs) {
      if (!write_job_row_locked(writer_, j, true, err)) {
        std::string ignored;
        exec(writer_.db, "ROLLBACK", ignored);
        return false;
      }
    }
    return exec(writer_.db, "COMMIT", err);
  }

  // Group commit: queue the rows and wait for the batch containing them. Concurrent
  // submitters coalesce into one transaction instead of paying a commit each, while every
  // caller still returns only once its own row is durable.
  uint64_t my_seq = 0;
  {
    std::lock_guard<std::mutex> lock(wb_mutex_);
    my_seq = ++insert_seq_;
    for (const Job& j : jobs) {
      VersionedJob v;
      v.version = ++version_counter_;
      v.insert_seq = my_seq;
      v.job = j;
      pending_inserts_.push_back(v);
      cache_[j.job_id] = v;
    }
  }
  wb_work_cv_.notify_one();
  std::unique_lock<std::mutex> lock(wb_mutex_);
  wb_done_cv_.wait(lock, [&] { return committed_insert_seq_ >= my_seq || flusher_stop_; });
  if (!last_flush_error_.empty()) {
    err = last_flush_error_;
    return false;
  }
  return true;
}

Job JobStore::read_job_row(sqlite3_stmt* st) {
  Job j;
  j.job_id = col_text(st, 0);
  j.type = col_text(st, 1);
  j.payload = Json::parse_or_null(col_text(st, 2));
  if (j.payload.is_null()) j.payload = Json::object();
  j.priority = static_cast<int>(sqlite3_column_int64(st, 3));
  JobState s = JobState::Queued;
  state_from_string(col_text(st, 4), s);
  j.state = s;
  j.attempt = static_cast<int>(sqlite3_column_int64(st, 5));
  j.retry.max_attempts = static_cast<int>(sqlite3_column_int64(st, 6));
  j.timeout_ms = sqlite3_column_int64(st, 7);
  j.retry.backoff_base_ms = sqlite3_column_int64(st, 8);
  j.retry.backoff_multiplier = sqlite3_column_double(st, 9);
  j.retry.backoff_max_ms = sqlite3_column_int64(st, 10);
  j.retry.jitter = sqlite3_column_int64(st, 11) != 0;
  j.created_at_ms = sqlite3_column_int64(st, 12);
  j.started_at_wall_ms = sqlite3_column_int64(st, 13);
  j.finished_at_wall_ms = sqlite3_column_int64(st, 14);
  j.ready_at_wall_ms = sqlite3_column_int64(st, 15);
  j.queue_wait_us = sqlite3_column_int64(st, 16);
  j.exec_us = sqlite3_column_int64(st, 17);
  const std::string result_text = col_text(st, 18);
  j.result = result_text.empty() ? Json() : Json::parse_or_null(result_text);
  j.error = col_text(st, 19);
  j.workflow_id = col_text(st, 20);
  j.workflow_node = col_text(st, 21);
  j.pending_deps = static_cast<int>(sqlite3_column_int64(st, 22));
  j.lease_token = static_cast<uint64_t>(sqlite3_column_int64(st, 23));
  j.lease_owner = col_text(st, 24);
  j.lease_expires_ms = sqlite3_column_int64(st, 25);
  j.cancel_requested = sqlite3_column_int64(st, 26) != 0;
  j.seq = sqlite3_column_int64(st, 27);
  return j;
}

bool JobStore::get_job(const std::string& job_id, Job& out) const {
  if (cfg_.group_commit) {
    // Read-through: a row queued for flush, or inside the transaction currently being
    // committed, is newer than anything the database can return.
    std::lock_guard<std::mutex> lock(wb_mutex_);
    const auto it = cache_.find(job_id);
    if (it != cache_.end()) { out = it->second.job; return true; }
  }
  Reader r(this);
  std::string err;
  sqlite3_stmt* st =
      r->prepare(std::string("SELECT ") + kJobColumns + " FROM jobs WHERE job_id=?1", err);
  if (!st) return false;
  bind_text(st, 1, job_id);
  const bool found = step_with_retry(st) == SQLITE_ROW;
  if (found) out = read_job_row(st);
  sqlite3_reset(st);
  return found;
}

std::vector<Job> JobStore::list_jobs(const std::optional<JobState>& state, int limit,
                                     int offset) const {
  const_cast<JobStore*>(this)->flush();
  Reader r(this);
  std::string err;
  std::vector<Job> out;
  const std::string sql =
      state ? (std::string("SELECT ") + kJobColumns +
               " FROM jobs WHERE state=?1 ORDER BY created_at_ms DESC, job_id DESC "
               "LIMIT ?2 OFFSET ?3")
            : (std::string("SELECT ") + kJobColumns +
               " FROM jobs ORDER BY created_at_ms DESC, job_id DESC LIMIT ?1 OFFSET ?2");
  sqlite3_stmt* st = r->prepare(sql, err);
  if (!st) return out;
  if (state) {
    bind_text(st, 1, to_string(*state));
    sqlite3_bind_int64(st, 2, limit);
    sqlite3_bind_int64(st, 3, offset);
  } else {
    sqlite3_bind_int64(st, 1, limit);
    sqlite3_bind_int64(st, 2, offset);
  }
  while (step_with_retry(st) == SQLITE_ROW) out.push_back(read_job_row(st));
  sqlite3_reset(st);
  return out;
}

std::vector<Job> JobStore::list_workflow_jobs(const std::string& workflow_id) const {
  const_cast<JobStore*>(this)->flush();
  Reader r(this);
  std::string err;
  std::vector<Job> out;
  sqlite3_stmt* st = r->prepare(std::string("SELECT ") + kJobColumns +
                                    " FROM jobs WHERE workflow_id=?1 ORDER BY seq ASC",
                                err);
  if (!st) return out;
  bind_text(st, 1, workflow_id);
  while (step_with_retry(st) == SQLITE_ROW) out.push_back(read_job_row(st));
  sqlite3_reset(st);
  return out;
}

std::vector<Job> JobStore::load_non_terminal_jobs() const {
  const_cast<JobStore*>(this)->flush();
  Reader r(this);
  std::string err;
  std::vector<Job> out;
  sqlite3_stmt* st = r->prepare(
      std::string("SELECT ") + kJobColumns +
          " FROM jobs WHERE state IN ('PENDING','QUEUED','SCHEDULED','RUNNING','RETRYING') "
          "ORDER BY priority DESC, seq ASC",
      err);
  if (!st) return out;
  while (step_with_retry(st) == SQLITE_ROW) out.push_back(read_job_row(st));
  sqlite3_reset(st);
  return out;
}

int64_t JobStore::count_jobs(const std::optional<JobState>& state) const {
  const_cast<JobStore*>(this)->flush();
  Reader r(this);
  std::string err;
  sqlite3_stmt* st = r->prepare(
      state ? "SELECT COUNT(*) FROM jobs WHERE state=?1" : "SELECT COUNT(*) FROM jobs", err);
  if (!st) return 0;
  if (state) bind_text(st, 1, to_string(*state));
  int64_t n = 0;
  if (step_with_retry(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
  sqlite3_reset(st);
  return n;
}

std::unordered_map<std::string, int64_t> JobStore::count_by_state() const {
  const_cast<JobStore*>(this)->flush();
  Reader r(this);
  std::string err;
  std::unordered_map<std::string, int64_t> out;
  sqlite3_stmt* st = r->prepare("SELECT state, COUNT(*) FROM jobs GROUP BY state", err);
  if (!st) return out;
  while (step_with_retry(st) == SQLITE_ROW) out[col_text(st, 0)] = sqlite3_column_int64(st, 1);
  sqlite3_reset(st);
  return out;
}

// ---- attempts -----------------------------------------------------------------------------

bool JobStore::append_attempt(const AttemptRecord& rec, std::string& err) {
  if (cfg_.group_commit) {
    {
      std::lock_guard<std::mutex> lock(wb_mutex_);
      pending_attempts_.push_back(rec);
      ++version_counter_;
    }
    wb_work_cv_.notify_one();
    (void)err;
    return true;
  }
  return append_attempt_direct(rec, err);
}

bool JobStore::append_attempt_direct(const AttemptRecord& rec, std::string& err) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  return append_attempt_locked(rec, err);
}

bool JobStore::append_attempt_locked(const AttemptRecord& rec, std::string& err) {
  // INSERT OR REPLACE keeps the (job_id, attempt) primary key authoritative: a retried
  // delivery of the same attempt cannot create a second row.
  sqlite3_stmt* st = writer_.prepare(
      "INSERT OR REPLACE INTO job_attempts "
      "(job_id,attempt,status,started_ms,finished_ms,duration_ms,worker,error) "
      "VALUES (?1,?2,?3,?4,?5,?6,?7,?8)",
      err);
  if (!st) return false;
  bind_text(st, 1, rec.job_id);
  sqlite3_bind_int64(st, 2, rec.attempt);
  bind_text(st, 3, rec.status);
  sqlite3_bind_int64(st, 4, rec.started_ms);
  sqlite3_bind_int64(st, 5, rec.finished_ms);
  sqlite3_bind_int64(st, 6, rec.duration_ms);
  bind_text_or_null(st, 7, rec.worker);
  bind_text_or_null(st, 8, rec.error);
  if (step_with_retry(st) != SQLITE_DONE) {
    err = std::string("attempt write failed: ") + sqlite3_errmsg(writer_.db);
    sqlite3_reset(st);
    return false;
  }
  sqlite3_reset(st);
  return true;
}

std::vector<AttemptRecord> JobStore::list_attempts(const std::string& job_id) const {
  const_cast<JobStore*>(this)->flush();
  Reader r(this);
  std::string err;
  std::vector<AttemptRecord> out;
  sqlite3_stmt* st = r->prepare(
      "SELECT job_id,attempt,status,started_ms,finished_ms,duration_ms,worker,error "
      "FROM job_attempts WHERE job_id=?1 ORDER BY attempt ASC",
      err);
  if (!st) return out;
  bind_text(st, 1, job_id);
  while (step_with_retry(st) == SQLITE_ROW) {
    AttemptRecord a;
    a.job_id = col_text(st, 0);
    a.attempt = static_cast<int>(sqlite3_column_int64(st, 1));
    a.status = col_text(st, 2);
    a.started_ms = sqlite3_column_int64(st, 3);
    a.finished_ms = sqlite3_column_int64(st, 4);
    a.duration_ms = sqlite3_column_int64(st, 5);
    a.worker = col_text(st, 6);
    a.error = col_text(st, 7);
    out.push_back(std::move(a));
  }
  sqlite3_reset(st);
  return out;
}

// ---- workflows ------------------------------------------------------------------------------

bool JobStore::insert_workflow(const WorkflowRecord& wf, const std::vector<Job>& nodes,
                               const std::vector<std::pair<std::string, std::string>>& edges,
                               std::string& err) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (!exec(writer_.db, "BEGIN IMMEDIATE", err)) return false;
  const auto rollback = [&] {
    std::string ignored;
    exec(writer_.db, "ROLLBACK", ignored);
  };

  sqlite3_stmt* st = writer_.prepare(
      "INSERT INTO workflows (workflow_id,name,state,created_at_ms,finished_at_ms,node_count) "
      "VALUES (?1,?2,?3,?4,?5,?6)",
      err);
  if (!st) { rollback(); return false; }
  bind_text(st, 1, wf.workflow_id);
  bind_text(st, 2, wf.name);
  bind_text(st, 3, wf.state);
  sqlite3_bind_int64(st, 4, wf.created_at_ms);
  sqlite3_bind_int64(st, 5, wf.finished_at_ms);
  sqlite3_bind_int64(st, 6, wf.node_count);
  if (step_with_retry(st) != SQLITE_DONE) {
    err = std::string("workflow insert failed: ") + sqlite3_errmsg(writer_.db);
    sqlite3_reset(st);
    rollback();
    return false;
  }
  sqlite3_reset(st);

  for (const Job& j : nodes) {
    if (!write_job_row_locked(writer_, j, true, err)) { rollback(); return false; }
  }
  for (const auto& e : edges) {
    sqlite3_stmt* ds = writer_.prepare(
        "INSERT INTO job_dependencies (workflow_id,parent_job_id,child_job_id) "
        "VALUES (?1,?2,?3)",
        err);
    if (!ds) { rollback(); return false; }
    bind_text(ds, 1, wf.workflow_id);
    bind_text(ds, 2, e.first);
    bind_text(ds, 3, e.second);
    if (step_with_retry(ds) != SQLITE_DONE) {
      err = std::string("dependency insert failed: ") + sqlite3_errmsg(writer_.db);
      sqlite3_reset(ds);
      rollback();
      return false;
    }
    sqlite3_reset(ds);
  }
  return exec(writer_.db, "COMMIT", err);
}

bool JobStore::update_workflow(const WorkflowRecord& wf, std::string& err) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  sqlite3_stmt* st = writer_.prepare(
      "UPDATE workflows SET state=?2, finished_at_ms=?3 WHERE workflow_id=?1", err);
  if (!st) return false;
  bind_text(st, 1, wf.workflow_id);
  bind_text(st, 2, wf.state);
  sqlite3_bind_int64(st, 3, wf.finished_at_ms);
  if (step_with_retry(st) != SQLITE_DONE) {
    err = std::string("workflow update failed: ") + sqlite3_errmsg(writer_.db);
    sqlite3_reset(st);
    return false;
  }
  sqlite3_reset(st);
  return true;
}

bool JobStore::get_workflow(const std::string& workflow_id, WorkflowRecord& out) const {
  Reader r(this);
  std::string err;
  sqlite3_stmt* st = r->prepare(
      "SELECT workflow_id,name,state,created_at_ms,finished_at_ms,node_count "
      "FROM workflows WHERE workflow_id=?1",
      err);
  if (!st) return false;
  bind_text(st, 1, workflow_id);
  const bool found = step_with_retry(st) == SQLITE_ROW;
  if (found) {
    out.workflow_id = col_text(st, 0);
    out.name = col_text(st, 1);
    out.state = col_text(st, 2);
    out.created_at_ms = sqlite3_column_int64(st, 3);
    out.finished_at_ms = sqlite3_column_int64(st, 4);
    out.node_count = static_cast<int>(sqlite3_column_int64(st, 5));
  }
  sqlite3_reset(st);
  return found;
}

std::vector<WorkflowRecord> JobStore::list_workflows(int limit) const {
  Reader r(this);
  std::string err;
  std::vector<WorkflowRecord> out;
  sqlite3_stmt* st = r->prepare(
      "SELECT workflow_id,name,state,created_at_ms,finished_at_ms,node_count "
      "FROM workflows ORDER BY created_at_ms DESC LIMIT ?1",
      err);
  if (!st) return out;
  sqlite3_bind_int64(st, 1, limit);
  while (step_with_retry(st) == SQLITE_ROW) {
    WorkflowRecord w;
    w.workflow_id = col_text(st, 0);
    w.name = col_text(st, 1);
    w.state = col_text(st, 2);
    w.created_at_ms = sqlite3_column_int64(st, 3);
    w.finished_at_ms = sqlite3_column_int64(st, 4);
    w.node_count = static_cast<int>(sqlite3_column_int64(st, 5));
    out.push_back(std::move(w));
  }
  sqlite3_reset(st);
  return out;
}

std::vector<std::pair<std::string, std::string>> JobStore::load_dependencies(
    const std::string& workflow_id) const {
  Reader r(this);
  std::string err;
  std::vector<std::pair<std::string, std::string>> out;
  sqlite3_stmt* st = r->prepare(
      "SELECT parent_job_id, child_job_id FROM job_dependencies WHERE workflow_id=?1", err);
  if (!st) return out;
  bind_text(st, 1, workflow_id);
  while (step_with_retry(st) == SQLITE_ROW) out.emplace_back(col_text(st, 0), col_text(st, 1));
  sqlite3_reset(st);
  return out;
}

std::vector<std::pair<std::string, std::string>> JobStore::load_all_dependencies() const {
  Reader r(this);
  std::string err;
  std::vector<std::pair<std::string, std::string>> out;
  sqlite3_stmt* st = r->prepare("SELECT parent_job_id, child_job_id FROM job_dependencies", err);
  if (!st) return out;
  while (step_with_retry(st) == SQLITE_ROW) out.emplace_back(col_text(st, 0), col_text(st, 1));
  sqlite3_reset(st);
  return out;
}

// ---- workers ----------------------------------------------------------------------------------

bool JobStore::upsert_worker(const WorkerRecord& w, std::string& err) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  sqlite3_stmt* st = writer_.prepare(
      "INSERT INTO workers (worker_id,name,capacity,state,registered_ms,last_heartbeat_ms,"
      "jobs_completed) VALUES (?1,?2,?3,?4,?5,?6,?7) "
      "ON CONFLICT(worker_id) DO UPDATE SET name=excluded.name,capacity=excluded.capacity,"
      "state=excluded.state,last_heartbeat_ms=excluded.last_heartbeat_ms,"
      "jobs_completed=excluded.jobs_completed",
      err);
  if (!st) return false;
  bind_text(st, 1, w.worker_id);
  bind_text(st, 2, w.name);
  sqlite3_bind_int64(st, 3, w.capacity);
  bind_text(st, 4, w.state);
  sqlite3_bind_int64(st, 5, w.registered_ms);
  sqlite3_bind_int64(st, 6, w.last_heartbeat_ms);
  sqlite3_bind_int64(st, 7, w.jobs_completed);
  if (step_with_retry(st) != SQLITE_DONE) {
    err = std::string("worker upsert failed: ") + sqlite3_errmsg(writer_.db);
    sqlite3_reset(st);
    return false;
  }
  sqlite3_reset(st);
  return true;
}

bool JobStore::touch_worker(const std::string& worker_id, int64_t heartbeat_ms,
                            std::string& err) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  sqlite3_stmt* st = writer_.prepare(
      "UPDATE workers SET last_heartbeat_ms=?2, state='ALIVE' WHERE worker_id=?1", err);
  if (!st) return false;
  bind_text(st, 1, worker_id);
  sqlite3_bind_int64(st, 2, heartbeat_ms);
  if (step_with_retry(st) != SQLITE_DONE) {
    err = std::string("worker heartbeat failed: ") + sqlite3_errmsg(writer_.db);
    sqlite3_reset(st);
    return false;
  }
  sqlite3_reset(st);
  return true;
}

bool JobStore::set_worker_state(const std::string& worker_id, const std::string& state,
                                std::string& err) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  sqlite3_stmt* st = writer_.prepare("UPDATE workers SET state=?2 WHERE worker_id=?1", err);
  if (!st) return false;
  bind_text(st, 1, worker_id);
  bind_text(st, 2, state);
  if (step_with_retry(st) != SQLITE_DONE) {
    err = std::string("worker state update failed: ") + sqlite3_errmsg(writer_.db);
    sqlite3_reset(st);
    return false;
  }
  sqlite3_reset(st);
  return true;
}

std::vector<WorkerRecord> JobStore::list_workers() const {
  Reader r(this);
  std::string err;
  std::vector<WorkerRecord> out;
  sqlite3_stmt* st = r->prepare(
      "SELECT worker_id,name,capacity,state,registered_ms,last_heartbeat_ms,jobs_completed "
      "FROM workers ORDER BY registered_ms ASC",
      err);
  if (!st) return out;
  while (step_with_retry(st) == SQLITE_ROW) {
    WorkerRecord w;
    w.worker_id = col_text(st, 0);
    w.name = col_text(st, 1);
    w.capacity = static_cast<int>(sqlite3_column_int64(st, 2));
    w.state = col_text(st, 3);
    w.registered_ms = sqlite3_column_int64(st, 4);
    w.last_heartbeat_ms = sqlite3_column_int64(st, 5);
    w.jobs_completed = sqlite3_column_int64(st, 6);
    out.push_back(std::move(w));
  }
  sqlite3_reset(st);
  return out;
}

// ---- group commit ------------------------------------------------------------------------------

void JobStore::start_flusher() {
  if (!cfg_.group_commit) return;
  flusher_ = std::thread([this] { flusher_loop(); });
}

void JobStore::stop_flusher() {
  if (!flusher_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(wb_mutex_);
    flusher_stop_ = true;
  }
  wb_work_cv_.notify_all();
  flusher_.join();
  wb_done_cv_.notify_all();
}

void JobStore::flusher_loop() {
  for (;;) {
    std::vector<VersionedJob> inserts, updates;
    std::vector<AttemptRecord> attempts;
    uint64_t batch_insert_seq = 0;
    {
      std::unique_lock<std::mutex> lock(wb_mutex_);
      wb_work_cv_.wait(lock, [this] { return flusher_stop_ || has_pending_locked(); });
      if (!has_pending_locked()) {
        if (flusher_stop_) return;
        continue;
      }
      if (cfg_.commit_delay_us > 0 && !flusher_stop_) {
        // Let more writers join this batch before the transaction is opened.
        wb_work_cv_.wait_for(lock, std::chrono::microseconds(cfg_.commit_delay_us),
                             [this] { return pending_inserts_.size() + pending_updates_.size() +
                                             pending_attempts_.size() >= cfg_.max_batch; });
      }
      // Take a bounded prefix so one huge burst cannot hold the write lock indefinitely.
      const auto take = [&](auto& src, auto& dst) {
        const std::size_t n = std::min(src.size(), cfg_.max_batch);
        dst.assign(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(n));
        src.erase(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(n));
      };
      take(pending_inserts_, inserts);
      take(pending_updates_, updates);
      take(pending_attempts_, attempts);
      // Inserts are taken in arrival order, so the last one taken is a valid watermark:
      // every insert batch with a smaller sequence is already in this or an earlier write.
      if (!inserts.empty()) batch_insert_seq = inserts.back().insert_seq;
      // Attempt rows must be counted too. Leaving them out let flush() return while a
      // transaction containing only attempt rows was still open, so a caller that flushed
      // and then read job_attempts could see nothing - which is exactly what
      // StoreTest.AttemptsArePerAttemptAndIdempotent caught under ThreadSanitizer.
      in_flight_ = inserts.size() + updates.size() + attempts.size();
    }

    std::string err;
    const bool ok = commit_batch(inserts, updates, attempts, err);

    {
      std::lock_guard<std::mutex> lock(wb_mutex_);
      // Drop a cache entry only when the version just written is still the newest one.
      // A write that arrived while the transaction was open keeps its entry alive.
      const auto retire = [&](const VersionedJob& v) {
        const auto it = cache_.find(v.job.job_id);
        if (it != cache_.end() && it->second.version == v.version) cache_.erase(it);
      };
      for (const VersionedJob& v : inserts) retire(v);
      for (const VersionedJob& v : updates) retire(v);
      in_flight_ = 0;
      if (!ok) last_flush_error_ = err;
      if (batch_insert_seq > committed_insert_seq_) committed_insert_seq_ = batch_insert_seq;
      batches_committed_.fetch_add(1, std::memory_order_relaxed);
      writes_committed_.fetch_add(inserts.size() + updates.size() + attempts.size(),
                                  std::memory_order_relaxed);
    }
    wb_done_cv_.notify_all();
  }
}

bool JobStore::commit_batch(const std::vector<VersionedJob>& inserts,
                            const std::vector<VersionedJob>& updates,
                            const std::vector<AttemptRecord>& attempts, std::string& err) {
  if (inserts.empty() && updates.empty() && attempts.empty()) return true;
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (!exec(writer_.db, "BEGIN IMMEDIATE", err)) return false;
  const auto rollback = [&] {
    std::string ignored;
    exec(writer_.db, "ROLLBACK", ignored);
  };
  // Inserts first: the same batch can contain a row's insert and a later update to it.
  for (const VersionedJob& v : inserts) {
    if (!write_job_row_locked(writer_, v.job, true, err)) { rollback(); return false; }
  }
  for (const VersionedJob& v : updates) {
    if (!write_job_row_locked(writer_, v.job, false, err)) { rollback(); return false; }
  }
  for (const AttemptRecord& a : attempts) {
    if (!append_attempt_locked(a, err)) { rollback(); return false; }
  }
  return exec(writer_.db, "COMMIT", err);
}

void JobStore::flush() {
  if (!cfg_.group_commit) return;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(wb_mutex_);
      if ((!has_pending_locked() && in_flight_ == 0) || flusher_stop_) return;
    }
    wb_work_cv_.notify_one();
    std::unique_lock<std::mutex> lock(wb_mutex_);
    wb_done_cv_.wait_for(lock, std::chrono::milliseconds(5));
  }
}

}  // namespace je
