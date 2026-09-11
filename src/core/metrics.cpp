#include "jobengine/metrics.hpp"

#include <algorithm>

namespace je {
namespace {
// Bucket i covers (upper(i-1), upper(i)]; edges double every bucket from 16us to ~134s.
constexpr int64_t kBase = 16;
}  // namespace

int64_t Histogram::bucket_upper_us(std::size_t i) {
  int64_t v = kBase;
  for (std::size_t k = 0; k < i; ++k) v *= 2;
  return v;
}

void Histogram::observe(int64_t microseconds) {
  if (microseconds < 0) microseconds = 0;
  const uint64_t us = static_cast<uint64_t>(microseconds);
  std::size_t idx = 0;
  while (idx + 1 < kBuckets && microseconds > bucket_upper_us(idx)) ++idx;
  buckets_[idx].fetch_add(1, std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);
  sum_us_.fetch_add(us, std::memory_order_relaxed);
  uint64_t prev = max_us_.load(std::memory_order_relaxed);
  while (us > prev && !max_us_.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
  }
}

double Histogram::mean_us() const {
  const uint64_t n = count();
  return n == 0 ? 0.0 : static_cast<double>(sum_us()) / static_cast<double>(n);
}

double Histogram::percentile_us(double p) const {
  const uint64_t n = count();
  if (n == 0) return 0.0;
  const double target = (p / 100.0) * static_cast<double>(n);
  uint64_t cumulative = 0;
  for (std::size_t i = 0; i < kBuckets; ++i) {
    cumulative += buckets_[i].load(std::memory_order_relaxed);
    if (static_cast<double>(cumulative) >= target) {
      return static_cast<double>(bucket_upper_us(i));
    }
  }
  return static_cast<double>(bucket_upper_us(kBuckets - 1));
}

void Histogram::reset() {
  for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
  count_.store(0, std::memory_order_relaxed);
  sum_us_.store(0, std::memory_order_relaxed);
  max_us_.store(0, std::memory_order_relaxed);
}

Json Histogram::to_json() const {
  Json j = Json::object();
  j.set("count", Json(static_cast<int64_t>(count())));
  j.set("mean_ms", Json(mean_us() / 1000.0));
  j.set("max_ms", Json(static_cast<double>(max_us()) / 1000.0));
  j.set("p50_ms_est", Json(percentile_us(50) / 1000.0));
  j.set("p95_ms_est", Json(percentile_us(95) / 1000.0));
  j.set("p99_ms_est", Json(percentile_us(99) / 1000.0));
  return j;
}

Json Metrics::to_json() const {
  Json c = Json::object();
  const auto put = [&c](const char* k, const std::atomic<uint64_t>& v) {
    c.set(k, Json(static_cast<int64_t>(v.load(std::memory_order_relaxed))));
  };
  put("jobs_submitted", jobs_submitted);
  put("jobs_started", jobs_started);
  put("jobs_succeeded", jobs_succeeded);
  put("jobs_failed", jobs_failed);
  put("jobs_timed_out", jobs_timed_out);
  put("jobs_cancelled", jobs_cancelled);
  put("jobs_skipped", jobs_skipped);
  put("attempts_started", attempts_started);
  put("attempts_retried", attempts_retried);
  put("jobs_rejected_queue_full", jobs_rejected_queue_full);
  put("leases_expired", leases_expired);
  put("jobs_reassigned", jobs_reassigned);
  put("stale_results_rejected", stale_results_rejected);
  put("workers_registered", workers_registered);
  put("workers_marked_dead", workers_marked_dead);
  put("api_requests", api_requests);
  put("api_client_errors", api_client_errors);
  put("api_server_errors", api_server_errors);
  put("workflows_created", workflows_created);
  put("workflows_succeeded", workflows_succeeded);
  put("workflows_failed", workflows_failed);

  Json lat = Json::object();
  lat.set("queue_wait", queue_wait.to_json());
  lat.set("exec", exec.to_json());
  lat.set("end_to_end", end_to_end.to_json());
  lat.set("api", api_latency.to_json());

  Json out = Json::object();
  out.set("counters", c);
  out.set("latency", lat);
  out.set("note", Json(std::string(
                     "latency percentiles are histogram estimates (bucket upper edges); "
                     "benchmark numbers are computed from raw samples, see BENCHMARKS.md")));
  return out;
}

}  // namespace je
