#pragma once
// Shared benchmark plumbing: exact percentiles from raw samples and an environment header
// written into every CSV, so a result file records the machine it came from.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#include <sys/resource.h>
#include <vector>

#include "jobengine/util.hpp"

namespace je {

// Nearest-rank percentile on sorted samples: the smallest value at or above p% of the
// distribution. Stated explicitly because "p95" is ambiguous between interpolation methods.
inline int64_t percentile(std::vector<int64_t>& sorted_samples, double p) {
  if (sorted_samples.empty()) return 0;
  const double rank = (p / 100.0) * static_cast<double>(sorted_samples.size());
  std::size_t idx = static_cast<std::size_t>(std::ceil(rank));
  if (idx == 0) idx = 1;
  if (idx > sorted_samples.size()) idx = sorted_samples.size();
  return sorted_samples[idx - 1];
}

inline std::string run_command(const std::string& cmd) {
  std::string out;
  FILE* p = ::popen(cmd.c_str(), "r");
  if (!p) return out;
  char buf[512];
  while (std::fgets(buf, sizeof(buf), p)) out += buf;
  ::pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

inline std::string environment_block(const std::string& prefix) {
  std::string s;
  const auto line = [&](const std::string& k, const std::string& v) {
    s += prefix + k + "=" + v + "\n";
  };
  line("timestamp_utc", run_command("date -u +%Y-%m-%dT%H:%M:%SZ"));
  line("os", run_command("uname -srm"));
  line("cpu", run_command("sysctl -n machdep.cpu.brand_string 2>/dev/null || "
                          "grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- || "
                          "echo unknown"));
  line("logical_cores", num(static_cast<int64_t>(std::thread::hardware_concurrency())));
  line("compiler", run_command("c++ --version 2>/dev/null | head -1"));
  line("build_type", CMAKE_BUILD_TYPE_STRING);
  return s;
}

// Resource usage of this process, for the CPU-utilisation and memory columns. Reported
// from getrusage rather than sampled externally, so it covers exactly the measured run.
struct ResourceUsage {
  double user_cpu_s = 0.0;
  double system_cpu_s = 0.0;
  double peak_rss_mb = 0.0;
  int64_t voluntary_ctx_switches = 0;
  int64_t involuntary_ctx_switches = 0;

  static ResourceUsage capture() {
    ResourceUsage u;
    struct rusage r {};
    if (::getrusage(RUSAGE_SELF, &r) != 0) return u;
    u.user_cpu_s = static_cast<double>(r.ru_utime.tv_sec) + r.ru_utime.tv_usec / 1e6;
    u.system_cpu_s = static_cast<double>(r.ru_stime.tv_sec) + r.ru_stime.tv_usec / 1e6;
    // ru_maxrss is bytes on macOS and kilobytes on Linux.
#if defined(__APPLE__)
    u.peak_rss_mb = static_cast<double>(r.ru_maxrss) / (1024.0 * 1024.0);
#else
    u.peak_rss_mb = static_cast<double>(r.ru_maxrss) / 1024.0;
#endif
    u.voluntary_ctx_switches = static_cast<int64_t>(r.ru_nvcsw);
    u.involuntary_ctx_switches = static_cast<int64_t>(r.ru_nivcsw);
    return u;
  }

  double total_cpu_s() const { return user_cpu_s + system_cpu_s; }
  // Cores busy on average: 1.0 means one core fully used for the whole run.
  double cores_used(double wall_s) const {
    return wall_s > 0 ? total_cpu_s() / wall_s : 0.0;
  }
};

struct SummaryStats {
  std::size_t count = 0;
  int64_t min_us = 0, max_us = 0, p50 = 0, p90 = 0, p95 = 0, p99 = 0;
  double mean_us = 0.0;

  static SummaryStats from(std::vector<int64_t> samples) {
    SummaryStats st;
    if (samples.empty()) return st;
    std::sort(samples.begin(), samples.end());
    st.count = samples.size();
    st.min_us = samples.front();
    st.max_us = samples.back();
    double sum = 0;
    for (const int64_t v : samples) sum += static_cast<double>(v);
    st.mean_us = sum / static_cast<double>(samples.size());
    st.p50 = percentile(samples, 50);
    st.p90 = percentile(samples, 90);
    st.p95 = percentile(samples, 95);
    st.p99 = percentile(samples, 99);
    return st;
  }

  std::string to_line(const std::string& label) const {
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "%s: n=%zu mean=%.3fms p50=%.3fms p90=%.3fms p95=%.3fms p99=%.3fms "
                  "min=%.3fms max=%.3fms",
                  label.c_str(), count, mean_us / 1000.0, static_cast<double>(p50) / 1000.0,
                  static_cast<double>(p90) / 1000.0, static_cast<double>(p95) / 1000.0,
                  static_cast<double>(p99) / 1000.0, static_cast<double>(min_us) / 1000.0,
                  static_cast<double>(max_us) / 1000.0);
    return buf;
  }
};

}  // namespace je
