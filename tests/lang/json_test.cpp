// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for frost::lang::Json (json.hpp): building, parsing, dumping, numbers, member order,
// duplicate keys, escapes and the error cases.

#include "frost/lang/json.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using frost::lang::Json;
using frost::lang::parseJson;

TEST(Json, BuildsAndDumpsCompactly) {
    const Json value = Json::object()
                           .set("b", 1)
                           .set("a", Json::array({Json(true), Json(), Json("x\"y\n")}))
                           .set("c", 2.5)
                           .set("b", 3); // Replaced in place.
    EXPECT_EQ(value.dump(), R"({"b":3,"a":[true,null,"x\"y\n"],"c":2.5})");
    ASSERT_NE(value.find("a"), nullptr);
    EXPECT_EQ(value.find("a")->size(), 3U);
    EXPECT_EQ(value.integerAt("b"), 3);
    EXPECT_EQ(value.stringAt("missing", "fallback"), "fallback");
    EXPECT_EQ(value.find("missing"), nullptr);
}

TEST(Json, ParsesAndRoundTrips) {
    const std::string text = R"({"id":7,"method":"x/y","params":{"list":[1,-2,3.25,"é😀"],"ok":false}})";
    auto parsed = parseJson(text);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().describe();
    EXPECT_EQ(parsed->integerAt("id"), 7);
    EXPECT_EQ(parsed->stringAt("method"), "x/y");
    const Json* list = parsed->find("params")->find("list");
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->size(), 4U);
    EXPECT_EQ(list->items()[1].asInteger(), -2);
    EXPECT_EQ(list->items()[2].asDouble(), 3.25);
    EXPECT_EQ(*list->items()[3].asString(), "\xC3\xA9\xF0\x9F\x98\x80");
    EXPECT_EQ(parsed->find("params")->boolAt("ok"), false);
    auto again = parseJson(parsed->dump());
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*again, *parsed);
}

TEST(Json, NumbersKeepTheirKinds) {
    auto parsed = parseJson(R"([9223372036854775807, 18446744073709551615, 1e3, 2.0, -0])");
    ASSERT_TRUE(parsed.has_value());
    const auto& items = parsed->items();
    EXPECT_EQ(items[0].kind(), Json::Kind::Integer);
    EXPECT_EQ(items[1].kind(), Json::Kind::Double); // Beyond int64.
    EXPECT_EQ(items[2].asInteger(), 1000);          // An exact double reads as an integer.
    EXPECT_EQ(items[3].asInteger(), 2);
    EXPECT_EQ(Json(1), Json(1.0));
    EXPECT_NE(Json(1), Json("1"));
}

TEST(Json, DuplicateKeysKeepTheLastValue) {
    auto parsed = parseJson(R"({"a":1,"b":2,"a":3})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->keys().size(), 2U);
    EXPECT_EQ(parsed->integerAt("a"), 3);
}

TEST(Json, ControlCharactersAreEscaped) { EXPECT_EQ(Json(std::string("\x01\t\\", 3)).dump(), R"("\u0001\t\\")"); }

TEST(Json, RejectsMalformedText) {
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "nul", "1 2", "{\"a\":1}}"}) {
        auto parsed = parseJson(bad);
        ASSERT_FALSE(parsed.has_value()) << bad;
        EXPECT_EQ(parsed.error().code(), frost::ErrorCode::Corrupt) << bad;
    }
}

TEST(Json, RejectsExcessiveNesting) {
    const std::string deep = std::string(300, '[') + std::string(300, ']');
    auto parsed = parseJson(deep);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code(), frost::ErrorCode::Corrupt);
}

} // namespace
