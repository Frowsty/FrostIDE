// SPDX-License-Identifier: Apache-2.0
// Real pinned grammar structural matching: opaque trivia, exact captures, refusal and reparse boundaries.
#include "frost/syntax/structural_search.hpp"
#include "frost/text/text_buffer.hpp"
#include <gtest/gtest.h>
namespace frost::syntax {
namespace {
text::Snapshot snapshot(std::string bytes) { return text::TextBuffer::fromUtf8(std::move(bytes))->current(); }
TEST(StructuralSearch, CppCallPatternPreservesCapturedTriviaAndDoesNotMatchStringsOrComments) {
    const auto source = snapshot("void f(){foo(1 /* trivia */);const char* s=\"foo(1)\"; // foo(1)\n}\n");
    const std::string query = R"(((call_expression function: (identifier) @callee arguments: (argument_list) @args) @match (#eq? @callee "foo")))";
    auto result = structuralSearch(LanguageId::Cpp, source, query, "bar${args}");
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->matches.size(), 1U);
    EXPECT_EQ(result->matches[0].captures.at("args").text, "(1 /* trivia */)");
    EXPECT_EQ(*result->after, "void f(){bar(1 /* trivia */);const char* s=\"foo(1)\"; // foo(1)\n}\n");
}
TEST(StructuralSearch, LuaIdentifierQueryUsesActualSyntaxAndVerbatimReplacement) {
    auto result = structuralSearch(LanguageId::Lua, snapshot("local old=1\nprint(old)\n-- old\nlocal s='old'\n"),
        R"(((identifier) @match (#eq? @match "old")))", "fresh");
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->matches.size(), 2U);
    EXPECT_EQ(*result->after, "local fresh=1\nprint(fresh)\n-- old\nlocal s='old'\n");
}
TEST(StructuralSearch, RefusesInvalidReplacementSemanticPredicatesMissingCapturesAndIncompleteSyntax) {
    const auto source = snapshot("void f(){foo();}\n");
    EXPECT_FALSE(structuralSearch(LanguageId::Cpp, source, "(identifier) @name", "x"));
    EXPECT_FALSE(structuralSearch(LanguageId::Cpp, source, R"(((identifier) @match (#match? @match "foo")))", "x"));
    EXPECT_FALSE(structuralSearch(LanguageId::Cpp, source, R"(((identifier) @match (#type? @match "int")))", "x"));
    EXPECT_FALSE(structuralSearch(LanguageId::Cpp, source, R"(((identifier) @match (#eq? @match "foo")))", "${missing}"));
    EXPECT_FALSE(structuralSearch(LanguageId::Cpp, source, R"(((identifier) @match (#eq? @match "foo")))", "if"));
    EXPECT_FALSE(structuralSearch(LanguageId::Cpp, snapshot("void f( {"), "(identifier) @match"));
    EXPECT_FALSE(structuralSearch(LanguageId::Cpp, source, "((identifier) @match)+"));
}
TEST(StructuralSearch, NestedReplacementRefusesRatherThanApplyingAnArbitrarySubset) {
    const auto source = snapshot("void f(){foo(bar());}\n");
    auto searched = structuralSearch(LanguageId::Cpp, source, "(call_expression) @match");
    ASSERT_TRUE(searched); EXPECT_EQ(searched->matches.size(), 2U);
    auto replaced = structuralSearch(LanguageId::Cpp, source, "(call_expression) @match", "other()");
    ASSERT_FALSE(replaced); EXPECT_EQ(replaced.error().code(), ErrorCode::Unsupported);
}
TEST(StructuralSearch, CancelledOrOversizedQueriesReturnNoPartialResult) {
    std::stop_source stopped; stopped.request_stop();
    auto result = structuralSearch(LanguageId::Lua, snapshot("local a=1"), "(identifier) @match", {}, stopped.get_token());
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_FALSE(structuralSearch(LanguageId::Lua, snapshot("local a=1"), std::string(16385, ' ')));
}
} // namespace
} // namespace frost::syntax
