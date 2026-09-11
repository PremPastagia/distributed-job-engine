#pragma once

#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "jobengine/job.hpp"

namespace je {

using JobHandler = std::function<JobOutcome(const JobContext&)>;

// Type -> handler. Registration is expected to happen during start-up, but the registry
// is read from every worker thread, so lookups take a shared lock.
class HandlerRegistry {
 public:
  void register_handler(const std::string& type, JobHandler fn);
  bool has(const std::string& type) const;
  // Returns a copy: the caller executes it after every engine lock has been released,
  // so handing out a reference into the map would be a lifetime hazard.
  bool get(const std::string& type, JobHandler& out) const;
  std::vector<std::string> types() const;
  std::size_t size() const;

  // Built-in workloads used by the tests and the benchmark suite.
  //   noop                 - returns immediately (measures engine overhead)
  //   echo                 - returns the payload
  //   sleep {ms}           - cooperative I/O-bound stand-in, polls cancellation
  //   sleep_uncooperative {ms} - ignores cancellation; demonstrates the D-07 limitation
  //   cpu_fib {n}          - CPU-bound recursive fibonacci
  //   cpu_hash {iterations}- CPU-bound integer mixing loop
  //   file_lines {path}    - I/O-bound: counts lines/bytes of a file
  //   fail {message, fail_until_attempt} - fails until the given attempt, then succeeds
  //   flaky {fail_probability_pct}       - fails randomly, for failure-heavy benchmarks
  static void register_builtins(HandlerRegistry& reg);

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, JobHandler> handlers_;
};

}  // namespace je
