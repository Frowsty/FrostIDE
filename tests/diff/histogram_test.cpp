// SPDX-License-Identifier: Apache-2.0
//
// Histogram goldens (ADR 0019, D3, with D5 and D6): the worked examples and the 64/65 chain
// boundary measured on git 2.43 and 2.56, the twelve dialect canaries (git's classic dialect), the
// unique-anchor behaviour that separates histogram from minimal diffs, and the absence of
// prefix/suffix trimming.

#include "frost/diff/line_diff.hpp"

#include "diff/diff_test_util.hpp"
#include "diff/support/goldens.hpp"
#include <gtest/gtest.h>

namespace frost::diff::test {
namespace {

LineDiff diffText(const std::string& a, const std::string& b, const DiffOptions& options = {}) {
    const LineSequence sa = makeSequence(a, LineSplit::Lf);
    const LineSequence sb = makeSequence(b, LineSplit::Lf);
    auto diff = diffLines(sa, sb, options);
    EXPECT_TRUE(diff.has_value()) << diff.error().describe();
    if (!diff) {
        return {};
    }
    EXPECT_TRUE(appliesCleanly(sa, sb, *diff, options));
    return std::move(*diff);
}

TEST(Histogram, WorkedExamples) {
    auto goldens = loadHistogramGoldens(testDataPath("goldens/histogram"));
    ASSERT_TRUE(goldens.has_value()) << goldens.error().describe();
    ASSERT_GE(goldens->size(), 13u); // 4 boundary, 3 trimming, 3 alignment, 1 single pass, 2 absorption.
    for (const HistogramGolden& golden : *goldens) {
        const LineDiff diff = diffText(golden.a, golden.b);
        EXPECT_EQ(formatHunks(hunkTuples(diff)), formatHunks(golden.hunks)) << golden.name;
        EXPECT_FALSE(diff.degraded) << golden.name;
    }
}

TEST(Histogram, ChainBoundary) {
    // A = x^n y^(n+1), B = y^(n+1) x^n: up to 64 repeats the histogram finds the LCS itself; at 65
    // the region falls back to Myers (JGit's maxChainLength rule).
    const std::uint32_t expectedFallbacks[] = {0, 0, 1, 1};
    for (std::uint32_t n = 63; n <= 66; ++n) {
        const std::string a = digitLines(std::string(n, '0') + std::string(n + 1, '1'));
        const std::string b = digitLines(std::string(n + 1, '1') + std::string(n, '0'));
        const LineDiff diff = diffText(a, b);
        EXPECT_EQ(diff.fallbacks, expectedFallbacks[n - 63]) << "n=" << n;
        HunkList expected;
        if (n <= 64) {
            expected = {{0, 0, 0, n + 1}, {n, 2 * n + 1, 2 * n + 1, 2 * n + 1}};
        } else {
            expected = {{0, n, 0, 0}, {2 * n + 1, 2 * n + 1, n + 1, 2 * n + 1}};
        }
        EXPECT_EQ(formatHunks(hunkTuples(diff)), formatHunks(expected)) << "n=" << n;
    }
}

TEST(Histogram, DialectCanaries) {
    auto canaries = loadDialectGoldens(testDataPath("goldens/histogram/dialect"));
    ASSERT_TRUE(canaries.has_value()) << canaries.error().describe();
    ASSERT_EQ(canaries->size(), 12u);
    for (const DialectGolden& canary : *canaries) {
        ASSERT_NE(canary.classic, canary.fresh) << canary.name;
        const LineDiff diff = diffText(canary.a, canary.b);
        EXPECT_EQ(formatHunks(hunkTuples(diff)), formatHunks(canary.classic)) << canary.name;
        // The new dialect always changes strictly fewer lines (ADR 0019, Measurements).
        std::uint64_t classicLines = 0;
        std::uint64_t newLines = 0;
        for (const HunkTuple& h : canary.classic) {
            classicLines += (h[1] - h[0]) + (h[3] - h[2]);
        }
        for (const HunkTuple& h : canary.fresh) {
            newLines += (h[1] - h[0]) + (h[3] - h[2]);
        }
        EXPECT_LT(newLines, classicLines) << canary.name;
    }
}

TEST(Histogram, PatienceUniqueAnchor) {
    // The unique line 9 anchors the match although "000" or "111" would be a longer common
    // subsequence: histogram output, not minimal.
    const LineDiff histogram = diffText(digitLines("0009111"), digitLines("1119000"));
    EXPECT_EQ(formatHunks(hunkTuples(histogram)), "[(0,3,0,3),(4,7,4,7)]");
    DiffOptions myers;
    myers.algorithm = Algorithm::Myers;
    const LineDiff minimal = diffText(digitLines("0009111"), digitLines("1119000"), myers);
    EXPECT_EQ(minimal.deletedLines() + minimal.insertedLines(), 8u); // 7 + 7 - 2 * 3.
    EXPECT_LT(minimal.deletedLines() + minimal.insertedLines(), histogram.deletedLines() + histogram.insertedLines());
    // An anchor on a unique line inside a repetitive block.
    const LineDiff anchored = diffText(digitLines("1112111"), digitLines("1211121"));
    EXPECT_TRUE(appliesCleanly(makeSequence(digitLines("1112111"), LineSplit::Lf),
                               makeSequence(digitLines("1211121"), LineSplit::Lf), anchored));
}

TEST(Histogram, NoTrimming) {
    // JGit's DiffAlgorithm trims the common prefix and suffix first; git does not. This pair is
    // one where the trimmed variant of the prototype disagreed with git (ADR 0019, Measurements).
    const LineDiff diff = diffText(digitLines("00011"), digitLines("100111"));
    EXPECT_EQ(formatHunks(hunkTuples(diff)), "[(0,1,0,1),(5,5,5,6)]");
}

TEST(Histogram, SlidersNoneKeepsTheAlgorithmOutput) {
    // The same input with and without compaction: both valid, and GitClassic only moves changes.
    DiffOptions none;
    none.sliders = Sliders::None;
    const LineDiff raw = diffText(digitLines("021"), digitLines("11"), none);
    const LineDiff compacted = diffText(digitLines("021"), digitLines("11"));
    EXPECT_EQ(raw.deletedLines(), compacted.deletedLines());
    EXPECT_EQ(raw.insertedLines(), compacted.insertedLines());
    EXPECT_EQ(formatHunks(hunkTuples(compacted)), "[(0,2,0,1)]");
}

} // namespace
} // namespace frost::diff::test
