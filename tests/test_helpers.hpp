#pragma once
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "jobengine/engine.hpp"
#include "jobengine/handlers.hpp"
#include "jobengine/metrics.hpp"
#include "jobengine/api.hpp"
#include "jobengine/http.hpp"
#include "jobengine/store.hpp"
#include "jobengine/util.hpp"

#include <stdexcept>

namespace je {
namespace test {

// Polls a predicate until it holds or the deadline passes. Returns false on timeout, so a
// test asserts on the result instead of sleeping a guessed interval and hoping.
template <typename Fn>
bool wait_until(Fn predicate, int64_t timeout_ms = 5000, int64_t poll_ms = 2) {
  const int64_t deadline = now_ms() + timeout_ms;
  while (now_ms() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
  }
  return predicate();
}

// A complete engine wired to a private in-memory store, for tests that need the real thing.
struct Harness {
  std::unique_ptr<JobStore> store;
  HandlerRegistry handlers;
  Metrics metrics;
  std::unique_ptr<JobEngine> engine;

  explicit Harness(int workers = 2, EngineConfig cfg = EngineConfig()) {
    StoreConfig sc;
    sc.path = ":memory:";
    sc.reader_connections = 2;
    std::string err;
    store = JobStore::open(sc, err);
    HandlerRegistry::register_builtins(handlers);
    cfg.worker_threads = workers;
    cfg.node_name = "test";
    engine = std::make_unique<JobEngine>(cfg, store.get(), &handlers, &metrics);
  }
  ~Harness() {
    if (engine) engine->stop(false);
  }
  void start() { engine->start(); }

  std::string submit(const std::string& type, Json payload = Json::object(), int priority = 0,
                     int max_attempts = 1, int64_t timeout_ms = 0, int64_t delay_ms = 0) {
    SubmitRequest req;
    req.type = type;
    req.payload = std::move(payload);
    req.priority = priority;
    req.retry.max_attempts = max_attempts;
    req.timeout_ms = timeout_ms;
    req.delay_ms = delay_ms;
    std::string id, err;
    const SubmitStatus st = engine->submit(req, id, err);
    return st == SubmitStatus::Ok ? id : std::string();
  }

  JobState state_of(const std::string& id) {
    Job j;
    if (!engine->get_job(id, j)) return JobState::Failed;
    return j.state;
  }
  bool wait_state(const std::string& id, JobState want, int64_t timeout_ms = 5000) {
    return wait_until([&] { return state_of(id) == want; }, timeout_ms);
  }
  bool wait_terminal(const std::string& id, int64_t timeout_ms = 5000) {
    return wait_until([&] { return is_terminal(state_of(id)); }, timeout_ms);
  }
};

// A live coordinator: engine + REST API on an ephemeral port, plus a client for it.
struct ServerHarness {
  Harness engine_harness;
  std::unique_ptr<ApiRouter> router;
  std::unique_ptr<HttpServer> server;

  explicit ServerHarness(int workers = 2, EngineConfig cfg = EngineConfig(),
                         int http_threads = 8)
      : engine_harness(workers, cfg) {
    engine_harness.start();
    router = std::make_unique<ApiRouter>(engine_harness.engine.get(),
                                         engine_harness.store.get(),
                                         &engine_harness.metrics, &engine_harness.handlers);
    HttpServerConfig hc;
    hc.bind_address = "127.0.0.1";
    hc.port = 0;  // ephemeral: tests never collide on a fixed port
    hc.threads = http_threads;
    server = std::make_unique<HttpServer>(hc);
    std::string err;
    const bool ok = server->start(
        [this](const HttpRequest& req, HttpResponse& res) { router->handle(req, res); }, err);
    if (!ok) throw std::runtime_error("test server failed to start: " + err);
  }
  ~ServerHarness() {
    if (server) server->stop();
    if (engine_harness.engine) engine_harness.engine->stop(false);
  }
  int port() const { return server->port(); }
  HttpClient client() const { return HttpClient("127.0.0.1", server->port(), 15000); }

  JobEngine* engine() { return engine_harness.engine.get(); }
  JobStore* store() { return engine_harness.store.get(); }
  Metrics& metrics() { return engine_harness.metrics; }
  HandlerRegistry& handlers() { return engine_harness.handlers; }
};

// Parses a JSON response body, failing loudly rather than returning a silent null.
inline Json body_json(const HttpClientResponse& res) {
  Json j;
  std::string err;
  if (!Json::parse(res.body, j, err)) return Json();
  return j;
}

}  // namespace test
}  // namespace je
