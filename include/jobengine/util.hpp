#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace je {

// ---- time -------------------------------------------------------------------------
// One clock choice, used everywhere: steady_clock for durations and scheduling,
// system_clock only for human-facing creation timestamps. Mixing the two is a classic
// source of scheduling bugs when the wall clock steps.
int64_t now_ms();       // steady, monotonic, milliseconds since process-independent epoch
int64_t now_us();       // steady, microseconds
int64_t wall_ms();      // system clock, milliseconds since Unix epoch

// ---- identifiers ------------------------------------------------------------------
// ULID-style: 48-bit millisecond timestamp + 80 bits of randomness, Crockford base32.
// Lexicographic order matches creation order, which makes "newest first" a plain ORDER BY.
std::string new_id(char prefix);
uint64_t random_u64();

// ---- strings ----------------------------------------------------------------------
std::vector<std::string> split(std::string_view s, char delim);
std::string trim(std::string_view s);
std::string to_lower(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix);
bool parse_int(std::string_view s, int64_t& out);
std::string url_decode(std::string_view s);

// ---- logging ----------------------------------------------------------------------
enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

class Log {
 public:
  static void set_level(LogLevel l);
  static LogLevel level();
  static void write(LogLevel l, std::string_view component, std::string_view message);
};

void log_debug(std::string_view component, std::string_view msg);
void log_info(std::string_view component, std::string_view msg);
void log_warn(std::string_view component, std::string_view msg);
void log_error(std::string_view component, std::string_view msg);

// Tiny formatter so log sites stay readable without pulling in <format>.
std::string concat(std::initializer_list<std::string_view> parts);
std::string num(int64_t v);
std::string num(double v);
// Small integral overloads so call sites do not need a cast to disambiguate.
inline std::string num(int v) { return num(static_cast<int64_t>(v)); }
inline std::string num(unsigned int v) { return num(static_cast<int64_t>(v)); }
inline std::string num(long v) { return num(static_cast<int64_t>(v)); }
inline std::string num(unsigned long v) { return num(static_cast<int64_t>(v)); }
inline std::string num(unsigned long long v) { return num(static_cast<int64_t>(v)); }

}  // namespace je
