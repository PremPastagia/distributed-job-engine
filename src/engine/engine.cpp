#include "jobengine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <stdexcept>

#include "jobengine/util.hpp"

namespace je {

const char* to_string(SubmitStatus s) {
  switch (s) {
    case SubmitStatus::Ok: return "ok";
    case SubmitStatus::UnknownType: return "unknown_type";
    case SubmitStatus::QueueFull: return "queue_full";
    case SubmitStatus::StoreError: return "store_error";
    case SubmitStatus::Stopped: return "stopped";
  }
  return "unknown";
}

const char* to_string(WorkflowStatus s) {
  switch (s) {
    case WorkflowStatus::Ok: return "ok";
    case WorkflowStatus::Empty: return "empty_workflow";
    case WorkflowStatus::UnknownType: return "unknown_type";
    case WorkflowStatus::DuplicateNode: return "duplicate_node_name";
    case WorkflowStatus::UnknownNodeRef: return "unknown_node_reference";
    case WorkflowStatus::SelfEdge: return "self_edge";
    case WorkflowStatus::Cycle: return "cycle_detected";
    case WorkflowStatus::QueueFull: return "queue_full";
    case WorkflowStatus::StoreError: return "store_error";
    case WorkflowStatus::Stopped: return "stopped";
  }
  return "unknown";
}

JobEngine::JobEngine(EngineConfig cfg, JobStore* store, HandlerRegistry* handlers,
                     Metrics* metrics)
    : cfg_(std::move(cfg)),
      store_(store),
      handlers_(handlers),
      metrics_(metrics),
      queue_(cfg_.max_outstanding_jobs) {}

JobEngine::~JobEngine() {
  if (started_ && !stop_requested_.load()) stop(false);
}

// ---- admission control ---------------------------------------------------------------
// Capacity bounds *outstanding* (non-terminal) jobs rather than queued ones, so a burst
// cannot make the engine hold unbounded state in PENDING/RETRYING either. A slot is
// reserved before the row is written, so a rejected submission never leaves a row behind.

bool JobEngine::reserve_slot() {
  std::size_t cur = outstanding_.load(std::memory_order_relaxed);
  for (;;) {
    if (cur >= cfg_.max_outstanding_jobs) return false;
    if (outstanding_.compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) {
      return true;
    }
  }
}

void JobEngine::release_slot() {
  const std::size_t prev = outstanding_.fetch_sub(1, std::memory_order_acq_rel);
  if (prev == 1) {
    std::lock_guard<std::mutex> lock(run_mutex_);
    idle_cv_.notify_all();
  }
}

bool JobEngine::wait_idle(int64_t timeout_ms) const {
  std::unique_lock<std::mutex> lock(run_mutex_);
  return idle_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [this] { return outstanding_.load() == 0; });
}

void JobEngine::persist(const Job& job) {
  std::string err;
  if (!store_->update_job(job, err)) {
    log_error("engine", concat({"failed to persist job ", job.job_id, ": ", err}));
  }
}

void JobEngine::enqueue(const Job& job) {
  const int64_t now_steady = now_ms();
  const int64_t now_wall = wall_ms();
  // ready_at is stored as wall clock; convert to the steady timeline the queue schedules on.
  int64_t ready_steady = now_steady;
  if (job.ready_at_wall_ms > now_wall) ready_steady = now_steady + (job.ready_at_wall_ms - now_wall);
  const auto st = queue_.push(job.job_id, job.priority, ready_steady);
  if (st == PriorityJobQueue::PushStatus::Full) {
    log_error("engine", concat({"queue full while enqueuing ", job.job_id}));
  }
}

// ---- submit ---------------------------------------------------------------------------

SubmitStatus JobEngine::submit(const SubmitRequest& req, std::string& out_job_id,
                               std::string& err) {
  if (stop_requested_.load()) { err = "engine is stopping"; return SubmitStatus::Stopped; }
  if (!handlers_->has(req.type)) {
    err = "unknown job type: " + req.type;
    return SubmitStatus::UnknownType;
  }
  if (!reserve_slot()) {
    metrics_->jobs_rejected_queue_full.fetch_add(1, std::memory_order_relaxed);
    err = "engine at capacity (" + num(static_cast<int64_t>(cfg_.max_outstanding_jobs)) +
          " outstanding jobs)";
    return SubmitStatus::QueueFull;
  }

  Job job;
  job.job_id = new_id('j');
  job.type = req.type;
  job.payload = req.payload;
  job.priority = req.priority;
  job.retry = req.retry;
  if (job.retry.max_attempts < 1) job.retry.max_attempts = 1;
  job.timeout_ms = req.timeout_ms;
  job.created_at_ms = wall_ms();
  job.ready_at_wall_ms = job.created_at_ms + std::max<int64_t>(0, req.delay_ms);
  job.enqueued_at_steady = now_ms();
  job.workflow_id = req.workflow_id;
  job.workflow_node = req.workflow_node;
  job.pending_deps = req.pending_deps;
  job.seq = seq_counter_.fetch_add(1, std::memory_order_relaxed);

  if (job.pending_deps > 0) job.state = JobState::Pending;
  else if (req.delay_ms > 0) job.state = JobState::Scheduled;
  else job.state = JobState::Queued;

  if (!store_->insert_job(job, err)) {
    release_slot();
    return SubmitStatus::StoreError;
  }
  metrics_->jobs_submitted.fetch_add(1, std::memory_order_relaxed);
  if (job.state != JobState::Pending) enqueue(job);
  out_job_id = job.job_id;
  return SubmitStatus::Ok;
}

