// SPDX-License-Identifier: Apache-2.0
// Installed-dictionary source oracles; no dictionary persistence or user preference mutations.
#include "frost/lang/spelling.hpp"

#include <gtest/gtest.h>

#include <algorithm>
using namespace frost;
TEST(Spelling, InvalidAndOversizedRequestsNeverBecomeDictionaryMutations) {
    EXPECT_FALSE(lang::checkSpelling(std::string(65537, 'a'), "en-US"));
    EXPECT_FALSE(lang::addSpellingWord("two words", "en-US"));
    EXPECT_FALSE(lang::addSpellingWord("", "en-US"));
    EXPECT_FALSE(lang::checkSpelling("word", std::string(129, 'x')));
}
#ifdef _WIN32
TEST(SpellingWindows, ActualInstalledEnglishProviderReturnsUtf8RangesAndRealSuggestions) {
    auto languages = lang::spellingLanguages();
    ASSERT_TRUE(languages) << languages.error().describe();
    ASSERT_NE(std::ranges::find(*languages, "en-US"), languages->end())
        << "This oracle requires the installed en-US Windows spelling dictionary";
    const std::string prefix = "\xf0\x9f\x99\x82 This ";
    auto checked = lang::checkSpelling(prefix + "wrold.", "en-US");
    ASSERT_TRUE(checked) << checked.error().describe();
    const auto issue = std::ranges::find(*checked, "wrold", &lang::SpellingIssue::word);
    ASSERT_NE(issue, checked->end());
    EXPECT_EQ(issue->begin, prefix.size());
    EXPECT_EQ(issue->end, prefix.size() + 5);
    EXPECT_NE(std::ranges::find(issue->suggestions, "world"), issue->suggestions.end());
    EXPECT_FALSE(lang::checkSpelling("text", "not-a-real-installed-language"));
}
#else
TEST(Spelling, UnsupportedPlatformDoesNotInventDictionaryResults) {
    auto result = lang::spellingLanguages();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
}
#endif
