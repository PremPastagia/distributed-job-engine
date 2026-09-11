// jobengine-server: the coordinator. Owns the queue, the database and every scheduling
// decision; optionally runs an embedded worker pool as well.
#include <signal.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "cli.hpp"
#include "jobengine/api.hpp"
#include "jobengine/engine.hpp"
#include "jobengine/handlers.hpp"
#include "jobengine/http.hpp"
#include "jobengine/metrics.hpp"
#include "jobengine/store.hpp"

namespace {
volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

void usage() {
  std::cout <<
      "jobengine-server - job processing coordinator\n\n"
      "  --host <addr>              bind address (default 127.0.0.1)\n"
      "  --port <n>                 listen port, 0 for ephemeral (default 8080)\n"
      "  --db <path>                SQLite file, or :memory: (default jobengine.db)\n"
      "  --workers <n>              embedded worker threads (default: hardware threads)\n"
      "  --http-threads <n>         concurrent HTTP connections (default 16)\n"
      "  --readers <n>              SQLite reader connections (default 4)\n"
      "  --max-outstanding <n>      admission limit on non-terminal jobs (default 100000)\n"
      "  --lease-ms <n>             remote lease duration (default 30000)\n"
      "  --heartbeat-timeout-ms <n> worker liveness timeout (default 6000)\n"
      "  --watchdog-ms <n>          watchdog scan interval (default 25)\n"
      "  --sqlite-sync <normal|full> durability setting (default normal)\n"
      "  --group-commit <bool>      batch writes into shared transactions (default true)\n"
      "  --commit-delay-us <n>      group-commit batching delay (default 0)\n"
      "  --drain-on-shutdown        finish queued jobs before exiting\n"
      "  --log-level <level>        debug|info|warn|error|off (default info)\n";
}
}  // namespace

int main(int argc, char** argv) {
  using namespace je;
  const Args args(argc, argv);
  if (args.has("help")) { usage(); return 0; }
  apply_log_level(args);

  StoreConfig store_cfg;
  store_cfg.path = args.str("db", "jobengine.db");
  store_cfg.synchronous_full = to_lower(args.str("sqlite-sync", "normal")) == "full";
  store_cfg.reader_connections = static_cast<int>(args.integer("readers", 4));
  store_cfg.group_commit = args.flag("group-commit", true);
  store_cfg.commit_delay_us = args.integer("commit-delay-us", 0);

  std::string err;
  std::unique_ptr<JobStore> store = JobStore::open(store_cfg, err);
  if (!store) {
    std::cerr << "cannot open store: " << err << "\n";
    return 1;
  }

  HandlerRegistry handlers;
  HandlerRegistry::register_builtins(handlers);
  Metrics metrics;

  EngineConfig engine_cfg;
  const int64_t hw = static_cast<int64_t>(std::thread::hardware_concurrency());
  engine_cfg.worker_threads =
      static_cast<int>(args.integer("workers", hw > 0 ? hw : 4));
  engine_cfg.max_outstanding_jobs =
      static_cast<std::size_t>(args.integer("max-outstanding", 100000));
  engine_cfg.lease_duration_ms = args.integer("lease-ms", 30000);
  engine_cfg.worker_heartbeat_timeout_ms = args.integer("heartbeat-timeout-ms", 6000);
  engine_cfg.watchdog_interval_ms = args.integer("watchdog-ms", 25);
  engine_cfg.drain_on_shutdown = args.flag("drain-on-shutdown");

  JobEngine engine(engine_cfg, store.get(), &handlers, &metrics);
  const int recovered = engine.recover();
  engine.start();

  ApiRouter router(&engine, store.get(), &metrics, &handlers);
  HttpServerConfig http_cfg;
  http_cfg.bind_address = args.str("host", "127.0.0.1");
  http_cfg.port = static_cast<int>(args.integer("port", 8080));
  http_cfg.threads = static_cast<int>(args.integer("http-threads", 16));

  HttpServer server(http_cfg);
  if (!server.start([&router](const HttpRequest& req, HttpResponse& res) {
                      router.handle(req, res);
                    },
                    err)) {
    std::cerr << "cannot start HTTP server: " << err << "\n";
    engine.stop(false);
    return 1;
  }

  ::signal(SIGINT, on_signal);
  ::signal(SIGTERM, on_signal);

  // A single machine-readable readiness line, so test and benchmark scripts can wait for
  // the server instead of sleeping a guessed interval.
  std::cout << "READY host=" << http_cfg.bind_address << " port=" << server.port()
            << " workers=" << engine_cfg.worker_threads << " db=" << store_cfg.path
            << " recovered=" << recovered << std::endl;

  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  std::cout << "SHUTDOWN starting" << std::endl;
  server.stop();
  engine.stop(engine_cfg.drain_on_shutdown);
  std::cout << "SHUTDOWN complete" << std::endl;
  return 0;
}