bool JobEngine::get_job(const std::string& job_id, Job& out) const {
  return store_->get_job(job_id, out);
}

std::size_t JobEngine::running_count() const {
  std::lock_guard<std::mutex> lock(run_mutex_);
  return running_.size();
}

std::size_t JobEngine::alive_workers() const {
  std::lock_guard<std::mutex> lock(run_mutex_);
  std::size_t n = 0;
  for (const auto& kv : remote_workers_) {
    if (kv.second.alive) ++n;
  }
  return n;
}

// ---- cancellation ----------------------------------------------------------------------
// run_mutex_ is the job-transition lock: every read-decide-write of a job's state runs
// under it, so cancel() and begin_attempt() can never both win for the same job.

CancelStatus JobEngine::cancel(const std::string& job_id, std::string& err) {
  Job terminal_job;
  bool became_terminal = false;
  {
    std::lock_guard<std::mutex> lock(run_mutex_);
    Job job;
    if (!store_->get_job(job_id, job)) return CancelStatus::NotFound;
    if (is_terminal(job.state)) return CancelStatus::AlreadyTerminal;

    if (job.state == JobState::Running) {
      // Cooperative: flag it and let the handler notice. The attempt is finished by the
      // worker, which will see the User cancel reason.
      job.cancel_requested = true;
      if (!store_->update_job(job, err)) return CancelStatus::NotFound;
      const auto it = running_.find(job_id);
      if (it != running_.end()) {
        it->second.cancel_reason->store(static_cast<int>(CancelReason::User),
                                        std::memory_order_relaxed);
        it->second.cancel_flag->store(true, std::memory_order_release);
      }
      return CancelStatus::Ok;
    }

    // PENDING / QUEUED / SCHEDULED / RETRYING: terminal immediately.
    queue_.remove(job_id);
    job.state = JobState::Cancelled;
    job.cancel_requested = true;
    job.finished_at_wall_ms = wall_ms();
    job.error = "cancelled by client";
    if (!store_->update_job(job, err)) return CancelStatus::NotFound;
    terminal_job = job;
    became_terminal = true;
  }
  if (became_terminal) {
    metrics_->jobs_cancelled.fetch_add(1, std::memory_order_relaxed);
    on_terminal(terminal_job);
    if (completion_cb_) completion_cb_(terminal_job);
    release_slot();
  }
  return CancelStatus::Ok;
}

// ---- attempt lifecycle --------------------------------------------------------------------

bool JobEngine::begin_attempt(const std::string& job_id, const std::string& owner,
                              int64_t eligible_at, bool remote, Job& out_job,
                              RunningInfo& out_info) {
  Job cancelled_job;
  Job failed_start_job;
  bool cancelled_now = false;
  bool started = false;
  bool failed_start = false;
  {
    std::lock_guard<std::mutex> lock(run_mutex_);
    Job job;
    if (!store_->get_job(job_id, job)) return false;
    if (is_terminal(job.state)) return false;
    if (job.state != JobState::Queued && job.state != JobState::Scheduled &&
        job.state != JobState::Retrying) {
      return false;  // already RUNNING (a stale queue entry) or still PENDING
    }
    if (job.cancel_requested) {
      job.state = JobState::Cancelled;
      job.finished_at_wall_ms = wall_ms();
      job.error = "cancelled by client";
      std::string err;
      store_->update_job(job, err);
      cancelled_job = job;
      cancelled_now = true;
    } else {
      const int64_t now_w = wall_ms();
      job.attempt += 1;
      job.state = JobState::Running;
      job.started_at_wall_ms = now_w;
      job.started_at_steady = now_ms();
      job.started_at_steady_us = now_us();
      job.queue_wait_us =
          eligible_at > 0 ? std::max<int64_t>(0, job.started_at_steady - eligible_at) * 1000 : 0;
      job.lease_token = random_u64() | 1ULL;  // never 0, so 0 means "no lease"
      job.lease_owner = owner;
      job.lease_expires_ms = 0;
      if (remote) {
        int64_t lease = cfg_.lease_duration_ms;
        // A job with its own deadline should not be held past it plus a grace period.
        if (job.timeout_ms > 0) lease = std::min(lease, job.timeout_ms + 2000);
        job.lease_expires_ms = now_w + lease;
      }
      std::string err;
      if (!store_->update_job(job, err)) {
        // The entry has already been popped. Without re-enqueuing it the job would stay
        // QUEUED in the store but absent from the queue, and would only run again after a
        // restart, so put it back rather than losing it.
        log_error("engine", concat({"cannot start ", job_id, ": ", err, "; requeuing"}));
        requeue_after_failed_start_ = true;
      }

      RunningInfo info;
      info.cancel_flag = std::make_shared<std::atomic<bool>>(false);
      info.cancel_reason = std::make_shared<std::atomic<int>>(static_cast<int>(CancelReason::None));
      info.lease_token = job.lease_token;
      info.owner = owner;
      info.attempt = job.attempt;
      info.deadline_wall_ms = job.timeout_ms > 0 ? now_w + job.timeout_ms : 0;
      info.lease_expires_ms = job.lease_expires_ms;
      info.started_steady = job.started_at_steady;
      info.started_steady_us = job.started_at_steady_us;
      info.started_wall = now_w;
      info.eligible_at_steady = eligible_at;
      info.remote = remote;
      if (requeue_after_failed_start_) {
        requeue_after_failed_start_ = false;
        failed_start_job = job;
        failed_start = true;
      } else {
        running_[job_id] = info;
        out_job = job;
        out_info = info;
        started = true;
      }
    }
  }
  if (failed_start) {
    failed_start_job.state = JobState::Queued;
    failed_start_job.attempt -= 1;
    enqueue(failed_start_job);
    return false;
  }
  if (cancelled_now) {
    metrics_->jobs_cancelled.fetch_add(1, std::memory_order_relaxed);
    on_terminal(cancelled_job);
    if (completion_cb_) completion_cb_(cancelled_job);
    release_slot();
    return false;
  }
  if (started) {
    metrics_->jobs_started.fetch_add(1, std::memory_order_relaxed);
    metrics_->attempts_started.fetch_add(1, std::memory_order_relaxed);
    metrics_->queue_wait.observe(out_job.queue_wait_us);
  }
  return started;
}

