#include "jobengine/util.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>

namespace je {
namespace {

std::mt19937_64& rng() {
  // One generator per thread: no shared state, so no lock and no contention, and the
  // seed mixes a random_device with the thread id so two threads never share a stream.
  static thread_local std::mt19937_64 gen([] {
    std::random_device rd;
    const uint64_t a = (static_cast<uint64_t>(rd()) << 32) ^ rd();
    const uint64_t b = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return a ^ (b * 0x9E3779B97F4A7C15ULL);
  }());
  return gen;
}

std::atomic<LogLevel> g_level{LogLevel::Info};
std::mutex g_log_mutex;

const char* level_name(LogLevel l) {
  switch (l) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Off: return "OFF  ";
  }
  return "?????";
}

}  // namespace

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

uint64_t random_u64() { return rng()(); }

std::string new_id(char prefix) {
  static const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // Crockford base32
  const uint64_t ts = static_cast<uint64_t>(wall_ms()) & 0xFFFFFFFFFFFFULL;  // 48 bits
  const uint64_t r1 = rng()();
  const uint64_t r2 = rng()();

  std::string out;
  out.reserve(28);
  out.push_back(prefix);
  out.push_back('_');
  // 48-bit timestamp -> 10 base32 characters, most significant first.
  for (int i = 9; i >= 0; --i) {
    out.push_back(kAlphabet[(ts >> (i * 5)) & 0x1F]);
  }
  // 80 bits of randomness -> 16 base32 characters.
  for (int i = 0; i < 8; ++i) out.push_back(kAlphabet[(r1 >> (i * 5)) & 0x1F]);
  for (int i = 0; i < 8; ++i) out.push_back(kAlphabet[(r2 >> (i * 5)) & 0x1F]);
  return out;
}

std::vector<std::string> split(std::string_view s, char delim) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    const std::size_t pos = s.find(delim, start);
    if (pos == std::string_view::npos) {
      out.emplace_back(s.substr(start));
      break;
    }
    out.emplace_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

std::string trim(std::string_view s) {
  std::size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return std::string(s.substr(b, e - b));
}

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool starts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool parse_int(std::string_view s, int64_t& out) {
  const std::string tmp = trim(s);
  if (tmp.empty()) return false;
  std::size_t i = 0;
  bool neg = false;
  if (tmp[0] == '-' || tmp[0] == '+') { neg = tmp[0] == '-'; i = 1; }
  if (i >= tmp.size()) return false;
  int64_t v = 0;
  for (; i < tmp.size(); ++i) {
    if (tmp[i] < '0' || tmp[i] > '9') return false;
    const int d = tmp[i] - '0';
    if (v > (INT64_MAX - d) / 10) return false;  // overflow
    v = v * 10 + d;
  }
  out = neg ? -v : v;
  return true;
}

std::string url_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  const auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') { out.push_back(' '); continue; }
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

void Log::set_level(LogLevel l) { g_level.store(l, std::memory_order_relaxed); }
LogLevel Log::level() { return g_level.load(std::memory_order_relaxed); }

void Log::write(LogLevel l, std::string_view component, std::string_view message) {
  if (static_cast<int>(l) < static_cast<int>(level())) return;
  const int64_t t = wall_ms();
  char stamp[32];
  const std::time_t secs = static_cast<std::time_t>(t / 1000);
  std::tm tm_buf{};
  ::gmtime_r(&secs, &tm_buf);
  std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", tm_buf.tm_hour, tm_buf.tm_min,
                tm_buf.tm_sec, static_cast<int>(t % 1000));
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::fprintf(stderr, "%s %s [%.*s] %.*s\n", stamp, level_name(l),
               static_cast<int>(component.size()), component.data(),
               static_cast<int>(message.size()), message.data());
  std::fflush(stderr);
}

void log_debug(std::string_view c, std::string_view m) { Log::write(LogLevel::Debug, c, m); }
void log_info(std::string_view c, std::string_view m) { Log::write(LogLevel::Info, c, m); }
void log_warn(std::string_view c, std::string_view m) { Log::write(LogLevel::Warn, c, m); }
void log_error(std::string_view c, std::string_view m) { Log::write(LogLevel::Error, c, m); }

std::string concat(std::initializer_list<std::string_view> parts) {
  std::size_t n = 0;
  for (const auto& p : parts) n += p.size();
  std::string out;
  out.reserve(n);
  for (const auto& p : parts) out.append(p);
  return out;
}

std::string num(int64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  return buf;
}

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.3f", v);
  return buf;
}

}  // namespace je
