#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "jobengine/json.hpp"

namespace je {

// Lock-free latency histogram with fixed, documented bucket edges (microseconds).
// This exists for live observability only. Every number reported in BENCHMARKS.md is
// computed offline from raw per-job samples, because bucketed percentiles are estimates
// (docs/DESIGN_DECISIONS.md D-13).
class Histogram {
 public:
  static constexpr std::size_t kBuckets = 24;
  void observe(int64_t microseconds);
  uint64_t count() const { return count_.load(std::memory_order_relaxed); }
  uint64_t sum_us() const { return sum_us_.load(std::memory_order_relaxed); }
  uint64_t max_us() const { return max_us_.load(std::memory_order_relaxed); }
  double mean_us() const;
  // Estimated percentile from bucket boundaries; upper edge of the containing bucket.
  double percentile_us(double p) const;
  Json to_json() const;
  void reset();
  static int64_t bucket_upper_us(std::size_t i);

 private:
  std::array<std::atomic<uint64_t>, kBuckets> buckets_{};
  std::atomic<uint64_t> count_{0};
  std::atomic<uint64_t> sum_us_{0};
  std::atomic<uint64_t> max_us_{0};
};

struct Metrics {
  std::atomic<uint64_t> jobs_submitted{0};
  std::atomic<uint64_t> jobs_started{0};
  std::atomic<uint64_t> jobs_succeeded{0};
  std::atomic<uint64_t> jobs_failed{0};
  std::atomic<uint64_t> jobs_timed_out{0};
  std::atomic<uint64_t> jobs_cancelled{0};
  std::atomic<uint64_t> jobs_skipped{0};
  std::atomic<uint64_t> attempts_started{0};
  std::atomic<uint64_t> attempts_retried{0};
  std::atomic<uint64_t> jobs_rejected_queue_full{0};
  std::atomic<uint64_t> leases_expired{0};
  std::atomic<uint64_t> jobs_reassigned{0};
  std::atomic<uint64_t> stale_results_rejected{0};
  std::atomic<uint64_t> workers_registered{0};
  std::atomic<uint64_t> workers_marked_dead{0};
  std::atomic<uint64_t> api_requests{0};
  std::atomic<uint64_t> api_client_errors{0};
  std::atomic<uint64_t> api_server_errors{0};
  std::atomic<uint64_t> workflows_created{0};
  std::atomic<uint64_t> workflows_succeeded{0};
  std::atomic<uint64_t> workflows_failed{0};

  Histogram queue_wait;   // enqueue -> start
  Histogram exec;         // start -> finish of one attempt
  Histogram end_to_end;   // submit -> terminal state
  Histogram api_latency;  // request received -> response written

  Json to_json() const;
};

}  // namespace je