void JobEngine::finish_attempt(Job job, const RunningInfo& info, bool ok, Json result,
                               std::string error) {
  // Fencing: claim the right to finish this attempt. If the running entry is gone or
  // carries a different lease token, this attempt was superseded (lease expiry, or a
  // reassignment) and its result must be discarded.
  {
    std::lock_guard<std::mutex> lock(run_mutex_);
    const auto it = running_.find(job.job_id);
    if (it == running_.end() || it->second.lease_token != info.lease_token) {
      metrics_->stale_results_rejected.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    running_.erase(it);
  }

  const int reason = info.cancel_reason->load(std::memory_order_acquire);
  const int64_t now_w = wall_ms();
  const int64_t now_s = now_ms();
  const int64_t duration_us = now_us() - info.started_steady_us;
  const int64_t duration = duration_us / 1000;

  job.finished_at_wall_ms = now_w;
  job.finished_at_steady = now_s;
  job.exec_us = duration_us;
  job.lease_token = 0;
  job.lease_expires_ms = 0;

  JobState next = JobState::Succeeded;
  std::string attempt_status;
  if (reason == static_cast<int>(CancelReason::User)) {
    next = JobState::Cancelled;
    attempt_status = "CANCELLED";
    if (error.empty()) error = "cancelled by client";
  } else if (reason == static_cast<int>(CancelReason::Timeout)) {
    attempt_status = "TIMED_OUT";
    error = "deadline exceeded after " + num(job.timeout_ms) + "ms";
    next = (job.attempt < job.retry.max_attempts) ? JobState::Retrying : JobState::TimedOut;
  } else if (ok) {
    next = JobState::Succeeded;
    attempt_status = "SUCCEEDED";
  } else {
    attempt_status = "FAILED";
    next = (job.attempt < job.retry.max_attempts) ? JobState::Retrying : JobState::Failed;
  }

  AttemptRecord rec;
  rec.job_id = job.job_id;
  rec.attempt = job.attempt;
  rec.status = attempt_status;
  rec.started_ms = info.started_wall;
  rec.finished_ms = now_w;
  rec.duration_ms = duration;
  rec.worker = info.owner;
  rec.error = error;
  std::string err;
  if (!store_->append_attempt(rec, err)) {
    log_error("engine", concat({"attempt record failed for ", job.job_id, ": ", err}));
  }
  metrics_->exec.observe(duration_us);

  if (next == JobState::Retrying) {
    const int64_t delay = job.retry.delay_after_attempt(job.attempt);
    job.state = JobState::Retrying;
    job.error = error;
    job.result = Json();
    job.ready_at_wall_ms = now_w + delay;
    job.finished_at_wall_ms = 0;  // not finished; it will run again
    persist(job);
    metrics_->attempts_retried.fetch_add(1, std::memory_order_relaxed);
    enqueue(job);
    return;
  }

  job.state = next;
  job.result = (next == JobState::Succeeded) ? std::move(result) : Json();
  job.error = (next == JobState::Succeeded) ? std::string() : error;
  persist(job);

  switch (next) {
    case JobState::Succeeded: metrics_->jobs_succeeded.fetch_add(1, std::memory_order_relaxed); break;
    case JobState::Failed: metrics_->jobs_failed.fetch_add(1, std::memory_order_relaxed); break;
    case JobState::TimedOut: metrics_->jobs_timed_out.fetch_add(1, std::memory_order_relaxed); break;
    case JobState::Cancelled: metrics_->jobs_cancelled.fetch_add(1, std::memory_order_relaxed); break;
    default: break;
  }
  if (job.created_at_ms > 0) {
    metrics_->end_to_end.observe((now_w - job.created_at_ms) * 1000);
  }
  on_terminal(job);
  if (completion_cb_) completion_cb_(job);
  release_slot();
}

bool JobEngine::execute_entry(const QueueEntry& entry, const std::string& owner) {
  Job job;
  RunningInfo info;
  if (!begin_attempt(entry.job_id, owner, entry.eligible_at_steady, false, job, info)) {
    return false;
  }
  JobHandler fn;
  if (!handlers_->get(job.type, fn)) {
    finish_attempt(std::move(job), info, false, Json(),
                   "no handler registered for type " + job.type);
    return true;
  }
  bool ok = false;
  Json result;
  std::string error;
  // The handler runs with no engine lock held; a slow handler must never block submission.
  try {
    JobContext ctx(job.job_id, job.payload, job.attempt, info.cancel_flag,
                   info.deadline_wall_ms);
    JobOutcome outcome = fn(ctx);
    ok = outcome.ok;
    result = std::move(outcome.result);
    error = std::move(outcome.error);
  } catch (const std::exception& e) {
    ok = false;
    error = std::string("handler threw: ") + e.what();
  } catch (...) {
    ok = false;
    error = "handler threw a non-standard exception";
  }
  finish_attempt(std::move(job), info, ok, std::move(result), std::move(error));
  return true;
}

// ---- threads ----------------------------------------------------------------------------

void JobEngine::worker_loop(int index) {
  const std::string owner = cfg_.node_name + "-w" + num(static_cast<int64_t>(index));
  for (;;) {
    auto entry = queue_.pop();
    if (!entry) break;  // queue closed and drained
    if (stop_requested_.load() && !drain_.load()) {
      // Abandon the entry: its row stays QUEUED and is recovered on the next start.
      break;
    }
    execute_entry(*entry, owner);
  }
}

void JobEngine::watchdog_loop() {
  while (!stop_requested_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.watchdog_interval_ms));
    const int64_t now_w = wall_ms();

    std::vector<std::string> expired_leases;
    std::vector<std::string> dead_workers;
    {
      std::lock_guard<std::mutex> lock(run_mutex_);
      for (auto& kv : running_) {
        RunningInfo& info = kv.second;
        if (info.deadline_wall_ms > 0 && now_w > info.deadline_wall_ms &&
            info.cancel_reason->load(std::memory_order_relaxed) ==
                static_cast<int>(CancelReason::None)) {
          info.cancel_reason->store(static_cast<int>(CancelReason::Timeout),
                                    std::memory_order_relaxed);
          info.cancel_flag->store(true, std::memory_order_release);
        }
        // Lease expiry applies to remote workers only: reassigning a hung in-process
        // handler to another thread of the same process would just duplicate the work.
        if (info.remote && info.lease_expires_ms > 0 && now_w > info.lease_expires_ms) {
          expired_leases.push_back(kv.first);
        }
      }
      for (auto& kv : remote_workers_) {
        if (kv.second.alive &&
            now_w - kv.second.last_heartbeat_ms > cfg_.worker_heartbeat_timeout_ms) {
          kv.second.alive = false;
          // Counted under the same lock that clears the flag, so an observer can never
          // see a worker already gone from alive_workers() but not yet counted as dead.
          metrics_->workers_marked_dead.fetch_add(1, std::memory_order_relaxed);
          dead_workers.push_back(kv.first);
        }
      }
    }

    for (const std::string& wid : dead_workers) {
      std::string err;
      store_->set_worker_state(wid, "DEAD", err);
      log_warn("engine", concat({"worker ", wid, " missed heartbeats; marking DEAD"}));
      // Expire that worker's leases immediately rather than waiting for lease_expires_ms.
      std::vector<std::string> owned;
      {
        std::lock_guard<std::mutex> lock(run_mutex_);
        for (const auto& kv : running_) {
          if (kv.second.remote && kv.second.owner == wid) owned.push_back(kv.first);
        }
      }
      expired_leases.insert(expired_leases.end(), owned.begin(), owned.end());
    }

    std::sort(expired_leases.begin(), expired_leases.end());
    expired_leases.erase(std::unique(expired_leases.begin(), expired_leases.end()),
                         expired_leases.end());

    for (const std::string& job_id : expired_leases) {
      Job terminal_job;
      bool terminal = false;
      bool requeued = false;
      {
        std::lock_guard<std::mutex> lock(run_mutex_);
        const auto it = running_.find(job_id);
        if (it == running_.end()) continue;
        running_.erase(it);  // any later result from that worker is now stale

        Job job;
        if (!store_->get_job(job_id, job)) continue;
        if (is_terminal(job.state) || job.state != JobState::Running) continue;
        job.lease_token = 0;
        job.lease_expires_ms = 0;
        job.lease_owner.clear();
        std::string err;
        if (job.attempt >= job.retry.max_attempts) {
          job.state = JobState::Failed;
          job.error = "lease expired and retry attempts exhausted";
          job.finished_at_wall_ms = now_w;
          store_->update_job(job, err);
          terminal_job = job;
          terminal = true;
        } else {
          job.state = JobState::Queued;
          job.ready_at_wall_ms = now_w;
          job.error = "lease expired; reassigned";
          store_->update_job(job, err);
          requeued = true;
          terminal_job = job;  // reused as the row to enqueue
        }
      }
      metrics_->leases_expired.fetch_add(1, std::memory_order_relaxed);
      if (terminal) {
        metrics_->jobs_failed.fetch_add(1, std::memory_order_relaxed);
        on_terminal(terminal_job);
        if (completion_cb_) completion_cb_(terminal_job);
        release_slot();
      } else if (requeued) {
        metrics_->jobs_reassigned.fetch_add(1, std::memory_order_relaxed);
        log_warn("engine", concat({"lease expired for ", job_id, "; requeued"}));
        enqueue(terminal_job);
      }
    }
  }
}

