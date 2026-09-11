#pragma once

#include <string>
#include <vector>

#include "jobengine/engine.hpp"
#include "jobengine/http.hpp"
#include "jobengine/metrics.hpp"
#include "jobengine/store.hpp"

namespace je {

// Maps HTTP requests onto engine operations.
//
// The route table is a single static data structure used both for dispatch and for
// introspection, so `GET /routes` reports exactly what the server can serve. The docs
// drift check (scripts/gates/g18_api_drift.mjs) compares that list against
// docs/ARCHITECTURE.md, which is why the table is exposed rather than hidden in a switch.
class ApiRouter {
 public:
  ApiRouter(JobEngine* engine, JobStore* store, Metrics* metrics, HandlerRegistry* handlers);

  void handle(const HttpRequest& req, HttpResponse& res);
  // "METHOD /path/{param}" for every implemented route.
  static std::vector<std::string> routes();

 private:
  bool dispatch(const HttpRequest& req, HttpResponse& res);

  void post_jobs(const HttpRequest& req, HttpResponse& res);
  void get_job(const std::string& id, HttpResponse& res);
  void get_job_attempts(const std::string& id, HttpResponse& res);
  void list_jobs(const HttpRequest& req, HttpResponse& res);
  void cancel_job(const std::string& id, HttpResponse& res);
  void post_workflows(const HttpRequest& req, HttpResponse& res);
  void get_workflow(const std::string& id, HttpResponse& res);
  void list_workflows(const HttpRequest& req, HttpResponse& res);
  void cancel_workflow(const std::string& id, HttpResponse& res);
  void health(HttpResponse& res);
  void metrics_endpoint(HttpResponse& res);
  void worker_register(const HttpRequest& req, HttpResponse& res);
  void worker_heartbeat(const HttpRequest& req, HttpResponse& res);
  void worker_claim(const HttpRequest& req, HttpResponse& res);
  void worker_complete(const HttpRequest& req, HttpResponse& res);

  JobEngine* engine_;
  JobStore* store_;
  Metrics* metrics_;
  HandlerRegistry* handlers_;
  int64_t started_wall_ms_;
};

}  // namespace je
