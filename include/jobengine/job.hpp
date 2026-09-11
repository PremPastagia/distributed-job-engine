#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "jobengine/json.hpp"

namespace je {

// ---- lifecycle ---------------------------------------------------------------------
// Ten states. Every transition in the system goes through is_valid_transition(), so the
// state machine is defined in exactly one place and can be exhaustively tested.
enum class JobState {
  Pending,    // waiting on unmet workflow dependencies
  Queued,     // eligible now, in the ready heap
  Scheduled,  // eligible at a future time (submitted with delay_ms)
  Running,    // leased to a worker
  Retrying,   // previous attempt failed; waiting out backoff
  Succeeded,  // terminal
  Failed,     // terminal: attempts exhausted after handler failure
  TimedOut,   // terminal: attempts exhausted after deadline expiry
  Cancelled,  // terminal: cancelled by a client
  Skipped,    // terminal: an ancestor in the workflow failed
};

const char* to_string(JobState s);
bool state_from_string(std::string_view s, JobState& out);
bool is_terminal(JobState s);
bool is_failure_terminal(JobState s);  // Failed | TimedOut | Cancelled | Skipped
bool is_valid_transition(JobState from, JobState to);
const std::vector<JobState>& all_states();

// ---- retry -------------------------------------------------------------------------
struct RetryPolicy {
  int max_attempts = 1;              // 1 == execute once, never retry (safe default, D-08)
  int64_t backoff_base_ms = 100;
  double backoff_multiplier = 2.0;
  int64_t backoff_max_ms = 30000;
  bool jitter = false;               // off by default so backoff timing is testable

  // Delay before the next attempt, given the 1-based attempt number that just failed.
  int64_t delay_after_attempt(int failed_attempt) const;
};

// ---- handler contract ---------------------------------------------------------------
struct JobOutcome {
  bool ok = false;
  Json result;
  std::string error;

  static JobOutcome success(Json r = Json::object()) { return JobOutcome{true, std::move(r), {}}; }
  static JobOutcome failure(std::string e) { return JobOutcome{false, Json(), std::move(e)}; }
};

// Handed to every handler. Cancellation is cooperative (D-07): a handler is expected to
// poll cancel_requested() at natural boundaries, exactly like Go's context.Context.
class JobContext {
 public:
  JobContext(std::string job_id, const Json& payload, int attempt,
             std::shared_ptr<std::atomic<bool>> cancel, int64_t deadline_ms)
      : job_id_(std::move(job_id)),
        payload_(payload),
        attempt_(attempt),
        cancel_(std::move(cancel)),
        deadline_ms_(deadline_ms) {}

  const std::string& job_id() const { return job_id_; }
  const Json& payload() const { return payload_; }
  int attempt() const { return attempt_; }
  // 0 means "no deadline". Otherwise a steady-clock millisecond stamp.
  int64_t deadline_ms() const { return deadline_ms_; }
  bool cancel_requested() const {
    return cancel_ && cancel_->load(std::memory_order_relaxed);
  }
  // Sleeps up to ms, waking early if cancellation is requested. Returns false if cancelled.
  bool sleep_or_cancel(int64_t ms) const;

 private:
  std::string job_id_;
  const Json& payload_;
  int attempt_;
  std::shared_ptr<std::atomic<bool>> cancel_;
  int64_t deadline_ms_;
};

// ---- the record ----------------------------------------------------------------------
// Two clocks, kept explicitly separate (docs/ARCHITECTURE.md §4):
//   *_wall_ms   system clock, persisted, human-facing, meaningful across restarts
//   *_steady    steady clock, in-memory only, used for every latency measurement
// Durations (queue_wait_ms / exec_ms) are computed from the steady clock and persisted,
// because a duration stays meaningful across processes while a steady stamp does not.
struct Job {
  std::string job_id;
  std::string type;
  Json payload = Json::object();
  int priority = 0;
  JobState state = JobState::Queued;
  int attempt = 0;                   // attempts started so far
  RetryPolicy retry;
  int64_t timeout_ms = 0;            // 0 == no deadline

  int64_t created_at_ms = 0;         // wall
  int64_t started_at_wall_ms = 0;    // wall, 0 until first start
  int64_t finished_at_wall_ms = 0;   // wall, 0 until terminal
  int64_t ready_at_wall_ms = 0;      // wall, when the job becomes eligible

  int64_t enqueued_at_steady = 0;    // steady, start of the queue-wait measurement
  int64_t started_at_steady = 0;     // steady
  int64_t started_at_steady_us = 0;  // steady, microseconds
  int64_t finished_at_steady = 0;    // steady
  int64_t ready_at_steady = 0;       // steady, what the queue actually schedules on

  // Durations are stored in MICROSECONDS: millisecond resolution is too coarse to
  // measure scheduling overhead, and a duration stays meaningful across processes while a
  // steady-clock stamp does not. JSON exposes them as fractional milliseconds.
  int64_t queue_wait_us = 0;         // measured, persisted
  int64_t exec_us = 0;               // measured, persisted

  Json result;
  std::string error;
  std::string workflow_id;
  std::string workflow_node;         // node name inside the workflow, for readable output
  int pending_deps = 0;
  uint64_t lease_token = 0;
  std::string lease_owner;
  int64_t lease_expires_ms = 0;      // wall, so an expiry survives a coordinator restart
  bool cancel_requested = false;
  int64_t seq = 0;                   // FIFO tie-break within a priority level

  Json to_json() const;
};

}  // namespace je
