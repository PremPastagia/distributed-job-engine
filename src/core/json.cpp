#include "jobengine/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace je {
namespace {

constexpr int kMaxDepth = 64;

class Parser {
 public:
  Parser(std::string_view t, std::string& err) : t_(t), err_(err) {}

  bool run(Json& out) {
    skip_ws();
    if (!value(out, 0)) return false;
    skip_ws();
    if (i_ != t_.size()) return fail("trailing content after JSON value");
    return true;
  }

 private:
  bool fail(const char* msg) {
    if (err_.empty()) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%s at offset %zu", msg, i_);
      err_ = buf;
    }
    return false;
  }

  void skip_ws() {
    while (i_ < t_.size()) {
      const char c = t_[i_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
      else break;
    }
  }

  bool lit(std::string_view s) {
    if (t_.size() - i_ < s.size() || t_.compare(i_, s.size(), s) != 0) return false;
    i_ += s.size();
    return true;
  }

  bool value(Json& out, int depth) {
    if (depth > kMaxDepth) return fail("maximum nesting depth exceeded");
    if (i_ >= t_.size()) return fail("unexpected end of input");
    switch (t_[i_]) {
      case '{': return object(out, depth);
      case '[': return array(out, depth);
      case '"': {
        std::string s;
        if (!string(s)) return false;
        out = Json(std::move(s));
        return true;
      }
      case 't': if (!lit("true")) return fail("invalid literal"); out = Json(true); return true;
      case 'f': if (!lit("false")) return fail("invalid literal"); out = Json(false); return true;
      case 'n': if (!lit("null")) return fail("invalid literal"); out = Json(); return true;
      default: return number(out);
    }
  }

  bool object(Json& out, int depth) {
    ++i_;  // '{'
    out = Json::object();
    skip_ws();
    if (i_ < t_.size() && t_[i_] == '}') { ++i_; return true; }
    for (;;) {
      skip_ws();
      if (i_ >= t_.size() || t_[i_] != '"') return fail("expected object key string");
      std::string key;
      if (!string(key)) return false;
      skip_ws();
      if (i_ >= t_.size() || t_[i_] != ':') return fail("expected ':' after object key");
      ++i_;
      skip_ws();
      Json v;
      if (!value(v, depth + 1)) return false;
      out.set(std::move(key), std::move(v));
      skip_ws();
      if (i_ >= t_.size()) return fail("unterminated object");
      if (t_[i_] == ',') { ++i_; continue; }
      if (t_[i_] == '}') { ++i_; return true; }
      return fail("expected ',' or '}' in object");
    }
  }

  bool array(Json& out, int depth) {
    ++i_;  // '['
    out = Json::array();
    skip_ws();
    if (i_ < t_.size() && t_[i_] == ']') { ++i_; return true; }
    for (;;) {
      skip_ws();
      Json v;
      if (!value(v, depth + 1)) return false;
      out.push_back(std::move(v));
      skip_ws();
      if (i_ >= t_.size()) return fail("unterminated array");
      if (t_[i_] == ',') { ++i_; continue; }
      if (t_[i_] == ']') { ++i_; return true; }
      return fail("expected ',' or ']' in array");
    }
  }

  // Appends the UTF-8 encoding of a code point.
  static void utf8(uint32_t cp, std::string& out) {
    if (cp <= 0x7F) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool hex4(uint32_t& v) {
    if (t_.size() - i_ < 4) return fail("truncated \\u escape");
    v = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = t_[i_ + static_cast<std::size_t>(k)];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
      else return fail("invalid hex digit in \\u escape");
    }
    i_ += 4;
    return true;
  }

  bool string(std::string& out) {
    ++i_;  // opening quote
    out.clear();
    for (;;) {
      if (i_ >= t_.size()) return fail("unterminated string");
      const unsigned char c = static_cast<unsigned char>(t_[i_]);
      if (c == '"') { ++i_; return true; }
      if (c < 0x20) return fail("unescaped control character in string");
      if (c != '\\') { out.push_back(static_cast<char>(c)); ++i_; continue; }
      ++i_;
      if (i_ >= t_.size()) return fail("unterminated escape");
      const char e = t_[i_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate, expect a low one
            if (t_.size() - i_ >= 2 && t_[i_] == '\\' && t_[i_ + 1] == 'u') {
              i_ += 2;
              uint32_t lo = 0;
              if (!hex4(lo)) return false;
              if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                return fail("invalid low surrogate in \\u escape");
              }
            } else {
              return fail("unpaired high surrogate in \\u escape");
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail("unpaired low surrogate in \\u escape");
          }
          utf8(cp, out);
          break;
        }
        default: return fail("invalid escape character");
      }
    }
  }

  bool number(Json& out) {
    const std::size_t start = i_;
    if (i_ < t_.size() && t_[i_] == '-') ++i_;
    if (i_ >= t_.size() || t_[i_] < '0' || t_[i_] > '9') return fail("invalid number");
    if (t_[i_] == '0') {
      ++i_;
    } else {
      while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') ++i_;
    }
    bool integral = true;
    if (i_ < t_.size() && t_[i_] == '.') {
      integral = false;
      ++i_;
      if (i_ >= t_.size() || t_[i_] < '0' || t_[i_] > '9') return fail("expected digit after '.'");
      while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') ++i_;
    }
    if (i_ < t_.size() && (t_[i_] == 'e' || t_[i_] == 'E')) {
      integral = false;
      ++i_;
      if (i_ < t_.size() && (t_[i_] == '+' || t_[i_] == '-')) ++i_;
      if (i_ >= t_.size() || t_[i_] < '0' || t_[i_] > '9') return fail("expected digit in exponent");
      while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') ++i_;
    }
    const std::string raw(t_.substr(start, i_ - start));
    if (integral) {
      errno = 0;
      char* end = nullptr;
      const long long v = std::strtoll(raw.c_str(), &end, 10);
      if (errno == 0 && end && *end == '\0') {
        out = Json(static_cast<int64_t>(v));
        return true;
      }
      // Out of int64 range: fall through and keep it as a double.
    }
    out = Json(std::strtod(raw.c_str(), nullptr));
    return true;
  }

  std::string_view t_;
  std::string& err_;
  std::size_t i_ = 0;
};

}  // namespace

