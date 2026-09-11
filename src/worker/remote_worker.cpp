#include "jobengine/remote_worker.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "jobengine/json.hpp"
#include "jobengine/util.hpp"

namespace je {

RemoteWorker::RemoteWorker(RemoteWorkerConfig cfg, HandlerRegistry* handlers)
    : cfg_(std::move(cfg)), handlers_(handlers) {}

RemoteWorker::~RemoteWorker() { stop(); }

bool RemoteWorker::start(std::string& err) {
  HttpClient client(cfg_.host, cfg_.port, 10000);
  Json body = Json::object();
  body.set("name", Json(cfg_.name));
  body.set("capacity", Json(static_cast<int64_t>(cfg_.threads)));
  const HttpClientResponse res = client.post("/internal/workers/register", body.dump());
  if (!res.ok) { err = "cannot reach coordinator: " + res.error; return false; }
  if (res.status != 200) { err = "registration rejected: HTTP " + num(res.status) + " " + res.body;
                           return false; }
  const Json reply = Json::parse_or_null(res.body);
  worker_id_ = reply.get_string("worker_id", "");
  if (worker_id_.empty()) { err = "coordinator returned no worker_id"; return false; }
  heartbeat_interval_ms_ = cfg_.heartbeat_ms > 0
                               ? cfg_.heartbeat_ms
                               : std::max<int64_t>(250, reply.get_int("heartbeat_interval_ms", 2000));

  stopping_.store(false);
  for (int i = 0; i < cfg_.threads; ++i) {
    threads_.emplace_back([this, i] { poll_loop(i); });
  }
  heartbeat_ = std::thread([this] { heartbeat_loop(); });
  log_info("worker", concat({"registered as ", worker_id_, " (", cfg_.name, ") with ",
                             num(static_cast<int64_t>(cfg_.threads)), " execution threads"}));
  return true;
}

void RemoteWorker::stop() {
  if (stopping_.exchange(true)) return;
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
  threads_.clear();
  if (heartbeat_.joinable()) heartbeat_.join();
}

void RemoteWorker::heartbeat_loop() {
  HttpClient client(cfg_.host, cfg_.port, 5000);
  while (!stopping_.load()) {
    const int64_t next = now_ms() + heartbeat_interval_ms_;
    while (now_ms() < next && !stopping_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (stopping_.load()) break;

    Json body = Json::object();
    body.set("worker_id", Json(worker_id_));
    const HttpClientResponse res = client.post("/internal/workers/heartbeat", body.dump());
    if (!res.ok) {
      log_warn("worker", concat({"heartbeat failed: ", res.error}));
      continue;
    }
    // The coordinator answers with the jobs it wants cancelled; apply them locally.
    const Json reply = Json::parse_or_null(res.body);
    const Json* cancel = reply.find("cancel");
    const int64_t now_w = wall_ms();
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancel && cancel->is_array()) {
      for (std::size_t i = 0; i < cancel->size(); ++i) {
        const auto it = active_.find(cancel->at(i).as_string());
        if (it != active_.end()) it->second.cancel->store(true, std::memory_order_release);
      }
    }
    // Enforce the job deadline locally too, so a job stops as soon as it is over time
    // instead of waiting for the coordinator's lease to expire.
    for (auto& kv : active_) {
      if (kv.second.deadline_wall_ms > 0 && now_w > kv.second.deadline_wall_ms) {
        kv.second.cancel->store(true, std::memory_order_release);
      }
    }
  }
}

void RemoteWorker::poll_loop(int index) {
  HttpClient client(cfg_.host, cfg_.port, cfg_.poll_wait_ms + 30000);
  const std::string owner = cfg_.name + "-" + num(static_cast<int64_t>(index));

  while (!stopping_.load()) {
    Json claim = Json::object();
    claim.set("worker_id", Json(worker_id_));
    claim.set("max_jobs", Json(static_cast<int64_t>(1)));
    claim.set("wait_ms", Json(cfg_.poll_wait_ms));
    const HttpClientResponse res = client.post("/internal/claim", claim.dump());
    if (!res.ok) {
      if (!stopping_.load()) {
        log_warn("worker", concat({"claim failed: ", res.error}));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      continue;
    }
    if (res.status != 200) {
      log_warn("worker", concat({"claim rejected: HTTP ", num(res.status), " ", res.body}));
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    const Json reply = Json::parse_or_null(res.body);
    const Json* jobs = reply.find("jobs");
    if (!jobs || !jobs->is_array() || jobs->size() == 0) continue;

    for (std::size_t i = 0; i < jobs->size(); ++i) {
      const Json& j = jobs->at(i);
      const std::string job_id = j.get_string("job_id", "");
      const std::string type = j.get_string("type", "");
      const std::string lease_token = j.get_string("lease_token", "0");
      const int attempt = static_cast<int>(j.get_int("attempt", 1));
      const int64_t timeout_ms = j.get_int("timeout_ms", 0);
      const Json* payload = j.find("payload");
      const Json job_payload = payload ? *payload : Json::object();

      auto cancel = std::make_shared<std::atomic<bool>>(false);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        Active a;
        a.cancel = cancel;
        a.deadline_wall_ms = timeout_ms > 0 ? wall_ms() + timeout_ms : 0;
        active_[job_id] = a;
      }

      std::string status = "SUCCEEDED";
      Json result = Json::object();
      std::string error;
      JobHandler fn;
      if (!handlers_->get(type, fn)) {
        status = "FAILED";
        error = "worker has no handler registered for type " + type;
      } else {
        try {
          JobContext ctx(job_id, job_payload, attempt, cancel,
                         timeout_ms > 0 ? wall_ms() + timeout_ms : 0);
          const JobOutcome outcome = fn(ctx);
          if (cancel->load(std::memory_order_acquire)) {
            // The coordinator decides whether this counts as a timeout or a cancellation;
            // the worker reports TIMED_OUT only when it was its own deadline that fired.
            status = "TIMED_OUT";
            error = outcome.error.empty() ? "deadline exceeded" : outcome.error;
          } else if (outcome.ok) {
            result = outcome.result;
          } else {
            status = "FAILED";
            error = outcome.error;
          }
        } catch (const std::exception& e) {
          status = "FAILED";
          error = std::string("handler threw: ") + e.what();
        } catch (...) {
          status = "FAILED";
          error = "handler threw a non-standard exception";
        }
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        active_.erase(job_id);
      }

      Json done = Json::object();
      done.set("worker_id", Json(worker_id_));
      done.set("job_id", Json(job_id));
      done.set("attempt", Json(static_cast<int64_t>(attempt)));
      done.set("lease_token", Json(lease_token));
      done.set("status", Json(status));
      done.set("result", result);
      done.set("error", Json(error));
      const HttpClientResponse done_res = client.post("/internal/complete", done.dump());
      if (!done_res.ok) {
        log_warn("worker", concat({"failed to report ", job_id, ": ", done_res.error}));
        continue;
      }
      const Json done_reply = Json::parse_or_null(done_res.body);
      if (!done_reply.get_bool("accepted", false)) {
        // The lease was revoked while this attempt ran: the coordinator has already
        // reassigned the job, so the result is correctly discarded.
        rejected_.fetch_add(1, std::memory_order_relaxed);
        log_warn("worker", concat({"result for ", job_id, " rejected: ",
                                   done_reply.get_string("reason", "unknown")}));
        continue;
      }
      if (status == "SUCCEEDED") completed_.fetch_add(1, std::memory_order_relaxed);
      else failed_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

}  // namespace je
