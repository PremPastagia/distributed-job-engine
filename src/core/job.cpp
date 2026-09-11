#include "jobengine/job.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "jobengine/util.hpp"

namespace je {

const char* to_string(JobState s) {
  switch (s) {
    case JobState::Pending: return "PENDING";
    case JobState::Queued: return "QUEUED";
    case JobState::Scheduled: return "SCHEDULED";
    case JobState::Running: return "RUNNING";
    case JobState::Retrying: return "RETRYING";
    case JobState::Succeeded: return "SUCCEEDED";
    case JobState::Failed: return "FAILED";
    case JobState::TimedOut: return "TIMED_OUT";
    case JobState::Cancelled: return "CANCELLED";
    case JobState::Skipped: return "SKIPPED";
  }
  return "UNKNOWN";
}

const std::vector<JobState>& all_states() {
  static const std::vector<JobState> kAll = {
      JobState::Pending,   JobState::Queued,  JobState::Scheduled, JobState::Running,
      JobState::Retrying,  JobState::Succeeded, JobState::Failed,  JobState::TimedOut,
      JobState::Cancelled, JobState::Skipped};
  return kAll;
}

bool state_from_string(std::string_view s, JobState& out) {
  for (const JobState st : all_states()) {
    if (s == to_string(st)) { out = st; return true; }
  }
  return false;
}

bool is_terminal(JobState s) {
  switch (s) {
    case JobState::Succeeded:
    case JobState::Failed:
    case JobState::TimedOut:
    case JobState::Cancelled:
    case JobState::Skipped:
      return true;
    default:
      return false;
  }
}

bool is_failure_terminal(JobState s) { return is_terminal(s) && s != JobState::Succeeded; }

bool is_valid_transition(JobState from, JobState to) {
  if (from == to) return false;      // a no-op transition is a bug, not a transition
  if (is_terminal(from)) return false;  // terminal states are absorbing
  switch (from) {
    case JobState::Pending:
      return to == JobState::Queued || to == JobState::Scheduled ||
             to == JobState::Cancelled || to == JobState::Skipped;
    case JobState::Scheduled:
      return to == JobState::Queued || to == JobState::Cancelled ||
             to == JobState::Skipped;
    case JobState::Queued:
      return to == JobState::Running || to == JobState::Cancelled ||
             to == JobState::Skipped;
    case JobState::Running:
      // Queued is reachable from Running only via lease expiry / worker death, where the
      // coordinator reclaims the job and makes it eligible again.
      return to == JobState::Succeeded || to == JobState::Retrying ||
             to == JobState::Failed || to == JobState::TimedOut ||
             to == JobState::Cancelled || to == JobState::Queued;
    case JobState::Retrying:
      return to == JobState::Queued || to == JobState::Cancelled ||
             to == JobState::Failed || to == JobState::TimedOut ||
             to == JobState::Skipped;
    default:
      return false;
  }
}

int64_t RetryPolicy::delay_after_attempt(int failed_attempt) const {
  if (failed_attempt < 1) failed_attempt = 1;
  double d = static_cast<double>(backoff_base_ms);
  // backoff_base * multiplier^(failed_attempt - 1), computed iteratively so a large
  // attempt count cannot overflow through pow().
  for (int i = 1; i < failed_attempt; ++i) {
    d *= backoff_multiplier;
    if (d >= static_cast<double>(backoff_max_ms)) break;
  }
  int64_t delay = static_cast<int64_t>(std::llround(d));
  delay = std::min(delay, backoff_max_ms);
  delay = std::max<int64_t>(delay, 0);
  if (jitter && delay > 0) {
    delay = static_cast<int64_t>(random_u64() % static_cast<uint64_t>(delay + 1));
  }
  return delay;
}

bool JobContext::sleep_or_cancel(int64_t ms) const {
  // Poll in short slices so cancellation latency is bounded by the slice, not by ms.
  constexpr int64_t kSlice = 5;
  const int64_t end = now_ms() + ms;
  for (;;) {
    if (cancel_requested()) return false;
    const int64_t remaining = end - now_ms();
    if (remaining <= 0) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(kSlice, remaining)));
  }
}

Json Job::to_json() const {
  Json j = Json::object();
  j.set("job_id", Json(job_id));
  j.set("type", Json(type));
  j.set("payload", payload);
  j.set("priority", Json(static_cast<int64_t>(priority)));
  j.set("state", Json(std::string(to_string(state))));
  j.set("attempt", Json(static_cast<int64_t>(attempt)));
  j.set("max_attempts", Json(static_cast<int64_t>(retry.max_attempts)));
  j.set("timeout_ms", Json(timeout_ms));
  j.set("created_at_ms", Json(created_at_ms));
  j.set("started_at_ms", started_at_wall_ms ? Json(started_at_wall_ms) : Json());
  j.set("finished_at_ms", finished_at_wall_ms ? Json(finished_at_wall_ms) : Json());
  j.set("queue_wait_ms", Json(static_cast<double>(queue_wait_us) / 1000.0));
  j.set("exec_ms", Json(static_cast<double>(exec_us) / 1000.0));
  j.set("queue_wait_us", Json(queue_wait_us));
  j.set("exec_us", Json(exec_us));
  j.set("result", result);
  j.set("error", error.empty() ? Json() : Json(error));
  j.set("workflow_id", workflow_id.empty() ? Json() : Json(workflow_id));
  j.set("workflow_node", workflow_node.empty() ? Json() : Json(workflow_node));
  j.set("pending_deps", Json(static_cast<int64_t>(pending_deps)));
  j.set("lease_owner", lease_owner.empty() ? Json() : Json(lease_owner));
  return j;
}

}  // namespace je