void JobEngine::start() {
  if (started_) return;
  started_ = true;
  stop_requested_.store(false);
  for (int i = 0; i < cfg_.worker_threads; ++i) {
    workers_.emplace_back([this, i] { worker_loop(i); });
  }
  watchdog_ = std::thread([this] { watchdog_loop(); });
  log_info("engine", concat({"started with ", num(static_cast<int64_t>(cfg_.worker_threads)),
                             " worker threads"}));
}

void JobEngine::stop(bool drain) {
  if (!started_) return;
  drain_.store(drain);
  if (drain) {
    // Let the queue empty before telling workers to stop pulling.
    const int64_t deadline = now_ms() + 30000;
    while (now_ms() < deadline && (queue_.size() > 0 || running_count() > 0)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  stop_requested_.store(true);
  queue_.close();
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
  workers_.clear();
  if (watchdog_.joinable()) watchdog_.join();
  started_ = false;
  std::string err;
  store_->checkpoint(err);
  log_info("engine", "stopped");
}

// ---- workflow DAG --------------------------------------------------------------------------

WorkflowStatus JobEngine::submit_workflow(
    const std::string& name, const std::vector<WorkflowNodeSpec>& nodes,
    const std::vector<std::pair<std::string, std::string>>& edges,
    std::string& out_workflow_id, std::vector<std::pair<std::string, std::string>>& out_node_ids,
    std::string& err) {
  if (stop_requested_.load()) { err = "engine is stopping"; return WorkflowStatus::Stopped; }
  if (nodes.empty()) { err = "workflow has no nodes"; return WorkflowStatus::Empty; }

  std::unordered_map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (!index.emplace(nodes[i].name, i).second) {
      err = "duplicate node name: " + nodes[i].name;
      return WorkflowStatus::DuplicateNode;
    }
    if (!handlers_->has(nodes[i].request.type)) {
      err = "unknown job type '" + nodes[i].request.type + "' for node " + nodes[i].name;
      return WorkflowStatus::UnknownType;
    }
  }

  // Validate edges and build the adjacency / indegree structure by node index.
  std::vector<std::vector<std::size_t>> adj(nodes.size());
  std::vector<int> indegree(nodes.size(), 0);
  for (const auto& e : edges) {
    const auto from = index.find(e.first);
    const auto to = index.find(e.second);
    if (from == index.end()) { err = "edge references unknown node: " + e.first;
                               return WorkflowStatus::UnknownNodeRef; }
    if (to == index.end()) { err = "edge references unknown node: " + e.second;
                             return WorkflowStatus::UnknownNodeRef; }
    if (from->second == to->second) { err = "self edge on node: " + e.first;
                                      return WorkflowStatus::SelfEdge; }
    adj[from->second].push_back(to->second);
    indegree[to->second] += 1;
  }

  // Kahn's algorithm: if fewer than N nodes can be ordered, the graph has a cycle.
  {
    std::vector<int> deg = indegree;
    std::deque<std::size_t> ready;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (deg[i] == 0) ready.push_back(i);
    }
    std::size_t visited = 0;
    while (!ready.empty()) {
      const std::size_t n = ready.front();
      ready.pop_front();
      ++visited;
      for (const std::size_t m : adj[n]) {
        if (--deg[m] == 0) ready.push_back(m);
      }
    }
    if (visited != nodes.size()) {
      err = "workflow contains a cycle";
      return WorkflowStatus::Cycle;
    }
  }

  // Reserve one admission slot per node so a workflow is accepted whole or not at all.
  std::size_t reserved = 0;
  for (; reserved < nodes.size(); ++reserved) {
    if (!reserve_slot()) break;
  }
  if (reserved < nodes.size()) {
    for (std::size_t i = 0; i < reserved; ++i) release_slot();
    metrics_->jobs_rejected_queue_full.fetch_add(1, std::memory_order_relaxed);
    err = "engine at capacity for a workflow of " + num(static_cast<int64_t>(nodes.size())) +
          " nodes";
    return WorkflowStatus::QueueFull;
  }

  const std::string wf_id = new_id('w');
  const int64_t created = wall_ms();
  std::vector<Job> job_rows;
  job_rows.reserve(nodes.size());
  std::vector<std::string> ids(nodes.size());

  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const SubmitRequest& req = nodes[i].request;
    Job job;
    job.job_id = new_id('j');
    ids[i] = job.job_id;
    job.type = req.type;
    job.payload = req.payload;
    job.priority = req.priority;
    job.retry = req.retry;
    if (job.retry.max_attempts < 1) job.retry.max_attempts = 1;
    job.timeout_ms = req.timeout_ms;
    job.created_at_ms = created;
    job.ready_at_wall_ms = created + std::max<int64_t>(0, req.delay_ms);
    job.enqueued_at_steady = now_ms();
    job.workflow_id = wf_id;
    job.workflow_node = nodes[i].name;
    job.pending_deps = indegree[i];
    job.seq = seq_counter_.fetch_add(1, std::memory_order_relaxed);
    job.state = indegree[i] > 0 ? JobState::Pending
                                : (req.delay_ms > 0 ? JobState::Scheduled : JobState::Queued);
    job_rows.push_back(std::move(job));
  }

  std::vector<std::pair<std::string, std::string>> id_edges;
  id_edges.reserve(edges.size());
  for (const auto& e : edges) {
    id_edges.emplace_back(ids[index[e.first]], ids[index[e.second]]);
  }

  WorkflowRecord wf;
  wf.workflow_id = wf_id;
  wf.name = name;
  wf.state = "RUNNING";
  wf.created_at_ms = created;
  wf.node_count = static_cast<int>(nodes.size());

  if (!store_->insert_workflow(wf, job_rows, id_edges, err)) {
    for (std::size_t i = 0; i < nodes.size(); ++i) release_slot();
    return WorkflowStatus::StoreError;
  }

  {
    std::lock_guard<std::mutex> lock(wf_mutex_);
    for (const auto& e : id_edges) children_[e.first].push_back(e.second);
    for (const Job& j : job_rows) {
      pending_deps_[j.job_id] = j.pending_deps;
      wf_nodes_[wf_id].insert(j.job_id);
    }
    wf_remaining_[wf_id] = static_cast<int>(nodes.size());
    wf_failures_[wf_id] = 0;
  }

  metrics_->workflows_created.fetch_add(1, std::memory_order_relaxed);
  metrics_->jobs_submitted.fetch_add(static_cast<uint64_t>(nodes.size()),
                                     std::memory_order_relaxed);
  for (const Job& j : job_rows) {
    if (j.state != JobState::Pending) enqueue(j);
  }

  out_workflow_id = wf_id;
  out_node_ids.clear();
  for (std::size_t i = 0; i < nodes.size(); ++i) out_node_ids.emplace_back(nodes[i].name, ids[i]);
  return WorkflowStatus::Ok;
}

