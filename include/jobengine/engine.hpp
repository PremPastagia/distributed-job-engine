#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jobengine/handlers.hpp"
#include "jobengine/job.hpp"
#include "jobengine/metrics.hpp"
#include "jobengine/queue.hpp"
#include "jobengine/store.hpp"

namespace je {

struct EngineConfig {
  int worker_threads = 4;              // embedded execution threads (0 = remote workers only)
  std::size_t max_outstanding_jobs = 100000;  // admission control: non-terminal jobs
  int64_t watchdog_interval_ms = 25;
  int64_t lease_duration_ms = 30000;   // how long a remote worker may hold a job
  int64_t worker_heartbeat_timeout_ms = 6000;
  bool drain_on_shutdown = false;      // true: finish queued work; false: leave it QUEUED
  std::string node_name = "coordinator";
};

struct SubmitRequest {
  std::string type;
  Json payload = Json::object();
  int priority = 0;
  RetryPolicy retry;
  int64_t timeout_ms = 0;
  int64_t delay_ms = 0;
  std::string workflow_id;
  std::string workflow_node;
  int pending_deps = 0;
};

struct WorkflowNodeSpec {
  std::string name;
  SubmitRequest request;
};

struct LeasedJob {
  std::string job_id;
  std::string type;
  Json payload;
  int attempt = 0;
  uint64_t lease_token = 0;
  int64_t timeout_ms = 0;
  int64_t lease_expires_ms = 0;  // wall clock
};

enum class SubmitStatus { Ok, UnknownType, QueueFull, StoreError, Stopped };
enum class CancelStatus { Ok, NotFound, AlreadyTerminal };
enum class WorkflowStatus {
  Ok, Empty, UnknownType, DuplicateNode, UnknownNodeRef, SelfEdge, Cycle, QueueFull, StoreError, Stopped
};
enum class CompleteStatus { Accepted, StaleLease, NotFound };

const char* to_string(SubmitStatus s);
const char* to_string(WorkflowStatus s);

// The scheduler and executor.
//
// Ownership: the engine owns the queue, the worker threads and the watchdog. It borrows
// the store, the handler registry and the metrics, all of which outlive it.
class JobEngine {
 public:
  JobEngine(EngineConfig cfg, JobStore* store, HandlerRegistry* handlers, Metrics* metrics);
  ~JobEngine();

  JobEngine(const JobEngine&) = delete;
  JobEngine& operator=(const JobEngine&) = delete;

  // Re-queues non-terminal jobs left behind by a previous run and rebuilds the workflow
  // graph. Returns the number of jobs recovered. Call before start().
  int recover();
  void start();
  // Graceful shutdown. drain==true finishes everything already queued; drain==false stops
  // pulling new work and leaves queued jobs QUEUED in the store for the next run.
  void stop(bool drain);
  bool running() const { return started_ && !stop_requested_.load(); }

  SubmitStatus submit(const SubmitRequest& req, std::string& out_job_id, std::string& err);
  bool get_job(const std::string& job_id, Job& out) const;
  CancelStatus cancel(const std::string& job_id, std::string& err);

  WorkflowStatus submit_workflow(const std::string& name,
                                 const std::vector<WorkflowNodeSpec>& nodes,
                                 const std::vector<std::pair<std::string, std::string>>& edges,
                                 std::string& out_workflow_id,
                                 std::vector<std::pair<std::string, std::string>>& out_node_ids,
                                 std::string& err);
  CancelStatus cancel_workflow(const std::string& workflow_id, std::string& err);

  // --- remote worker protocol (Phase 5) ---
  std::string register_worker(const std::string& name, int capacity);
  bool heartbeat_worker(const std::string& worker_id, std::vector<std::string>& out_cancelled);
  std::vector<LeasedJob> claim(const std::string& worker_id, std::size_t max_jobs,
                               int64_t wait_ms);
  CompleteStatus complete(const std::string& worker_id, const std::string& job_id, int attempt,
                          uint64_t lease_token, const std::string& status, const Json& result,
                          const std::string& error);

