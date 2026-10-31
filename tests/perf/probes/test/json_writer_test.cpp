// SPDX-License-Identifier: Apache-2.0
//
// Tests for the perf-report JSON writer (frost/perf/json_writer.hpp).

#include "frost/perf/json_writer.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>

namespace {

using frost::perf::JsonWriter;

TEST(JsonWriter, NestedObjectsAndArraysGetCommas) {
    JsonWriter json;
    json.beginObject();
    json.field("name", "s1");
    json.key("values").beginArray().value(1).value(2.5).value(true).null().endArray();
    json.key("inner").beginObject().field("n", std::uint64_t{7}).endObject();
    json.endObject();
    EXPECT_EQ(json.str(), R"({"name":"s1","values":[1,2.5,true,null],"inner":{"n":7}})");
}

TEST(JsonWriter, EscapesStrings) {
    JsonWriter json;
    json.value(std::string_view("a\"b\\c\n\x01"));
    EXPECT_EQ(json.str(), R"("a\"b\\c\n\u0001")");
}

TEST(JsonWriter, NonFiniteNumbersBecomeNull) {
    JsonWriter json;
    json.beginArray().value(std::numeric_limits<double>::quiet_NaN()).value(INFINITY).endArray();
    EXPECT_EQ(json.str(), "[null,null]");
}

TEST(JsonWriter, WritesSummaries) {
    frost::perf::Summary summary;
    summary.count = 2;
    summary.min = 1;
    summary.mean = 1.5;
    summary.p50 = 1;
    summary.p90 = 2;
    summary.p99 = 2;
    summary.max = 2;
    JsonWriter json;
    json.value(summary);
    EXPECT_EQ(json.str(), R"({"count":2,"min":1,"mean":1.5,"p50":1,"p90":2,"p99":2,"max":2})");
}

} // namespace