CancelStatus JobEngine::cancel_workflow(const std::string& workflow_id, std::string& err) {
  WorkflowRecord wf;
  if (!store_->get_workflow(workflow_id, wf)) return CancelStatus::NotFound;
  if (wf.state != "RUNNING") return CancelStatus::AlreadyTerminal;

  // Mark the workflow terminal FIRST. Cancelling its nodes makes them reach terminal
  // states, and the last one would otherwise finalise the workflow as FAILED before this
  // line ran, leaving a cancelled workflow labelled as a failure.
  wf.state = "CANCELLED";
  wf.finished_at_ms = wall_ms();
  if (!store_->update_workflow(wf, err)) return CancelStatus::NotFound;

  for (const Job& j : store_->list_workflow_jobs(workflow_id)) {
    if (!is_terminal(j.state)) {
      std::string ignored;
      cancel(j.job_id, ignored);
    }
  }
  return CancelStatus::Ok;
}

void JobEngine::on_terminal(const Job& job) {
  if (job.workflow_id.empty()) return;
  int skipped = 0;
  if (job.state == JobState::Succeeded) {
    release_dependents(job.job_id);
  } else {
    skipped = skip_descendants(job.job_id);
  }

  // Completion is decided by a counter under one lock, not by re-reading every node and
  // checking whether they all look terminal: two threads finishing the last two nodes
  // would both see "all terminal" and both finalise the workflow.
  bool finished = false;
  bool failed = false;
  {
    std::lock_guard<std::mutex> lock(wf_mutex_);
    if (job.state != JobState::Succeeded) wf_failures_[job.workflow_id] += 1 + skipped;
    const auto it = wf_remaining_.find(job.workflow_id);
    if (it == wf_remaining_.end()) return;
    it->second -= (1 + skipped);
    if (it->second <= 0) {
      finished = true;
      failed = wf_failures_[job.workflow_id] > 0;
      wf_remaining_.erase(it);
      wf_failures_.erase(job.workflow_id);
    }
  }
  if (finished) finalize_workflow(job.workflow_id, failed);
}