const std::string& Json::empty_string() {
  static const std::string kEmpty;
  return kEmpty;
}

int64_t Json::as_int(int64_t def) const {
  if (type_ != Type::Number) return def;
  if (is_int_) return int_;
  if (!std::isfinite(num_)) return def;
  return static_cast<int64_t>(num_);
}

bool Json::parse(std::string_view text, Json& out, std::string& err) {
  err.clear();
  out = Json();
  Parser p(text, err);
  return p.run(out);
}

Json Json::parse_or_null(std::string_view text) {
  Json j;
  std::string err;
  if (!parse(text, j, err)) return Json();
  return j;
}

const Json* Json::find(std::string_view key) const {
  if (type_ != Type::Object) return nullptr;
  for (const auto& kv : obj_) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

void Json::set(std::string key, Json value) {
  if (type_ != Type::Object) { type_ = Type::Object; obj_.clear(); }
  for (auto& kv : obj_) {
    if (kv.first == key) { kv.second = std::move(value); return; }
  }
  obj_.emplace_back(std::move(key), std::move(value));
}

int64_t Json::get_int(std::string_view key, int64_t def) const {
  const Json* v = find(key);
  return v ? v->as_int(def) : def;
}
double Json::get_double(std::string_view key, double def) const {
  const Json* v = find(key);
  return v ? v->as_double(def) : def;
}
bool Json::get_bool(std::string_view key, bool def) const {
  const Json* v = find(key);
  return v ? v->as_bool(def) : def;
}
std::string Json::get_string(std::string_view key, std::string def) const {
  const Json* v = find(key);
  return (v && v->is_string()) ? v->as_string() : def;
}

void Json::push_back(Json value) {
  if (type_ != Type::Array) { type_ = Type::Array; arr_.clear(); }
  arr_.push_back(std::move(value));
}

std::size_t Json::size() const {
  if (type_ == Type::Array) return arr_.size();
  if (type_ == Type::Object) return obj_.size();
  return 0;
}

const Json& Json::at(std::size_t i) const {
  static const Json kNull;
  if (type_ != Type::Array || i >= arr_.size()) return kNull;
  return arr_[i];
}

void Json::escape_into(std::string_view s, std::string& out) {
  out.push_back('"');
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

void Json::dump_into(std::string& out, int indent, int depth) const {
  const bool pretty = indent > 0;
  const auto newline_indent = [&](int d) {
    if (!pretty) return;
    out.push_back('\n');
    out.append(static_cast<std::size_t>(indent * d), ' ');
  };
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: {
      char buf[40];
      if (is_int_) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(int_));
      } else if (!std::isfinite(num_)) {
        out += "null";  // JSON has no NaN/Inf
        break;
      } else {
        std::snprintf(buf, sizeof(buf), "%.17g", num_);
      }
      out += buf;
      break;
    }
    case Type::String: escape_into(str_, out); break;
    case Type::Array: {
      if (arr_.empty()) { out += "[]"; break; }
      out.push_back('[');
      for (std::size_t k = 0; k < arr_.size(); ++k) {
        if (k) out.push_back(',');
        newline_indent(depth + 1);
        arr_[k].dump_into(out, indent, depth + 1);
      }
      newline_indent(depth);
      out.push_back(']');
      break;
    }
    case Type::Object: {
      if (obj_.empty()) { out += "{}"; break; }
      out.push_back('{');
      for (std::size_t k = 0; k < obj_.size(); ++k) {
        if (k) out.push_back(',');
        newline_indent(depth + 1);
        escape_into(obj_[k].first, out);
        out.push_back(':');
        if (pretty) out.push_back(' ');
        obj_[k].second.dump_into(out, indent, depth + 1);
      }
      newline_indent(depth);
      out.push_back('}');
      break;
    }
  }
}

std::string Json::dump() const {
  std::string out;
  dump_into(out, 0, 0);
  return out;
}

std::string Json::dump_pretty(int indent) const {
  std::string out;
  dump_into(out, indent, 0);
  return out;
}

}  // namespace je
