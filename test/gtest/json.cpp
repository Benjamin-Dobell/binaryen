#include "support/json.h"
#include "gtest/gtest.h"

using JSONTest = ::testing::Test;

TEST_F(JSONTest, RoundtripString) {
  // TODO: change the API to not require a copy
  auto input = "[\"hello\",\"world\"]";
  auto* copy = strdup(input);
  json::Value value;
  value.parse(copy, json::Value::ASCII);
  std::stringstream ss;
  value.stringify(ss);
  EXPECT_EQ(ss.str(), input);
  free(copy);
}

static void
checkOutput(json::Value::Ref ref, std::string expected, bool pretty = false) {
  std::stringstream ss;
  ref->stringify(ss, pretty);
  EXPECT_EQ(ss.str(), expected);
}

static void checkPrettyOutput(json::Value::Ref ref, std::string expected) {
  checkOutput(ref, expected, true);
}

TEST_F(JSONTest, StringifyArray) {
  auto array = json::Value::makeArray();
  array->push_back(json::Value::make(42));
  array->push_back(json::Value::make("1337"));
  array->push_back(json::Value::make()); // null
  checkOutput(array, "[42,\"1337\",null]");
  checkPrettyOutput(array, R"([
 42,
 "1337",
 null
])");
}

TEST_F(JSONTest, StringifyObject) {
  auto object = json::Value::makeObject();
  object["foo"] = json::Value::make(42);
  object["bar"] = json::Value::make("1337");
  checkOutput(object, "{\"foo\":42,\"bar\":\"1337\"}");
  checkPrettyOutput(object, R"({
 "foo": 42,
 "bar": "1337"
})");
}

TEST_F(JSONTest, StringifyNesting) {
  auto array = json::Value::makeArray();
  auto object = json::Value::makeObject();
  auto array1 = json::Value::makeArray();
  auto object1 = json::Value::makeObject();
  array->push_back(object);
  object["body"] = array1;
  array1->push_back(object1);
  object1["value"] = json::Value::make(42);
  checkPrettyOutput(array, R"([
 {
  "body": [
   {
    "value": 42
   }
  ]
 }
])");
}

TEST_F(JSONTest, StringifyObjectAndAllValueTypes) {
  auto input =
    "{\"z\":null,\"number\":4294967295,\"nested\":[true,false,-1.25]}";
  auto* copy = strdup(input);
  json::Value value;
  value.parse(copy, json::Value::ASCII);
  // Keep upstream insertion order while escaping keys as well as string values.
  // Construct actual control characters through the typed API.
  value["a\"\n"] = json::Value::make("escaped\tvalue");
  std::stringstream ss;
  value.stringify(ss);
  EXPECT_EQ(ss.str(),
            "{\"z\":null,\"number\":4294967295,\"nested\":[true,false,-1.25],"
            "\"a\\\"\\n\":\"escaped\\tvalue\"}");
  free(copy);
}

TEST_F(JSONTest, StringifyNumbersRoundTripAndIgnoreStreamFlags) {
  for (auto number : {0.0,
                      -0.0,
                      0.1,
                      std::numeric_limits<double>::min(),
                      std::numeric_limits<double>::max(),
                      std::numeric_limits<double>::denorm_min()}) {
    json::Value value(number);
    std::stringstream ss;
    ss << std::fixed << std::setprecision(1);
    value.stringify(ss);
    auto text = ss.str();
    json::Value parsed;
    parsed.parse(text.data(), json::Value::ASCII);
    EXPECT_EQ(parsed.getNumber(), number);
    EXPECT_EQ(std::signbit(parsed.getNumber()), std::signbit(number));
  }
}

TEST_F(JSONTest, StringifyNonfiniteNumbersAsNull) {
  for (auto number : {std::numeric_limits<double>::infinity(),
                      -std::numeric_limits<double>::infinity(),
                      std::numeric_limits<double>::quiet_NaN()}) {
    json::Value value(number);
    std::stringstream ss;
    value.stringify(ss);
    EXPECT_EQ(ss.str(), "null");
  }
}