void JobEngine::release_dependents(const std::string& parent_job_id) {
  std::vector<std::string> newly_ready;
  {
    std::lock_guard<std::mutex> lock(wf_mutex_);
    const auto it = children_.find(parent_job_id);
    if (it != children_.end()) {
      for (const std::string& child : it->second) {
        const auto p = pending_deps_.find(child);
        if (p == pending_deps_.end()) continue;
        if (p->second > 0 && --p->second == 0) newly_ready.push_back(child);
      }
    }
  }
  for (const std::string& child_id : newly_ready) {
    Job child;
    if (!store_->get_job(child_id, child)) continue;
    if (child.state != JobState::Pending) continue;  // cancelled or already handled
    child.state = (child.ready_at_wall_ms > wall_ms()) ? JobState::Scheduled : JobState::Queued;
    child.pending_deps = 0;
    persist(child);
    enqueue(child);
  }
}

int JobEngine::skip_descendants(const std::string& failed_job_id) {
  // Breadth-first over the DAG. Every reachable descendant becomes SKIPPED, because it can
  // never satisfy its "all parents SUCCEEDED" precondition.
  std::vector<std::string> frontier;
  {
    std::lock_guard<std::mutex> lock(wf_mutex_);
    const auto it = children_.find(failed_job_id);
    if (it == children_.end()) return 0;
    frontier = it->second;
  }
  std::unordered_set<std::string> seen;
  std::vector<Job> to_skip;
  while (!frontier.empty()) {
    const std::string id = frontier.back();
    frontier.pop_back();
    if (!seen.insert(id).second) continue;
    Job job;
    if (!store_->get_job(id, job)) continue;
    if (!is_terminal(job.state)) {
      queue_.remove(id);
      job.state = JobState::Skipped;
      job.finished_at_wall_ms = wall_ms();
      job.error = "skipped: an upstream dependency did not succeed";
      persist(job);
      to_skip.push_back(job);
    }
    std::lock_guard<std::mutex> lock(wf_mutex_);
    const auto ch = children_.find(id);
    if (ch != children_.end()) {
      for (const std::string& c : ch->second) frontier.push_back(c);
    }
  }
  if (!to_skip.empty()) {
    metrics_->jobs_skipped.fetch_add(static_cast<uint64_t>(to_skip.size()),
                                     std::memory_order_relaxed);
    for (const Job& skipped : to_skip) {
      if (completion_cb_) completion_cb_(skipped);
      release_slot();
    }
  }
  return static_cast<int>(to_skip.size());
}