  // --- introspection ---
  std::size_t queue_depth() const { return queue_.size(); }
  std::size_t ready_depth() const { return queue_.ready_size(); }
  std::size_t delayed_depth() const { return queue_.delayed_size(); }
  std::size_t running_count() const;
  std::size_t outstanding() const { return outstanding_.load(); }
  std::size_t alive_workers() const;
  int worker_threads() const { return cfg_.worker_threads; }
  const EngineConfig& config() const { return cfg_; }
  // Blocks until no job is outstanding or the timeout elapses. Test/benchmark helper.
  bool wait_idle(int64_t timeout_ms) const;
  // Invoked once per job when it reaches a terminal state, on the finishing thread and
  // with no engine lock held. Set before start(). Used by the benchmark harness to time
  // completions precisely instead of polling the store.
  void set_completion_callback(std::function<void(const Job&)> cb) {
    completion_cb_ = std::move(cb);
  }

 private:
  enum class CancelReason : int { None = 0, Timeout = 1, User = 2, LeaseExpired = 3 };

  struct RunningInfo {
    std::shared_ptr<std::atomic<bool>> cancel_flag;
    std::shared_ptr<std::atomic<int>> cancel_reason;
    uint64_t lease_token = 0;
    std::string owner;
    int attempt = 0;
    int64_t deadline_wall_ms = 0;   // 0 == no deadline
    int64_t lease_expires_ms = 0;   // wall
    int64_t started_steady = 0;
    int64_t started_steady_us = 0;
    int64_t started_wall = 0;
    int64_t eligible_at_steady = 0;
    bool remote = false;  // lease expiry / reassignment applies to remote workers only
  };

  struct WorkerInfo {
    std::string worker_id;
    std::string name;
    int capacity = 1;
    int64_t last_heartbeat_ms = 0;
    bool alive = true;
  };

  void worker_loop(int index);
  void watchdog_loop();
  // Executes one popped entry on the calling thread. Returns false if the entry was not
  // executable (already terminal, cancelled, or missing).
  bool execute_entry(const QueueEntry& entry, const std::string& owner);
  // Transitions a job into RUNNING and records the lease. Returns false if it is no longer
  // eligible. On success, out_job holds the RUNNING record and out_info the lease details.
  bool begin_attempt(const std::string& job_id, const std::string& owner, int64_t eligible_at,
                     bool remote, Job& out_job, RunningInfo& out_info);
  // Records a finished attempt and applies the retry / terminal decision.
  void finish_attempt(Job job, const RunningInfo& info, bool ok, Json result,
                      std::string error);
  void on_terminal(const Job& job);
  void release_dependents(const std::string& parent_job_id);
  // Returns how many descendants were newly marked SKIPPED.
  int skip_descendants(const std::string& failed_job_id);
  void finalize_workflow(const std::string& workflow_id, bool failed);
  void enqueue(const Job& job);
  void persist(const Job& job);
  bool reserve_slot();
  void release_slot();

  EngineConfig cfg_;
  JobStore* store_;
  HandlerRegistry* handlers_;
  Metrics* metrics_;
  PriorityJobQueue queue_;

  std::vector<std::thread> workers_;
  std::thread watchdog_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> drain_{false};
  bool started_ = false;
  std::function<void(const Job&)> completion_cb_;
  // Set inside begin_attempt when persisting the RUNNING transition fails; read and
  // cleared in the same critical section.
  bool requeue_after_failed_start_ = false;

  std::atomic<int64_t> seq_counter_{1};
  std::atomic<std::size_t> outstanding_{0};

  mutable std::mutex run_mutex_;
  mutable std::condition_variable idle_cv_;
  std::unordered_map<std::string, RunningInfo> running_;
  std::unordered_map<std::string, WorkerInfo> remote_workers_;

  mutable std::mutex wf_mutex_;
  std::unordered_map<std::string, std::vector<std::string>> children_;  // parent -> children
  std::unordered_map<std::string, int> pending_deps_;                   // child -> count
  std::unordered_map<std::string, std::unordered_set<std::string>> wf_nodes_;
  std::unordered_map<std::string, int> wf_remaining_;
  std::unordered_map<std::string, int> wf_failures_;
};

}  // namespace je
