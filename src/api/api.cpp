#include "jobengine/api.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include "jobengine/util.hpp"

namespace je {
namespace {

struct Route {
  const char* method;
  const char* pattern;
};

// The single source of truth for what this server serves.
const Route kRoutes[] = {
    {"POST", "/jobs"},
    {"GET", "/jobs"},
    {"GET", "/jobs/{id}"},
    {"GET", "/jobs/{id}/attempts"},
    {"POST", "/jobs/{id}/cancel"},
    {"POST", "/workflows"},
    {"GET", "/workflows"},
    {"GET", "/workflows/{id}"},
    {"POST", "/workflows/{id}/cancel"},
    {"GET", "/health"},
    {"GET", "/metrics"},
    {"GET", "/routes"},
    {"POST", "/internal/workers/register"},
    {"POST", "/internal/workers/heartbeat"},
    {"POST", "/internal/claim"},
    {"POST", "/internal/complete"},
};

// Matches "/jobs/{id}/cancel" against "/jobs/j_ABC/cancel", capturing the placeholder.
bool match(const std::string& pattern, const std::string& path, std::string& captured) {
  const std::vector<std::string> p = split(pattern, '/');
  const std::vector<std::string> a = split(path, '/');
  if (p.size() != a.size()) return false;
  for (std::size_t i = 0; i < p.size(); ++i) {
    if (p[i].size() >= 2 && p[i].front() == '{' && p[i].back() == '}') {
      if (a[i].empty()) return false;
      captured = a[i];
      continue;
    }
    if (p[i] != a[i]) return false;
  }
  return true;
}

void error_json(HttpResponse& res, int status, const std::string& message) {
  Json j = Json::object();
  j.set("error", Json(message));
  res.json(status, j.dump());
}

bool parse_body(const HttpRequest& req, HttpResponse& res, Json& out) {
  if (req.body.empty()) {
    error_json(res, 400, "request body is required and must be a JSON object");
    return false;
  }
  std::string err;
  if (!Json::parse(req.body, out, err)) {
    error_json(res, 400, "invalid JSON: " + err);
    return false;
  }
  if (!out.is_object()) {
    error_json(res, 400, "request body must be a JSON object");
    return false;
  }
  return true;
}

// Reads the shared job-parameter block used by POST /jobs and by workflow nodes.
bool read_submit_request(const Json& body, SubmitRequest& out, std::string& err) {
  out.type = body.get_string("type", "");
  if (out.type.empty()) { err = "field 'type' is required and must be a string"; return false; }
  const Json* payload = body.find("payload");
  if (payload) {
    if (!payload->is_object()) { err = "field 'payload' must be an object"; return false; }
    out.payload = *payload;
  }
  out.priority = static_cast<int>(body.get_int("priority", 0));
  const int64_t max_attempts = body.get_int("max_attempts", 1);
  if (max_attempts < 1 || max_attempts > 100) {
    err = "field 'max_attempts' must be between 1 and 100";
    return false;
  }
  out.retry.max_attempts = static_cast<int>(max_attempts);
  out.retry.backoff_base_ms = body.get_int("backoff_base_ms", 100);
  out.retry.backoff_multiplier = body.get_double("backoff_multiplier", 2.0);
  out.retry.backoff_max_ms = body.get_int("backoff_max_ms", 30000);
  out.retry.jitter = body.get_bool("backoff_jitter", false);
  if (out.retry.backoff_base_ms < 0 || out.retry.backoff_max_ms < 0) {
    err = "backoff values must not be negative";
    return false;
  }
  if (out.retry.backoff_multiplier < 1.0) {
    err = "field 'backoff_multiplier' must be >= 1.0";
    return false;
  }
  out.timeout_ms = body.get_int("timeout_ms", 0);
  if (out.timeout_ms < 0) { err = "field 'timeout_ms' must not be negative"; return false; }
  out.delay_ms = body.get_int("delay_ms", 0);
  if (out.delay_ms < 0) { err = "field 'delay_ms' must not be negative"; return false; }
  return true;
}

std::string u64_to_string(uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
  return buf;
}

uint64_t string_to_u64(const std::string& s) {
  return std::strtoull(s.c_str(), nullptr, 10);
}

}  // namespace

ApiRouter::ApiRouter(JobEngine* engine, JobStore* store, Metrics* metrics,
                     HandlerRegistry* handlers)
    : engine_(engine),
      store_(store),
      metrics_(metrics),
      handlers_(handlers),
      started_wall_ms_(wall_ms()) {}

std::vector<std::string> ApiRouter::routes() {
  std::vector<std::string> out;
  for (const Route& r : kRoutes) {
    out.push_back(std::string(r.method) + " " + r.pattern);
  }
  return out;
}

void ApiRouter::handle(const HttpRequest& req, HttpResponse& res) {
  const int64_t t0 = now_us();
  metrics_->api_requests.fetch_add(1, std::memory_order_relaxed);
  if (!dispatch(req, res)) {
    // Distinguish "no such path" from "path exists, wrong method", which is what a client
    // debugging an integration actually needs to know.
    bool path_exists = false;
    for (const Route& r : kRoutes) {
      std::string captured;
      if (match(r.pattern, req.path, captured)) { path_exists = true; break; }
    }
    if (path_exists) error_json(res, 405, "method " + req.method + " not allowed for " + req.path);
    else error_json(res, 404, "no such endpoint: " + req.path);
  }
  if (res.status >= 500) metrics_->api_server_errors.fetch_add(1, std::memory_order_relaxed);
  else if (res.status >= 400) metrics_->api_client_errors.fetch_add(1, std::memory_order_relaxed);
  metrics_->api_latency.observe(now_us() - t0);
}

bool ApiRouter::dispatch(const HttpRequest& req, HttpResponse& res) {
  std::string id;
  if (req.method == "POST" && match("/jobs", req.path, id)) { post_jobs(req, res); return true; }
  if (req.method == "GET" && match("/jobs", req.path, id)) { list_jobs(req, res); return true; }
  if (req.method == "GET" && match("/jobs/{id}", req.path, id)) { get_job(id, res); return true; }
  if (req.method == "GET" && match("/jobs/{id}/attempts", req.path, id)) {
    get_job_attempts(id, res);
    return true;
  }
  if (req.method == "POST" && match("/jobs/{id}/cancel", req.path, id)) {
    cancel_job(id, res);
    return true;
  }
  if (req.method == "POST" && match("/workflows", req.path, id)) {
    post_workflows(req, res);
    return true;
  }
  if (req.method == "GET" && match("/workflows", req.path, id)) {
    list_workflows(req, res);
    return true;
  }
  if (req.method == "GET" && match("/workflows/{id}", req.path, id)) {
    get_workflow(id, res);
    return true;
  }
  if (req.method == "POST" && match("/workflows/{id}/cancel", req.path, id)) {
    cancel_workflow(id, res);
    return true;
  }
  if (req.method == "GET" && match("/health", req.path, id)) { health(res); return true; }
  if (req.method == "GET" && match("/metrics", req.path, id)) { metrics_endpoint(res); return true; }
  if (req.method == "GET" && match("/routes", req.path, id)) {
    Json arr = Json::array();
    for (const std::string& r : routes()) arr.push_back(Json(r));
    Json out = Json::object();
    out.set("routes", arr);
    res.json(200, out.dump());
    return true;
  }
  if (req.method == "POST" && match("/internal/workers/register", req.path, id)) {
    worker_register(req, res);
    return true;
  }
  if (req.method == "POST" && match("/internal/workers/heartbeat", req.path, id)) {
    worker_heartbeat(req, res);
    return true;
  }
  if (req.method == "POST" && match("/internal/claim", req.path, id)) {
    worker_claim(req, res);
    return true;
  }
  if (req.method == "POST" && match("/internal/complete", req.path, id)) {
    worker_complete(req, res);
    return true;
  }
  return false;
}

// ---- jobs -------------------------------------------------------------------------------

void ApiRouter::post_jobs(const HttpRequest& req, HttpResponse& res) {
  Json body;
  if (!parse_body(req, res, body)) return;
  SubmitRequest sr;
  std::string err;
  if (!read_submit_request(body, sr, err)) { error_json(res, 400, err); return; }

  std::string job_id;
  const SubmitStatus st = engine_->submit(sr, job_id, err);
  switch (st) {
    case SubmitStatus::Ok: {
      Job job;
      Json out = Json::object();
      out.set("job_id", Json(job_id));
      out.set("state", Json(engine_->get_job(job_id, job) ? std::string(to_string(job.state))
                                                          : std::string("QUEUED")));
      res.json(201, out.dump());
      return;
    }
    case SubmitStatus::UnknownType: {
      std::vector<std::string> known = handlers_->types();
      std::sort(known.begin(), known.end());
      Json j = Json::object();
      j.set("error", Json(err));
      Json arr = Json::array();
      for (const std::string& t : known) arr.push_back(Json(t));
      j.set("known_types", arr);
      res.json(400, j.dump());
      return;
    }
    case SubmitStatus::QueueFull: error_json(res, 503, err); return;
    case SubmitStatus::Stopped: error_json(res, 503, err); return;
    case SubmitStatus::StoreError: error_json(res, 500, err); return;
  }
}

void ApiRouter::get_job(const std::string& id, HttpResponse& res) {
  Job job;
  if (!engine_->get_job(id, job)) { error_json(res, 404, "job not found: " + id); return; }
  res.json(200, job.to_json().dump());
}

void ApiRouter::get_job_attempts(const std::string& id, HttpResponse& res) {
  Job job;
  if (!engine_->get_job(id, job)) { error_json(res, 404, "job not found: " + id); return; }
  Json arr = Json::array();
  for (const AttemptRecord& a : store_->list_attempts(id)) {
    Json j = Json::object();
    j.set("attempt", Json(static_cast<int64_t>(a.attempt)));
    j.set("status", Json(a.status));
    j.set("started_at_ms", Json(a.started_ms));
    j.set("finished_at_ms", Json(a.finished_ms));
    j.set("duration_ms", Json(a.duration_ms));
    j.set("worker", a.worker.empty() ? Json() : Json(a.worker));
    j.set("error", a.error.empty() ? Json() : Json(a.error));
    arr.push_back(std::move(j));
  }
  Json out = Json::object();
  out.set("job_id", Json(id));
  out.set("attempts", arr);
  res.json(200, out.dump());
}

void ApiRouter::list_jobs(const HttpRequest& req, HttpResponse& res) {
  std::optional<JobState> filter;
  const std::string state_param = req.query_param("state");
  if (!state_param.empty()) {
    JobState s;
    if (!state_from_string(state_param, s)) {
      error_json(res, 400, "unknown state filter: " + state_param);
      return;
    }
    filter = s;
  }
  int64_t limit = 50, offset = 0;
  const std::string limit_param = req.query_param("limit");
  if (!limit_param.empty() && (!parse_int(limit_param, limit) || limit < 1 || limit > 1000)) {
    error_json(res, 400, "limit must be an integer between 1 and 1000");
    return;
  }
  const std::string offset_param = req.query_param("offset");
  if (!offset_param.empty() && (!parse_int(offset_param, offset) || offset < 0)) {
    error_json(res, 400, "offset must be a non-negative integer");
    return;
  }

  const std::vector<Job> jobs =
      store_->list_jobs(filter, static_cast<int>(limit), static_cast<int>(offset));
  Json arr = Json::array();
  for (const Job& j : jobs) arr.push_back(j.to_json());
  Json out = Json::object();
  out.set("jobs", arr);
  out.set("count", Json(static_cast<int64_t>(jobs.size())));
  out.set("total", Json(store_->count_jobs(filter)));
  res.json(200, out.dump());
}

void ApiRouter::cancel_job(const std::string& id, HttpResponse& res) {
  std::string err;
  const CancelStatus st = engine_->cancel(id, err);
  if (st == CancelStatus::NotFound) { error_json(res, 404, "job not found: " + id); return; }
  if (st == CancelStatus::AlreadyTerminal) {
    error_json(res, 409, "job is already in a terminal state: " + id);
    return;
  }
  Job job;
  engine_->get_job(id, job);
  Json out = Json::object();
  out.set("job_id", Json(id));
  out.set("state", Json(std::string(to_string(job.state))));
  out.set("cancel_requested", Json(true));
  res.json(200, out.dump());
}

// ---- workflows ----------------------------------------------------------------------------

void ApiRouter::post_workflows(const HttpRequest& req, HttpResponse& res) {
  Json body;
  if (!parse_body(req, res, body)) return;
  const std::string name = body.get_string("name", "workflow");
  const Json* nodes = body.find("nodes");
  if (!nodes || !nodes->is_array() || nodes->size() == 0) {
    error_json(res, 400, "field 'nodes' must be a non-empty array");
    return;
  }
  std::vector<WorkflowNodeSpec> specs;
  for (std::size_t i = 0; i < nodes->size(); ++i) {
    const Json& n = nodes->at(i);
    if (!n.is_object()) { error_json(res, 400, "every node must be an object"); return; }
    WorkflowNodeSpec spec;
    spec.name = n.get_string("name", "");
    if (spec.name.empty()) {
      error_json(res, 400, "every node needs a non-empty 'name'");
      return;
    }
    std::string err;
    if (!read_submit_request(n, spec.request, err)) {
      error_json(res, 400, "node '" + spec.name + "': " + err);
      return;
    }
    specs.push_back(std::move(spec));
  }

  std::vector<std::pair<std::string, std::string>> edges;
  const Json* edge_array = body.find("edges");
  if (edge_array) {
    if (!edge_array->is_array()) { error_json(res, 400, "field 'edges' must be an array"); return; }
    for (std::size_t i = 0; i < edge_array->size(); ++i) {
      const Json& e = edge_array->at(i);
      if (!e.is_object()) { error_json(res, 400, "every edge must be an object"); return; }
      const std::string from = e.get_string("from", "");
      const std::string to = e.get_string("to", "");
      if (from.empty() || to.empty()) {
        error_json(res, 400, "every edge needs non-empty 'from' and 'to'");
        return;
      }
      edges.emplace_back(from, to);
    }
  }

  std::string wf_id, err;
  std::vector<std::pair<std::string, std::string>> node_ids;
  const WorkflowStatus st = engine_->submit_workflow(name, specs, edges, wf_id, node_ids, err);
  if (st != WorkflowStatus::Ok) {
    const int code = (st == WorkflowStatus::QueueFull || st == WorkflowStatus::Stopped) ? 503
                     : (st == WorkflowStatus::StoreError) ? 500
                                                          : 400;
    Json j = Json::object();
    j.set("error", Json(err));
    j.set("reason", Json(std::string(to_string(st))));
    res.json(code, j.dump());
    return;
  }
  Json map = Json::object();
  for (const auto& kv : node_ids) map.set(kv.first, Json(kv.second));
  Json out = Json::object();
  out.set("workflow_id", Json(wf_id));
  out.set("state", Json(std::string("RUNNING")));
  out.set("jobs", map);
  res.json(201, out.dump());
}

void ApiRouter::get_workflow(const std::string& id, HttpResponse& res) {
  WorkflowRecord wf;
  if (!store_->get_workflow(id, wf)) {
    error_json(res, 404, "workflow not found: " + id);
    return;
  }
  Json arr = Json::array();
  for (const Job& j : store_->list_workflow_jobs(id)) arr.push_back(j.to_json());
  Json out = Json::object();
  out.set("workflow_id", Json(wf.workflow_id));
  out.set("name", Json(wf.name));
  out.set("state", Json(wf.state));
  out.set("created_at_ms", Json(wf.created_at_ms));
  out.set("finished_at_ms", wf.finished_at_ms ? Json(wf.finished_at_ms) : Json());
  out.set("node_count", Json(static_cast<int64_t>(wf.node_count)));
  out.set("nodes", arr);
  res.json(200, out.dump());
}

void ApiRouter::list_workflows(const HttpRequest& req, HttpResponse& res) {
  int64_t limit = 50;
  const std::string limit_param = req.query_param("limit");
  if (!limit_param.empty() && (!parse_int(limit_param, limit) || limit < 1 || limit > 1000)) {
    error_json(res, 400, "limit must be an integer between 1 and 1000");
    return;
  }
  Json arr = Json::array();
  for (const WorkflowRecord& w : store_->list_workflows(static_cast<int>(limit))) {
    Json j = Json::object();
    j.set("workflow_id", Json(w.workflow_id));
    j.set("name", Json(w.name));
    j.set("state", Json(w.state));
    j.set("created_at_ms", Json(w.created_at_ms));
    j.set("finished_at_ms", w.finished_at_ms ? Json(w.finished_at_ms) : Json());
    j.set("node_count", Json(static_cast<int64_t>(w.node_count)));
    arr.push_back(std::move(j));
  }
  Json out = Json::object();
  out.set("workflows", arr);
  out.set("count", Json(static_cast<int64_t>(arr.size())));
  res.json(200, out.dump());
}

void ApiRouter::cancel_workflow(const std::string& id, HttpResponse& res) {
  std::string err;
  const CancelStatus st = engine_->cancel_workflow(id, err);
  if (st == CancelStatus::NotFound) { error_json(res, 404, "workflow not found: " + id); return; }
  if (st == CancelStatus::AlreadyTerminal) {
    error_json(res, 409, "workflow is not running: " + id);
    return;
  }
  Json out = Json::object();
  out.set("workflow_id", Json(id));
  out.set("state", Json(std::string("CANCELLED")));
  res.json(200, out.dump());
}

// ---- operations ------------------------------------------------------------------------------

void ApiRouter::health(HttpResponse& res) {
  Json out = Json::object();
  out.set("status", Json(std::string(engine_->running() ? "ok" : "stopping")));
  out.set("uptime_ms", Json(wall_ms() - started_wall_ms_));
  out.set("queue_depth", Json(static_cast<int64_t>(engine_->queue_depth())));
  out.set("queue_ready", Json(static_cast<int64_t>(engine_->ready_depth())));
  out.set("queue_delayed", Json(static_cast<int64_t>(engine_->delayed_depth())));
  out.set("running", Json(static_cast<int64_t>(engine_->running_count())));
  out.set("outstanding", Json(static_cast<int64_t>(engine_->outstanding())));
  out.set("worker_threads", Json(static_cast<int64_t>(engine_->worker_threads())));
  out.set("remote_workers_alive", Json(static_cast<int64_t>(engine_->alive_workers())));
  Json types = Json::array();
  std::vector<std::string> t = handlers_->types();
  std::sort(t.begin(), t.end());
  for (const std::string& s : t) types.push_back(Json(s));
  out.set("job_types", types);
  res.json(200, out.dump());
}

void ApiRouter::metrics_endpoint(HttpResponse& res) {
  Json out = metrics_->to_json();
  Json states = Json::object();
  for (const auto& kv : store_->count_by_state()) {
    states.set(kv.first, Json(kv.second));
  }
  out.set("jobs_by_state", states);
  res.json(200, out.dump());
}

// ---- remote worker protocol --------------------------------------------------------------------

void ApiRouter::worker_register(const HttpRequest& req, HttpResponse& res) {
  Json body;
  if (!parse_body(req, res, body)) return;
  const std::string name = body.get_string("name", "worker");
  const int capacity = static_cast<int>(body.get_int("capacity", 1));
  if (capacity < 1 || capacity > 1024) {
    error_json(res, 400, "field 'capacity' must be between 1 and 1024");
    return;
  }
  const std::string id = engine_->register_worker(name, capacity);
  Json out = Json::object();
  out.set("worker_id", Json(id));
  out.set("heartbeat_interval_ms",
          Json(engine_->config().worker_heartbeat_timeout_ms / 3));
  out.set("lease_duration_ms", Json(engine_->config().lease_duration_ms));
  res.json(200, out.dump());
}

void ApiRouter::worker_heartbeat(const HttpRequest& req, HttpResponse& res) {
  Json body;
  if (!parse_body(req, res, body)) return;
  const std::string worker_id = body.get_string("worker_id", "");
  std::vector<std::string> cancelled;
  if (!engine_->heartbeat_worker(worker_id, cancelled)) {
    error_json(res, 404, "unknown worker: " + worker_id);
    return;
  }
  Json arr = Json::array();
  for (const std::string& c : cancelled) arr.push_back(Json(c));
  Json out = Json::object();
  out.set("ok", Json(true));
  out.set("cancel", arr);
  res.json(200, out.dump());
}

void ApiRouter::worker_claim(const HttpRequest& req, HttpResponse& res) {
  Json body;
  if (!parse_body(req, res, body)) return;
  const std::string worker_id = body.get_string("worker_id", "");
  int64_t max_jobs = body.get_int("max_jobs", 1);
  int64_t wait_ms = body.get_int("wait_ms", 0);
  if (max_jobs < 1 || max_jobs > 256) {
    error_json(res, 400, "field 'max_jobs' must be between 1 and 256");
    return;
  }
  if (wait_ms < 0 || wait_ms > 60000) {
    error_json(res, 400, "field 'wait_ms' must be between 0 and 60000");
    return;
  }
  const std::vector<LeasedJob> leased =
      engine_->claim(worker_id, static_cast<std::size_t>(max_jobs), wait_ms);
  Json arr = Json::array();
  for (const LeasedJob& lj : leased) {
    Json j = Json::object();
    j.set("job_id", Json(lj.job_id));
    j.set("type", Json(lj.type));
    j.set("payload", lj.payload);
    j.set("attempt", Json(static_cast<int64_t>(lj.attempt)));
    // The lease token is a full 64-bit value; JSON numbers lose precision above 2^53, so
    // it travels as a decimal string.
    j.set("lease_token", Json(u64_to_string(lj.lease_token)));
    j.set("timeout_ms", Json(lj.timeout_ms));
    j.set("lease_expires_ms", Json(lj.lease_expires_ms));
    arr.push_back(std::move(j));
  }
  Json out = Json::object();
  out.set("jobs", arr);
  out.set("count", Json(static_cast<int64_t>(arr.size())));
  res.json(200, out.dump());
}

void ApiRouter::worker_complete(const HttpRequest& req, HttpResponse& res) {
  Json body;
  if (!parse_body(req, res, body)) return;
  const std::string worker_id = body.get_string("worker_id", "");
  const std::string job_id = body.get_string("job_id", "");
  const int attempt = static_cast<int>(body.get_int("attempt", 0));
  const uint64_t token = string_to_u64(body.get_string("lease_token", "0"));
  const std::string status = body.get_string("status", "");
  if (status != "SUCCEEDED" && status != "FAILED" && status != "TIMED_OUT" &&
      status != "CANCELLED") {
    error_json(res, 400,
               "field 'status' must be one of SUCCEEDED, FAILED, TIMED_OUT, CANCELLED");
    return;
  }
  const Json* result = body.find("result");
  const std::string error = body.get_string("error", "");
  const CompleteStatus st = engine_->complete(worker_id, job_id, attempt, token, status,
                                              result ? *result : Json::object(), error);
  Json out = Json::object();
  switch (st) {
    case CompleteStatus::Accepted:
      out.set("accepted", Json(true));
      res.json(200, out.dump());
      return;
    case CompleteStatus::StaleLease:
      out.set("accepted", Json(false));
      out.set("reason", Json(std::string("stale_lease")));
      res.json(200, out.dump());
      return;
    case CompleteStatus::NotFound:
      error_json(res, 404, "job not found: " + job_id);
      return;
  }
}

}  // namespace je