void JobEngine::finalize_workflow(const std::string& workflow_id, bool failed) {
  if (workflow_id.empty()) return;
  WorkflowRecord wf;
  if (!store_->get_workflow(workflow_id, wf)) return;
  // A workflow cancelled by a client is already terminal; do not relabel it.
  if (wf.state != "RUNNING") return;
  wf.state = failed ? "FAILED" : "SUCCEEDED";
  wf.finished_at_ms = wall_ms();
  std::string err;
  store_->update_workflow(wf, err);
  if (failed) metrics_->workflows_failed.fetch_add(1, std::memory_order_relaxed);
  else metrics_->workflows_succeeded.fetch_add(1, std::memory_order_relaxed);
}

// ---- remote worker protocol ------------------------------------------------------------------

std::string JobEngine::register_worker(const std::string& name, int capacity) {
  const std::string id = new_id('k');
  WorkerInfo info;
  info.worker_id = id;
  info.name = name;
  info.capacity = capacity > 0 ? capacity : 1;
  info.last_heartbeat_ms = wall_ms();
  info.alive = true;
  {
    std::lock_guard<std::mutex> lock(run_mutex_);
    remote_workers_[id] = info;
  }
  WorkerRecord rec;
  rec.worker_id = id;
  rec.name = name;
  rec.capacity = info.capacity;
  rec.state = "ALIVE";
  rec.registered_ms = info.last_heartbeat_ms;
  rec.last_heartbeat_ms = info.last_heartbeat_ms;
  std::string err;
  store_->upsert_worker(rec, err);
  metrics_->workers_registered.fetch_add(1, std::memory_order_relaxed);
  log_info("engine", concat({"worker registered: ", name, " (", id, ")"}));
  return id;
}

bool JobEngine::heartbeat_worker(const std::string& worker_id,
                                 std::vector<std::string>& out_cancelled) {
  out_cancelled.clear();
  const int64_t now_w = wall_ms();
  {
    std::lock_guard<std::mutex> lock(run_mutex_);
    const auto it = remote_workers_.find(worker_id);
    if (it == remote_workers_.end()) return false;
    it->second.last_heartbeat_ms = now_w;
    it->second.alive = true;
    // Extend the lease of everything this worker still holds: a live worker keeps its work.
    for (auto& kv : running_) {
      if (kv.second.remote && kv.second.owner == worker_id) {
        if (kv.second.cancel_flag->load(std::memory_order_acquire)) {
          out_cancelled.push_back(kv.first);
        }
        kv.second.lease_expires_ms = now_w + cfg_.lease_duration_ms;
      }
    }
  }
  std::string err;
  store_->touch_worker(worker_id, now_w, err);
  return true;
}

