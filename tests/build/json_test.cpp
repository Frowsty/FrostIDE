// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost::build's private JSON reader (src/build/src/json.hpp).

#include "json.hpp"
#include <gtest/gtest.h>

#include <string>

namespace {

using frost::build::detail::JsonValue;
using frost::build::detail::parseJson;

TEST(Json, ParsesNestedValues) {
    auto value = parseJson("\xEF\xBB\xBF [ {\"a\": \"x\\\\y\", \"n\": -1.5e3, \"t\": true, \"z\": null, "
                           "\"o\": {\"k\": [1, 2]}} ] ");
    ASSERT_TRUE(value.has_value()) << value.error().describe();
    ASSERT_EQ(value->kind, JsonValue::Kind::Array);
    ASSERT_EQ(value->items.size(), 1U);
    const JsonValue& object = value->items[0];
    EXPECT_EQ(object.stringMember("a"), "x\\y");
    EXPECT_EQ(object.find("n")->text, "-1.5e3");
    EXPECT_TRUE(object.boolMember("t"));
    EXPECT_EQ(object.find("z")->kind, JsonValue::Kind::Null);
    EXPECT_EQ(object.find("o")->find("k")->items.size(), 2U);
    EXPECT_EQ(object.find("missing"), nullptr);
    EXPECT_EQ(object.stringMember("t"), ""); // Not a string.
}

TEST(Json, DecodesEscapes) {
    auto value = parseJson(R"("\u00e9\ud83d\ude00\n\t\"\/")");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->text, "\xC3\xA9\xF0\x9F\x98\x80\n\t\"/");
}

TEST(Json, RejectsMalformedInput) {
    for (const std::string_view bad :
         {"", "[", "[1,]", "{\"a\" 1}", "\"\\ud800\"", "\"\\x\"", "01", "[1] 2", "tru", "\"a\nb\"", "{\"a\":}"}) {
        auto value = parseJson(bad);
        ASSERT_FALSE(value.has_value()) << bad;
        EXPECT_EQ(value.error().code(), frost::ErrorCode::Corrupt) << bad;
    }
}

TEST(Json, LimitsNesting) {
    EXPECT_TRUE(parseJson(std::string(60, '[') + std::string(60, ']')).has_value());
    EXPECT_FALSE(parseJson(std::string(100, '[') + std::string(100, ']')).has_value());
}

} // namespace
