// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the fuzzy matcher (fuzzy.hpp) and the snippet parser (snippet.hpp): match kinds
// and positions, the word-start rule, and snippets with tab stops, placeholders, choices,
// variables, escapes and malformed input.

#include "frost/lang/fuzzy.hpp"
#include "frost/lang/snippet.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

using frost::lang::fuzzyMatch;
using frost::lang::MatchKind;
using frost::lang::parseSnippet;
using frost::lang::SnippetTabStop;

TEST(FuzzyMatch, ClassifiesPrefixWordStartsAndFuzzy) {
    EXPECT_EQ(fuzzyMatch("getValue", "getValue")->kind, MatchKind::Exact);
    EXPECT_EQ(fuzzyMatch("get", "getValue")->kind, MatchKind::Prefix);
    EXPECT_EQ(fuzzyMatch("GET", "getValue")->kind, MatchKind::PrefixIgnoreCase);
    auto humps = fuzzyMatch("gV", "getValue");
    ASSERT_TRUE(humps.has_value());
    EXPECT_EQ(humps->kind, MatchKind::WordStarts);
    EXPECT_EQ(humps->positions, (std::vector<std::uint32_t>{0, 3}));
    auto fuzzy = fuzzyMatch("gtv", "getValue");
    ASSERT_TRUE(fuzzy.has_value());
    EXPECT_EQ(fuzzy->kind, MatchKind::Fuzzy);
    // The same pattern scores higher on word starts than on letters inside a word.
    auto inside = fuzzyMatch("gV", "groove");
    ASSERT_TRUE(inside.has_value());
    EXPECT_EQ(inside->kind, MatchKind::Fuzzy);
    EXPECT_GT(humps->score, inside->score);
}

TEST(FuzzyMatch, TheFirstCharacterMustStartAWord) {
    EXPECT_FALSE(fuzzyMatch("etV", "getValue").has_value());
    EXPECT_FALSE(fuzzyMatch("xyz", "getValue").has_value());
    auto underscore = fuzzyMatch("fn", "file_name");
    ASSERT_TRUE(underscore.has_value());
    EXPECT_EQ(underscore->kind, MatchKind::WordStarts);
    EXPECT_EQ(underscore->positions, (std::vector<std::uint32_t>{0, 5}));
    EXPECT_TRUE(fuzzyMatch("", "anything").has_value());
}

TEST(FuzzyMatch, PrefersRunsAndWordStartsOverScatteredLetters) {
    // "fn" in "fontName": the word start N wins over the closer n of "font".
    auto match = fuzzyMatch("fn", "fontName");
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->positions, (std::vector<std::uint32_t>{0, 4}));
    EXPECT_EQ(match->kind, MatchKind::WordStarts);
    EXPECT_GT(fuzzyMatch("val", "value")->score, fuzzyMatch("val", "vXaXl")->score);
}

TEST(Snippet, PlainTextAndTabStops) {
    const auto snippet = parseSnippet("foo(${1:int x}, ${2:int y})$0");
    EXPECT_EQ(snippet.text, "foo(int x, int y)");
    ASSERT_EQ(snippet.tabStops.size(), 3u);
    EXPECT_EQ(snippet.tabStops[0], (SnippetTabStop{.index = 1, .begin = 4, .end = 9}));
    EXPECT_EQ(snippet.tabStops[1], (SnippetTabStop{.index = 2, .begin = 11, .end = 16}));
    EXPECT_EQ(snippet.tabStops[2], (SnippetTabStop{.index = 0, .begin = 17, .end = 17}));
    EXPECT_EQ(snippet.caret(), 4u);
}

TEST(Snippet, EscapesChoicesVariablesAndNesting) {
    const auto snippet = parseSnippet(R"(\$x ${1|a,b|} ${TM_FILENAME:def}$UNKNOWN ${2:outer ${3:inner}} \})");
    EXPECT_EQ(snippet.text, "$x a def outer inner }");
    ASSERT_EQ(snippet.tabStops.size(), 3u);
    EXPECT_EQ(snippet.tabStops[0].index, 1u);
    EXPECT_EQ(snippet.text.substr(snippet.tabStops[0].begin, snippet.tabStops[0].end - snippet.tabStops[0].begin), "a");
    EXPECT_EQ(snippet.tabStops[1].index, 2u);
    EXPECT_EQ(snippet.text.substr(snippet.tabStops[1].begin, snippet.tabStops[1].end - snippet.tabStops[1].begin),
              "outer inner");
    EXPECT_EQ(snippet.tabStops[2].index, 3u);
}

TEST(Snippet, MalformedInputStaysLiteral) {
    EXPECT_EQ(parseSnippet("${1:unclosed").text, "${1:unclosed");
    EXPECT_EQ(parseSnippet("cost $").text, "cost $");
    EXPECT_EQ(parseSnippet("a ${").text, "a ${");
    const auto plain = parseSnippet("noStops");
    EXPECT_TRUE(plain.tabStops.empty());
    EXPECT_EQ(plain.caret(), 7u);
}

} // namespace
