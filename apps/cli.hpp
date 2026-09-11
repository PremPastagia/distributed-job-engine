#pragma once
// Minimal argument parsing shared by the executables: --key value and --flag.
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "jobengine/util.hpp"

namespace je {

class Args {
 public:
  Args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a.rfind("--", 0) != 0) { positional_.push_back(a); continue; }
      a = a.substr(2);
      const std::size_t eq = a.find('=');
      if (eq != std::string::npos) {
        map_[a.substr(0, eq)] = a.substr(eq + 1);
      } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        map_[a] = argv[++i];
      } else {
        map_[a] = "true";
      }
    }
  }
  bool has(const std::string& k) const { return map_.count(k) > 0; }
  std::string str(const std::string& k, const std::string& def) const {
    const auto it = map_.find(k);
    return it == map_.end() ? def : it->second;
  }
  int64_t integer(const std::string& k, int64_t def) const {
    const auto it = map_.find(k);
    if (it == map_.end()) return def;
    int64_t v = 0;
    if (!parse_int(it->second, v)) {
      std::cerr << "invalid integer for --" << k << ": " << it->second << "\n";
      std::exit(2);
    }
    return v;
  }
  double real(const std::string& k, double def) const {
    const auto it = map_.find(k);
    return it == map_.end() ? def : std::strtod(it->second.c_str(), nullptr);
  }
  bool flag(const std::string& k, bool def = false) const {
    const auto it = map_.find(k);
    if (it == map_.end()) return def;
    return it->second == "true" || it->second == "1" || it->second == "yes";
  }
  const std::vector<std::string>& positional() const { return positional_; }

 private:
  std::unordered_map<std::string, std::string> map_;
  std::vector<std::string> positional_;
};

inline void apply_log_level(const Args& args, const char* def = "info") {
  const std::string level = to_lower(args.str("log-level", def));
  if (level == "debug") Log::set_level(LogLevel::Debug);
  else if (level == "warn") Log::set_level(LogLevel::Warn);
  else if (level == "error") Log::set_level(LogLevel::Error);
  else if (level == "off") Log::set_level(LogLevel::Off);
  else Log::set_level(LogLevel::Info);
}

}  // namespace je
