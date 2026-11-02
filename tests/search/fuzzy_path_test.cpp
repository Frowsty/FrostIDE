// SPDX-License-Identifier: Apache-2.0
//
// Unit tests of the Go to File matcher (frost/search/fuzzy_path.hpp): CamelHump and word-start
// matches, path segments, ranking order and the "file:line" suffix.

#include "frost/search/fuzzy_path.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace frost::search {
namespace {

TEST(FuzzyPath, WordStartsMatchAcrossTheExtension) {
    EXPECT_TRUE(scoreWord("mcpp", "main.cpp").has_value());
    EXPECT_TRUE(scoreWord("fisy", "FileSystem.cpp").has_value());
    EXPECT_FALSE(scoreWord("xyz", "main.cpp").has_value());
    // A word-start match beats a scattered one, an exact name beats a prefix.
    EXPECT_GT(*scoreWord("fs", "FileSystem.cpp"), *scoreWord("fs", "offset.cpp"));
    EXPECT_GT(*scoreWord("main.cpp", "main.cpp"), *scoreWord("main.cpp", "main.cpp.bak"));
}

TEST(FuzzyPath, RanksMainCppFirstForMcpp) {
    const std::vector<std::string> paths{"src/app/mycomponent.hpp", "src/main.cpp", "docs/manual.txt",
                                         "src/main_window.cpp"};
    const std::vector<RankedPath> ranked = rankPaths("mcpp", paths);
    ASSERT_FALSE(ranked.empty());
    EXPECT_EQ(paths[ranked.front().index], "src/main.cpp");
    for (const RankedPath& hit : ranked) {
        EXPECT_NE(paths[hit.index], "docs/manual.txt");
    }
}

TEST(FuzzyPath, PathSegmentsMatchDirectoriesInOrder) {
    const PathMatcher matcher("src/ma");
    EXPECT_TRUE(matcher.score("src/app/main.cpp").has_value());
    EXPECT_TRUE(matcher.score("C:\\work\\src\\main.cpp").has_value());
    EXPECT_FALSE(matcher.score("tests/main.cpp").has_value());
    EXPECT_FALSE(PathMatcher("app/src/ma").score("src/app/main.cpp").has_value());
    // An empty last segment lists the directory's files.
    EXPECT_TRUE(PathMatcher("tests/").score("tests/main.cpp").has_value());
    // Without separators the whole path is the fallback.
    EXPECT_TRUE(PathMatcher("srcmain").score("src/main.cpp").has_value());
}

TEST(FuzzyPath, EmptyPatternKeepsTheOrderAndLimitCuts) {
    const std::vector<std::string> paths{"b.cpp", "a.cpp", "c.cpp"};
    const std::vector<RankedPath> all = rankPaths("  ", paths);
    ASSERT_EQ(all.size(), 3U);
    EXPECT_EQ(all[0].index, 0U);
    EXPECT_EQ(all[2].index, 2U);
    EXPECT_EQ(rankPaths("cpp", paths, 2).size(), 2U);
    // Ties: shorter, then byte order.
    const std::vector<std::string> ties{"zz/a.cpp", "a.cpp", "b/a.cpp"};
    const std::vector<RankedPath> ranked = rankPaths("a.cpp", ties);
    ASSERT_EQ(ranked.size(), 3U);
    EXPECT_EQ(ties[ranked[0].index], "a.cpp");
}

TEST(FuzzyPath, ParsesLocationSuffixes) {
    EXPECT_EQ(parseLocationQuery("main.cpp:3"), (LocationQuery{"main.cpp", 3, std::nullopt}));
    EXPECT_EQ(parseLocationQuery(" main.cpp:3:7 "), (LocationQuery{"main.cpp", 3, 7}));
    EXPECT_EQ(parseLocationQuery("main.cpp(12)"), (LocationQuery{"main.cpp", 12, std::nullopt}));
    EXPECT_EQ(parseLocationQuery("main.cpp:"), (LocationQuery{"main.cpp", std::nullopt, std::nullopt}));
    EXPECT_EQ(parseLocationQuery("C:\\src\\a.cpp"), (LocationQuery{"C:\\src\\a.cpp", std::nullopt, std::nullopt}));
    EXPECT_EQ(parseLocationQuery("a.cpp:0"), (LocationQuery{"a.cpp:0", std::nullopt, std::nullopt}));
}

} // namespace
} // namespace frost::search
