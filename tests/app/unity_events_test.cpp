// SPDX-License-Identifier: Apache-2.0
// Authenticated journal payload fixtures: complete-page admission, sequence/loss/continuation and field bounds.
#include "../../src/app/src/unity_events.hpp"
#include <gtest/gtest.h>
namespace frost::app {
namespace {
lang::Json entry(std::int64_t sequence) {
    return lang::Json::object().set("sequence", sequence).set("kind", "compiler").set("level", "error")
        .set("message", "CS1002: fixture compiler error").set("stack", "").set("file", "Assets/Actual.cs")
        .set("assembly", "Assembly-CSharp.dll").set("line", 7).set("column", 9).set("truncated", false);
}
lang::Json page(std::int64_t first, std::int64_t next, std::int64_t last, std::int64_t lost, lang::Json events) {
    return lang::Json::object().set("eventFirst", first).set("eventNext", next).set("eventLast", last).set("eventLost", lost)
        .set("projectVersion", 2).set("compiling", true).set("events", std::move(events));
}
TEST(UnityEvents, PreservesActualCompilerIdentityAndContinuation) {
    const auto source = page(1, 1, 3, 0, lang::Json::array().push(entry(1)));
    const auto result = parseUnityEvents(source, 0);
    ASSERT_TRUE(result); ASSERT_EQ(result->events.size(), 1U);
    EXPECT_EQ(result->events.front().file, "Assets/Actual.cs");
    EXPECT_EQ(result->events.front().message, "CS1002: fixture compiler error");
    EXPECT_EQ(result->events.front().line, 7); EXPECT_EQ(result->events.front().column, 9);
    EXPECT_EQ(result->next, 1); EXPECT_EQ(result->last, 3); EXPECT_TRUE(result->compiling);
}
TEST(UnityEvents, ReportsExactLostPrefixAndRejectsSilentGapsOrImpossibleContinuation) {
    ASSERT_TRUE(parseUnityEvents(page(10, 10, 12, 9, lang::Json::array().push(entry(10))), 0));
    EXPECT_FALSE(parseUnityEvents(page(10, 10, 12, 0, lang::Json::array().push(entry(10))), 0));
    EXPECT_FALSE(parseUnityEvents(page(1, 2, 2, 0, lang::Json::array().push(entry(2))), 0));
    EXPECT_FALSE(parseUnityEvents(page(1, 2, 2, 0, lang::Json::array().push(entry(1))), 0));
    EXPECT_FALSE(parseUnityEvents(page(1, 0, 2, 0, lang::Json::array()), 0));
}
TEST(UnityEvents, InvalidTailRejectsTheWholeBatchAndOversizedFieldsStayErrors) {
    auto bad = entry(2); bad.set("line", -1);
    EXPECT_FALSE(parseUnityEvents(page(1, 2, 2, 0, lang::Json::array().push(entry(1)).push(bad)), 0));
    bad = entry(1); bad.set("message", std::string(2049, 'x'));
    EXPECT_FALSE(parseUnityEvents(page(1, 1, 1, 0, lang::Json::array().push(bad)), 0));
    auto empty = page(1, 0, 0, 0, lang::Json::array());
    EXPECT_TRUE(parseUnityEvents(empty, 0));
    EXPECT_FALSE(parseUnityEvents(empty, 1));
}
}
}
