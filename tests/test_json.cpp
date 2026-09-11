#include <gtest/gtest.h>

#include "jobengine/json.hpp"

using je::Json;

namespace {

Json must_parse(const std::string& text) {
  Json j;
  std::string err;
  EXPECT_TRUE(Json::parse(text, j, err)) << text << " -> " << err;
  return j;
}

void expect_reject(const std::string& text) {
  Json j;
  std::string err;
  EXPECT_FALSE(Json::parse(text, j, err)) << "expected rejection of: " << text;
  EXPECT_FALSE(err.empty());
}

}  // namespace

TEST(JsonTest, ParsesScalars) {
  EXPECT_TRUE(must_parse("null").is_null());
  EXPECT_TRUE(must_parse("true").as_bool());
  EXPECT_FALSE(must_parse("false").as_bool(true));
  EXPECT_EQ(must_parse("42").as_int(), 42);
  EXPECT_EQ(must_parse("-17").as_int(), -17);
  EXPECT_DOUBLE_EQ(must_parse("3.5").as_double(), 3.5);
  EXPECT_DOUBLE_EQ(must_parse("1e3").as_double(), 1000.0);
  EXPECT_EQ(must_parse("\"hello\"").as_string(), "hello");
}

TEST(JsonTest, DistinguishesIntegersFromDoubles) {
  EXPECT_TRUE(must_parse("42").is_integer());
  EXPECT_FALSE(must_parse("42.0").is_integer());
  // Large millisecond timestamps must survive a round trip without scientific notation.
  const int64_t ts = 1757000000123LL;
  EXPECT_EQ(Json(ts).dump(), "1757000000123");
  EXPECT_EQ(must_parse("1757000000123").as_int(), ts);
}

TEST(JsonTest, ParsesNestedStructures) {
  const Json j = must_parse(R"({"a":[1,2,{"b":null}],"c":{"d":true}})");
  ASSERT_TRUE(j.is_object());
  ASSERT_NE(j.find("a"), nullptr);
  EXPECT_EQ(j.find("a")->size(), 3u);
  EXPECT_EQ(j.find("a")->at(1).as_int(), 2);
  EXPECT_TRUE(j.find("a")->at(2).find("b")->is_null());
  EXPECT_TRUE(j.find("c")->find("d")->as_bool());
}

TEST(JsonTest, HandlesEscapesAndSurrogatePairs) {
  EXPECT_EQ(must_parse(R"("a\nb\tc\"d\\e")").as_string(), "a\nb\tc\"d\\e");
  EXPECT_EQ(must_parse(R"("\u0041")").as_string(), "A");
  EXPECT_EQ(must_parse(R"("\u00e9")").as_string(), "\xc3\xa9");        // e-acute, 2 bytes
  EXPECT_EQ(must_parse(R"("\u20ac")").as_string(), "\xe2\x82\xac");    // euro sign, 3 bytes
  EXPECT_EQ(must_parse(R"("\ud83d\ude00")").as_string(),
            "\xf0\x9f\x98\x80");                                       // emoji, 4 bytes
}

TEST(JsonTest, RejectsMalformedInput) {
  expect_reject("");
  expect_reject("{");
  expect_reject("[1,2");
  expect_reject("{\"a\"}");
  expect_reject("{\"a\":}");
  expect_reject("{a:1}");
  expect_reject("[1,]");
  expect_reject("tru");
  expect_reject("01");                 // leading zero is not valid JSON
  expect_reject("1.");
  expect_reject("1e");
  expect_reject("\"unterminated");
  expect_reject("\"bad \\x escape\"");
  expect_reject("\"\\ud83d\"");        // unpaired high surrogate
  expect_reject("\"\\ude00\"");        // unpaired low surrogate
  expect_reject("{} trailing");
  expect_reject(std::string("\"raw control ") + '\n' + "\"");
}

TEST(JsonTest, EnforcesNestingDepthLimit) {
  // A hostile input must be rejected rather than overflow the parser's stack.
  std::string deep;
  for (int i = 0; i < 200; ++i) deep += '[';
  for (int i = 0; i < 200; ++i) deep += ']';
  Json j;
  std::string err;
  EXPECT_FALSE(Json::parse(deep, j, err));
  EXPECT_NE(err.find("depth"), std::string::npos);
}

TEST(JsonTest, SerialisesAndRoundTrips) {
  Json obj = Json::object();
  obj.set("name", Json(std::string("job \"x\"\n")));
  obj.set("count", Json(static_cast<int64_t>(7)));
  obj.set("ok", Json(true));
  obj.set("nothing", Json());
  Json arr = Json::array();
  arr.push_back(Json(1));
  arr.push_back(Json(std::string("two")));
  obj.set("list", arr);

  const std::string text = obj.dump();
  const Json again = must_parse(text);
  EXPECT_EQ(again.get_string("name", ""), "job \"x\"\n");
  EXPECT_EQ(again.get_int("count", 0), 7);
  EXPECT_TRUE(again.get_bool("ok", false));
  EXPECT_TRUE(again.find("nothing")->is_null());
  EXPECT_EQ(again.find("list")->at(1).as_string(), "two");
}

TEST(JsonTest, ObjectSetReplacesRatherThanDuplicates) {
  Json o = Json::object();
  o.set("k", Json(1));
  o.set("k", Json(2));
  EXPECT_EQ(o.size(), 1u);
  EXPECT_EQ(o.get_int("k", 0), 2);
}

TEST(JsonTest, TypedGettersFallBackOnMismatch) {
  const Json j = must_parse(R"({"n":5,"s":"text","b":true})");
  EXPECT_EQ(j.get_int("n", -1), 5);
  EXPECT_EQ(j.get_int("missing", -1), -1);
  EXPECT_EQ(j.get_int("s", -1), -1);        // wrong type falls back to the default
  EXPECT_EQ(j.get_string("s", "d"), "text");
  EXPECT_EQ(j.get_string("n", "d"), "d");
  EXPECT_TRUE(j.get_bool("b", false));
  EXPECT_FALSE(j.get_bool("n", false));
}

TEST(JsonTest, EmptyContainersSerialiseCorrectly) {
  EXPECT_EQ(Json::object().dump(), "{}");
  EXPECT_EQ(Json::array().dump(), "[]");
  EXPECT_TRUE(must_parse("{}").is_object());
  EXPECT_TRUE(must_parse("[]").is_array());
}
