// Minimal RFC 8259 JSON value, parser and serialiser.
// Scope is deliberate (see docs/DESIGN_DECISIONS.md D-03): the API surface of this
// service is small and known, so a ~400 line recursive-descent parser is preferred to a
// 25k line dependency. Hostile input is bounded by a nesting-depth limit.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace je {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool b) : type_(Type::Bool), bool_(b) {}
  Json(int v) : Json(static_cast<int64_t>(v)) {}
  Json(int64_t v) : type_(Type::Number), int_(v), num_(static_cast<double>(v)), is_int_(true) {}
  Json(uint64_t v) : Json(static_cast<int64_t>(v)) {}
  Json(double v) : type_(Type::Number), num_(v) {}
  Json(const char* s) : type_(Type::String), str_(s ? s : "") {}
  Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

  static Json array() { Json j; j.type_ = Type::Array; return j; }
  static Json object() { Json j; j.type_ = Type::Object; return j; }
  static Json array(std::initializer_list<Json> items) {
    Json j = array();
    j.arr_.assign(items.begin(), items.end());
    return j;
  }

  // Parsing. Returns a Null value and sets *err on failure; check the bool result.
  static bool parse(std::string_view text, Json& out, std::string& err);
  static Json parse_or_null(std::string_view text);

  std::string dump() const;
  std::string dump_pretty(int indent = 2) const;

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }
  bool is_integer() const { return type_ == Type::Number && is_int_; }

  bool as_bool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
  double as_double(double def = 0.0) const { return type_ == Type::Number ? num_ : def; }
  int64_t as_int(int64_t def = 0) const;
  const std::string& as_string(const std::string& def = empty_string()) const {
    return type_ == Type::String ? str_ : def;
  }

  // Object access. find() returns nullptr when absent or when this is not an object.
  const Json* find(std::string_view key) const;
  bool has(std::string_view key) const { return find(key) != nullptr; }
  void set(std::string key, Json value);
  // Typed getters with defaults, for terse request parsing.
  int64_t get_int(std::string_view key, int64_t def) const;
  double get_double(std::string_view key, double def) const;
  bool get_bool(std::string_view key, bool def) const;
  std::string get_string(std::string_view key, std::string def) const;

  // Array access.
  void push_back(Json value);
  std::size_t size() const;
  const Json& at(std::size_t i) const;
  const std::vector<Json>& items() const { return arr_; }
  const std::vector<std::pair<std::string, Json>>& members() const { return obj_; }

  static void escape_into(std::string_view s, std::string& out);

 private:
  static const std::string& empty_string();
  void dump_into(std::string& out, int indent, int depth) const;

  Type type_ = Type::Null;
  bool bool_ = false;
  int64_t int_ = 0;
  double num_ = 0.0;
  bool is_int_ = false;
  std::string str_;
  std::vector<Json> arr_;
  std::vector<std::pair<std::string, Json>> obj_;
};

}  // namespace je
