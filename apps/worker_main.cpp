// jobengine-worker: a worker process. Holds no queue and makes no scheduling decision; it
// leases work from the coordinator and reports results with the lease token it was given.
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "cli.hpp"
#include "jobengine/handlers.hpp"
#include "jobengine/remote_worker.hpp"

namespace {
volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
  using namespace je;
  const Args args(argc, argv);
  if (args.has("help")) {
    std::cout <<
        "jobengine-worker - remote worker process\n\n"
        "  --host <addr>        coordinator address (default 127.0.0.1)\n"
        "  --port <n>           coordinator port (default 8080)\n"
        "  --name <name>        worker name reported to the coordinator\n"
        "  --threads <n>        concurrent jobs this process executes (default 4)\n"
        "  --poll-wait-ms <n>   long-poll duration on claim (default 1000)\n"
        "  --heartbeat-ms <n>   heartbeat interval, 0 = follow the coordinator\n"
        "  --log-level <level>  debug|info|warn|error|off\n";
    return 0;
  }
  apply_log_level(args);

  RemoteWorkerConfig cfg;
  cfg.host = args.str("host", "127.0.0.1");
  cfg.port = static_cast<int>(args.integer("port", 8080));
  cfg.name = args.str("name", "worker-" + num(static_cast<int64_t>(::getpid())));
  cfg.threads = static_cast<int>(args.integer("threads", 4));
  cfg.poll_wait_ms = args.integer("poll-wait-ms", 1000);
  cfg.heartbeat_ms = args.integer("heartbeat-ms", 0);

  HandlerRegistry handlers;
  HandlerRegistry::register_builtins(handlers);

  RemoteWorker worker(cfg, &handlers);
  std::string err;
  if (!worker.start(err)) {
    std::cerr << "worker failed to start: " << err << "\n";
    return 1;
  }
  ::signal(SIGINT, on_signal);
  ::signal(SIGTERM, on_signal);
  std::cout << "READY worker_id=" << worker.worker_id() << " name=" << cfg.name
            << " threads=" << cfg.threads << std::endl;

  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  worker.stop();
  std::cout << "STOPPED completed=" << worker.jobs_completed()
            << " failed=" << worker.jobs_failed()
            << " rejected=" << worker.results_rejected() << std::endl;
  return 0;
}
