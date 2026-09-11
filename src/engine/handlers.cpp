#include "jobengine/handlers.hpp"

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <chrono>

#include "jobengine/util.hpp"

namespace je {
namespace {

uint64_t fib(uint64_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

}  // namespace

void HandlerRegistry::register_handler(const std::string& type, JobHandler fn) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  handlers_[type] = std::move(fn);
}

bool HandlerRegistry::has(const std::string& type) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return handlers_.find(type) != handlers_.end();
}

bool HandlerRegistry::get(const std::string& type, JobHandler& out) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto it = handlers_.find(type);
  if (it == handlers_.end()) return false;
  out = it->second;
  return true;
}

std::vector<std::string> HandlerRegistry::types() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  std::vector<std::string> out;
  out.reserve(handlers_.size());
  for (const auto& kv : handlers_) out.push_back(kv.first);
  return out;
}

std::size_t HandlerRegistry::size() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return handlers_.size();
}

void HandlerRegistry::register_builtins(HandlerRegistry& reg) {
  reg.register_handler("noop", [](const JobContext&) { return JobOutcome::success(); });

  reg.register_handler("echo", [](const JobContext& ctx) {
    Json r = Json::object();
    r.set("echo", ctx.payload());
    r.set("attempt", Json(static_cast<int64_t>(ctx.attempt())));
    return JobOutcome::success(std::move(r));
  });

  reg.register_handler("sleep", [](const JobContext& ctx) {
    const int64_t ms = ctx.payload().get_int("ms", 10);
    if (!ctx.sleep_or_cancel(ms)) return JobOutcome::failure("cancelled");
    Json r = Json::object();
    r.set("slept_ms", Json(ms));
    return JobOutcome::success(std::move(r));
  });

  // Deliberately ignores the cancellation flag. Used by the timeout test to demonstrate
  // the documented limit of cooperative cancellation rather than assert it in prose.
  reg.register_handler("sleep_uncooperative", [](const JobContext& ctx) {
    const int64_t ms = ctx.payload().get_int("ms", 10);
    const int64_t end = now_ms() + ms;
    while (now_ms() < end) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Json r = Json::object();
    r.set("slept_ms", Json(ms));
    return JobOutcome::success(std::move(r));
  });

  reg.register_handler("cpu_fib", [](const JobContext& ctx) {
    const int64_t n = ctx.payload().get_int("n", 25);
    if (n < 0 || n > 45) return JobOutcome::failure("n out of range [0,45]");
    const uint64_t v = fib(static_cast<uint64_t>(n));
    Json r = Json::object();
    r.set("n", Json(n));
    r.set("fib", Json(static_cast<int64_t>(v)));
    return JobOutcome::success(std::move(r));
  });

  reg.register_handler("cpu_hash", [](const JobContext& ctx) {
    const int64_t iters = ctx.payload().get_int("iterations", 100000);
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int64_t i = 0; i < iters; ++i) {
      h ^= static_cast<uint64_t>(i);
      h *= 0x100000001b3ULL;
      // Poll cancellation cheaply: once every 4096 iterations, not every iteration.
      if ((i & 0xFFF) == 0 && ctx.cancel_requested()) return JobOutcome::failure("cancelled");
    }
    Json r = Json::object();
    r.set("iterations", Json(iters));
    r.set("hash", Json(static_cast<int64_t>(h & 0x7FFFFFFFFFFFFFFFULL)));
    return JobOutcome::success(std::move(r));
  });

  reg.register_handler("file_lines", [](const JobContext& ctx) {
    const std::string path = ctx.payload().get_string("path", "");
    if (path.empty()) return JobOutcome::failure("payload.path is required");
    std::ifstream in(path, std::ios::binary);
    if (!in) return JobOutcome::failure("cannot open file: " + path);
    int64_t lines = 0, bytes = 0, words = 0;
    std::string line;
    while (std::getline(in, line)) {
      ++lines;
      bytes += static_cast<int64_t>(line.size()) + 1;
      bool in_word = false;
      for (const char c : line) {
        const bool space = c == ' ' || c == '\t';
        if (!space && !in_word) { ++words; in_word = true; }
        else if (space) { in_word = false; }
      }
      if (ctx.cancel_requested()) return JobOutcome::failure("cancelled");
    }
    Json r = Json::object();
    r.set("lines", Json(lines));
    r.set("words", Json(words));
    r.set("bytes", Json(bytes));
    return JobOutcome::success(std::move(r));
  });

  // Fails until fail_until_attempt is reached, then succeeds. With
  // fail_until_attempt = 0 (the default) it always fails.
  reg.register_handler("fail", [](const JobContext& ctx) {
    const int64_t until = ctx.payload().get_int("fail_until_attempt", 0);
    const std::string msg = ctx.payload().get_string("message", "deliberate failure");
    if (until > 0 && ctx.attempt() >= until) {
      Json r = Json::object();
      r.set("succeeded_on_attempt", Json(static_cast<int64_t>(ctx.attempt())));
      return JobOutcome::success(std::move(r));
    }
    return JobOutcome::failure(msg + " (attempt " + num(static_cast<int64_t>(ctx.attempt())) + ")");
  });

  reg.register_handler("flaky", [](const JobContext& ctx) {
    const int64_t pct = ctx.payload().get_int("fail_probability_pct", 30);
    const int64_t ms = ctx.payload().get_int("ms", 0);
    if (ms > 0 && !ctx.sleep_or_cancel(ms)) return JobOutcome::failure("cancelled");
    if (static_cast<int64_t>(random_u64() % 100) < pct) {
      return JobOutcome::failure("flaky failure");
    }
    return JobOutcome::success();
  });

  // Throws, to prove the worker's exception boundary turns a throwing handler into a
  // failed attempt instead of a terminated process.
  reg.register_handler("throw", [](const JobContext&) -> JobOutcome {
    throw std::runtime_error("handler threw std::runtime_error");
  });
}

}  // namespace je