std::vector<LeasedJob> JobEngine::claim(const std::string& worker_id, std::size_t max_jobs,
                                        int64_t wait_ms) {
  std::vector<LeasedJob> out;
  {
    std::lock_guard<std::mutex> lock(run_mutex_);
    const auto it = remote_workers_.find(worker_id);
    if (it == remote_workers_.end()) return out;
    it->second.last_heartbeat_ms = wall_ms();
    it->second.alive = true;
  }
  if (stop_requested_.load()) return out;

  const std::vector<QueueEntry> entries = queue_.pop_batch(max_jobs, wait_ms);
  for (const QueueEntry& e : entries) {
    Job job;
    RunningInfo info;
    if (!begin_attempt(e.job_id, worker_id, e.eligible_at_steady, true, job, info)) continue;
    LeasedJob lj;
    lj.job_id = job.job_id;
    lj.type = job.type;
    lj.payload = job.payload;
    lj.attempt = job.attempt;
    lj.lease_token = job.lease_token;
    lj.timeout_ms = job.timeout_ms;
    lj.lease_expires_ms = job.lease_expires_ms;
    out.push_back(std::move(lj));
  }
  return out;
}

CompleteStatus JobEngine::complete(const std::string& worker_id, const std::string& job_id,
                                   int attempt, uint64_t lease_token, const std::string& status,
                                   const Json& result, const std::string& error) {
  RunningInfo info;
  {
    std::lock_guard<std::mutex> lock(run_mutex_);
    const auto it = running_.find(job_id);
    if (it == running_.end()) {
      metrics_->stale_results_rejected.fetch_add(1, std::memory_order_relaxed);
      return CompleteStatus::StaleLease;
    }
    // Fencing token check: the result is accepted only from the worker that currently
    // holds the lease, for the attempt that lease was issued for.
    if (it->second.lease_token != lease_token || it->second.owner != worker_id ||
        it->second.attempt != attempt) {
      metrics_->stale_results_rejected.fetch_add(1, std::memory_order_relaxed);
      return CompleteStatus::StaleLease;
    }
    info = it->second;
  }

  Job job;
  if (!store_->get_job(job_id, job)) return CompleteStatus::NotFound;
  if (status == "TIMED_OUT") {
    info.cancel_reason->store(static_cast<int>(CancelReason::Timeout), std::memory_order_relaxed);
  } else if (status == "CANCELLED") {
    info.cancel_reason->store(static_cast<int>(CancelReason::User), std::memory_order_relaxed);
  }
  const bool ok = status == "SUCCEEDED";
  finish_attempt(std::move(job), info, ok, result, error);
  return CompleteStatus::Accepted;
}

// ---- recovery --------------------------------------------------------------------------------

int JobEngine::recover() {
  const std::vector<Job> pending = store_->load_non_terminal_jobs();
  const auto deps = store_->load_all_dependencies();
  // Rebuild the per-workflow completion counters, or a workflow recovered mid-flight
  // would never be finalised.
  std::unordered_map<std::string, int> remaining, failures;
  {
    std::unordered_set<std::string> workflow_ids;
    for (const Job& j : pending) {
      if (!j.workflow_id.empty()) workflow_ids.insert(j.workflow_id);
    }
    for (const std::string& wid : workflow_ids) {
      for (const Job& j : store_->list_workflow_jobs(wid)) {
        if (!is_terminal(j.state)) remaining[wid] += 1;
        else if (j.state != JobState::Succeeded) failures[wid] += 1;
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(wf_mutex_);
    for (const auto& e : deps) children_[e.first].push_back(e.second);
    for (const Job& j : pending) {
      pending_deps_[j.job_id] = j.pending_deps;
      if (!j.workflow_id.empty()) wf_nodes_[j.workflow_id].insert(j.job_id);
    }
    for (const auto& kv : remaining) wf_remaining_[kv.first] = kv.second;
    for (const auto& kv : failures) wf_failures_[kv.first] = kv.second;
  }

  int64_t max_seq = 0;
  int recovered = 0;
  for (Job job : pending) {
    max_seq = std::max(max_seq, job.seq);
    outstanding_.fetch_add(1, std::memory_order_relaxed);
    if (job.state == JobState::Running) {
      // Interrupted mid-execution. At-least-once: it goes back on the queue. The attempt
      // counter already includes the interrupted attempt, so a job cannot loop forever.
      job.state = JobState::Queued;
      job.lease_token = 0;
      job.lease_owner.clear();
      job.lease_expires_ms = 0;
      job.ready_at_wall_ms = wall_ms();
      job.error = "recovered after coordinator restart";
      persist(job);
    }
    if (job.state == JobState::Queued || job.state == JobState::Scheduled ||
        job.state == JobState::Retrying) {
      enqueue(job);
    }
    ++recovered;
  }
  seq_counter_.store(max_seq + 1, std::memory_order_relaxed);
  if (recovered > 0) {
    log_info("engine", concat({"recovered ", num(static_cast<int64_t>(recovered)),
                               " non-terminal jobs from the store"}));
  }
  return recovered;
}

}  // namespace je
