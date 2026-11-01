// SPDX-License-Identifier: Apache-2.0
//
// Unified renderer tests (ADR 0019, D9): every fixture pair rendered at -U0 and -U3 equals git
// 2.43's bodies (goldens/unified, written by DiffGit.WriteUnifiedGoldens; an independent oracle that
// needs no git at run time), empty output for identical inputs, header escaping and the
// "\ No newline at end of file" marker.

#include "frost/diff/unified.hpp"

#include "diff/diff_test_util.hpp"
#include "diff/support/files.hpp"
#include <gtest/gtest.h>

namespace frost::diff::test {
namespace {

TEST(Unified, GitGoldens) {
    const std::string fixtures = testDataPath("fixtures/pairs");
    auto names = listNames(fixtures, true);
    ASSERT_TRUE(names.has_value()) << names.error().describe();
    ASSERT_GE(names->size(), 30u);
    for (const std::string& name : *names) {
        auto a = readBinaryFile(joinUtf8(joinUtf8(fixtures, name), "a"));
        auto b = readBinaryFile(joinUtf8(joinUtf8(fixtures, name), "b"));
        ASSERT_TRUE(a.has_value() && b.has_value()) << name;
        const LineSequence sa = makeSequence(*a, LineSplit::Lf);
        const LineSequence sb = makeSequence(*b, LineSplit::Lf);
        auto diff = diffLines(sa, sb);
        ASSERT_TRUE(diff.has_value());
        ASSERT_TRUE(appliesCleanly(sa, sb, *diff)) << name;
        for (const std::uint32_t context : {0u, 3u}) {
            auto golden = readBinaryFile(testDataPath(std::format("goldens/unified/{}.u{}", name, context)));
            ASSERT_TRUE(golden.has_value()) << golden.error().describe();
            UnifiedOptions options;
            options.context = context;
            EXPECT_EQ(renderUnified(sa, sb, *diff, options), *golden) << name << " -U" << context;
        }
    }
}

TEST(Unified, EmptyForIdentical) {
    const LineSequence a = makeSequence("same\nlines\n", LineSplit::Lf);
    auto diff = diffLines(a, a);
    ASSERT_TRUE(diff.has_value());
    UnifiedOptions options;
    options.oldHeader = "a/x";
    options.newHeader = "b/x";
    EXPECT_EQ(renderUnified(a, a, *diff, options), "");
    EXPECT_EQ(renderUnified(LineSequence(), LineSequence(), LineDiff{}, options), "");
}

TEST(Unified, Headers) {
    const LineSequence a = makeSequence("x\n", LineSplit::Lf);
    const LineSequence b = makeSequence("y\n", LineSplit::Lf);
    auto diff = diffLines(a, b);
    ASSERT_TRUE(diff.has_value());
    UnifiedOptions options;
    options.oldHeader = "a/evil\n+++ b/x\\\r";
    options.newHeader = "b/name";
    EXPECT_EQ(renderUnified(a, b, *diff, options), "--- a/evil\\n+++ b/x\\\\\\r\n+++ b/name\n@@ -1 +1 @@\n-x\n+y\n");
    // One header alone writes none.
    options.newHeader.clear();
    EXPECT_EQ(renderUnified(a, b, *diff, options), "@@ -1 +1 @@\n-x\n+y\n");
}

TEST(Unified, NoNewlineMarker) {
    const LineSequence a = makeSequence("a\nb", LineSplit::Lf);
    const LineSequence b = makeSequence("a\nc", LineSplit::Lf);
    auto diff = diffLines(a, b);
    ASSERT_TRUE(diff.has_value());
    UnifiedOptions options;
    options.context = 1;
    EXPECT_EQ(renderUnified(a, b, *diff, options),
              "@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n+c\n\\ No newline at end of file\n");
    // Only the old side lacks a final newline.
    const LineSequence c = makeSequence("a\nb\n", LineSplit::Lf);
    auto added = diffLines(a, c);
    ASSERT_TRUE(added.has_value());
    options.context = 0;
    EXPECT_EQ(renderUnified(a, c, *added, options), "@@ -2 +2 @@\n-b\n\\ No newline at end of file\n+b\n");
    // Grouping: hunks at most 2 * context lines apart share a header.
    const LineSequence d = makeSequence("1\n2\n3\n4\n5\n6\n7\n8\n", LineSplit::Lf);
    const LineSequence e = makeSequence("1\nX\n3\n4\n5\n6\nY\n8\n", LineSplit::Lf);
    auto two = diffLines(d, e);
    ASSERT_TRUE(two.has_value());
    options.context = 2;
    EXPECT_EQ(renderUnified(d, e, *two, options), "@@ -1,8 +1,8 @@\n 1\n-2\n+X\n 3\n 4\n 5\n 6\n-7\n+Y\n 8\n");
    options.context = 1;
    EXPECT_EQ(renderUnified(d, e, *two, options), "@@ -1,3 +1,3 @@\n 1\n-2\n+X\n 3\n@@ -6,3 +6,3 @@\n 6\n-7\n+Y\n 8\n");
}

} // namespace
} // namespace frost::diff::test
