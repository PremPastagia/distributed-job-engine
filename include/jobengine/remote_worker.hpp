#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "jobengine/handlers.hpp"
#include "jobengine/http.hpp"

namespace je {

struct RemoteWorkerConfig {
  std::string host = "127.0.0.1";
  int port = 8080;
  std::string name = "worker";
  int threads = 4;             // concurrent jobs this process executes
  int64_t poll_wait_ms = 1000; // long-poll duration on /internal/claim
  int64_t heartbeat_ms = 0;    // 0 == use the interval the coordinator returns
};

// A worker process: registers with the coordinator, long-polls for leased jobs, executes
// them against the local handler registry, and reports results with the lease token it was
// given. It holds no queue and makes no scheduling decision; the coordinator owns both.
class RemoteWorker {
 public:
  RemoteWorker(RemoteWorkerConfig cfg, HandlerRegistry* handlers);
  ~RemoteWorker();

  bool start(std::string& err);
  void stop();
  const std::string& worker_id() const { return worker_id_; }
  uint64_t jobs_completed() const { return completed_.load(); }
  uint64_t jobs_failed() const { return failed_.load(); }
  uint64_t results_rejected() const { return rejected_.load(); }

 private:
  struct Active {
    std::shared_ptr<std::atomic<bool>> cancel;
    int64_t deadline_wall_ms = 0;  // 0 == no deadline
  };

  void poll_loop(int index);
  void heartbeat_loop();

  RemoteWorkerConfig cfg_;
  HandlerRegistry* handlers_;
  std::string worker_id_;
  int64_t heartbeat_interval_ms_ = 2000;
  std::atomic<bool> stopping_{false};
  std::vector<std::thread> threads_;
  std::thread heartbeat_;
  std::mutex mutex_;
  std::unordered_map<std::string, Active> active_;
  std::atomic<uint64_t> completed_{0};
  std::atomic<uint64_t> failed_{0};
  std::atomic<uint64_t> rejected_{0};
};

}  // namespace je
