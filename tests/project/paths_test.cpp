// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost::project's private path helpers (src/project/src/paths.hpp): MSBuild-style
// resolution, keys, name matching, patterns and wildcard limits.
#include "paths.hpp"
#include "project_test_util.hpp"
#include <gtest/gtest.h>

namespace frost::project::detail {
namespace {

using test::MemTree;

constexpr bool kWindows = io::kNativePathStyle == io::PathStyle::Windows;

TEST(ProjectPaths, ResolvesRelativePathsLikeMsbuild) {
    const std::string base = MemTree::path("sln/App");
    EXPECT_EQ(resolvePath(base, "src\\main.cpp"), MemTree::path("sln/App/src/main.cpp"));
    EXPECT_EQ(resolvePath(base, "..\\..\\shared\\x.h"), MemTree::path("shared/x.h"));
    EXPECT_EQ(resolvePath(base, ".\\a/./b.cpp"), MemTree::path("sln/App/a/b.cpp"));
    EXPECT_EQ(resolvePath(base, "  spaced.cpp  "), MemTree::path("sln/App/spaced.cpp"));
    EXPECT_EQ(resolvePath(base, MemTree::path("abs/x.cpp")), MemTree::path("abs/x.cpp"));
    // ".." never climbs above the root.
    EXPECT_EQ(resolvePath(base, "..\\..\\..\\..\\..\\top.h"), kWindows ? "C:\\top.h" : "/top.h");
    EXPECT_FALSE(resolvePath(base, "").has_value());
    EXPECT_FALSE(resolvePath(base, std::string_view("a\0b", 3)).has_value());
}

TEST(ProjectPaths, KeysFoldCaseOnWindowsOnly) {
    EXPECT_EQ(pathKey("ABC"), kWindows ? "abc" : "ABC");
    EXPECT_TRUE(isSameOrUnder(MemTree::path("a/b"), MemTree::path("a")));
    EXPECT_TRUE(isSameOrUnder(MemTree::path("a"), MemTree::path("a")));
    EXPECT_FALSE(isSameOrUnder(MemTree::path("ab"), MemTree::path("a")));
    EXPECT_TRUE(isSameOrUnder(MemTree::path("a"), kWindows ? "C:\\" : "/"));
    EXPECT_EQ(isSameOrUnder(MemTree::path("A/b"), MemTree::path("a")), kWindows);
}

TEST(ProjectPaths, MatchesNames) {
    EXPECT_TRUE(matchName("*.cpp", "main.cpp"));
    EXPECT_TRUE(matchName("*.cpp", ".cpp"));
    EXPECT_FALSE(matchName("*.cpp", "main.cpp.bak"));
    EXPECT_TRUE(matchName("m?in.*", "main.h"));
    EXPECT_TRUE(matchName("*a*b*", "xxaxxbxx"));
    EXPECT_FALSE(matchName("*a*b*", "xxbxxaxx"));
    EXPECT_TRUE(matchName("*", ""));
    EXPECT_FALSE(matchName("?", ""));
    EXPECT_EQ(matchName("*.CPP", "main.cpp"), kWindows);
}

TEST(ProjectPaths, MatchesPatterns) {
    const std::string base = MemTree::path("p");
    auto all = PathPattern::make(base, "src\\**");
    ASSERT_TRUE(all.has_value()) << all.error().describe();
    EXPECT_TRUE(all->matches(MemTree::path("p/src/a.cpp")));
    EXPECT_TRUE(all->matches(MemTree::path("p/src/x/y/z.h")));
    EXPECT_FALSE(all->matches(MemTree::path("p/src")));
    EXPECT_FALSE(all->matches(MemTree::path("p/other/a.cpp")));

    auto deep = PathPattern::make(base, "**\\test_*.cpp");
    ASSERT_TRUE(deep.has_value());
    EXPECT_TRUE(deep->matches(MemTree::path("p/test_a.cpp")));
    EXPECT_TRUE(deep->matches(MemTree::path("p/x/y/test_b.cpp")));
    EXPECT_FALSE(deep->matches(MemTree::path("p/x/y/a_test.cpp")));

    auto literal = PathPattern::make(base, "a\\..\\b.cpp");
    ASSERT_TRUE(literal.has_value());
    EXPECT_TRUE(literal->matches(MemTree::path("p/b.cpp")));
    EXPECT_TRUE(literal->components().empty());

    EXPECT_FALSE(PathPattern::make(base, "src\\*\\..\\x.cpp").has_value());
}

TEST(ProjectPaths, ExpandsWildcardsWithinLimits) {
    MemTree tree;
    for (int i = 0; i < 5; ++i) {
        tree.write("p/d" + std::to_string(i) + "/f.cpp", "");
    }
    auto pattern = PathPattern::make(MemTree::path("p"), "**\\*.cpp");
    ASSERT_TRUE(pattern.has_value());
    auto all = expandWildcard(tree.fs, *pattern);
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->paths.size(), 5u);
    EXPECT_TRUE(all->warning.empty()) << all->warning;

    auto capped = expandWildcard(tree.fs, *pattern, WildcardLimits{.maxDirectories = 100, .maxFiles = 2});
    ASSERT_TRUE(capped.has_value());
    EXPECT_EQ(capped->paths.size(), 2u);
    EXPECT_NE(capped->warning.find("stopped"), std::string::npos) << capped->warning;

    auto missing = PathPattern::make(MemTree::path("nowhere"), "*.cpp");
    ASSERT_TRUE(missing.has_value());
    auto none = expandWildcard(tree.fs, *missing);
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none->paths.empty());
    EXPECT_TRUE(none->warning.empty());
}

} // namespace
} // namespace frost::project::detail
