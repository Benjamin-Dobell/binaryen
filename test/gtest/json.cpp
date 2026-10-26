#include "support/json.h"
#include "gtest/gtest.h"

using JSONTest = ::testing::Test;

TEST_F(JSONTest, Stringify) {
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

TEST_F(JSONTest, StringifyObjectAndAllValueTypes) {
  auto input =
    "{\"z\":null,\"number\":4294967295,\"nested\":[true,false,-1.25]}";
  auto* copy = strdup(input);
  json::Value value;
  value.parse(copy, json::Value::ASCII);
  // The existing ASCII parser does not unescape object keys. Construct actual
  // control characters through the typed API to exercise serializer escaping.
  value["a\"\n"] = json::Value::make("escaped\tvalue");
  std::stringstream ss;
  value.stringify(ss);
  EXPECT_EQ(ss.str(),
            "{\"a\\\"\\n\":\"escaped\\tvalue\",\"nested\":[true,false,-1.25],"
            "\"number\":4294967295,\"z\":null}");
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
