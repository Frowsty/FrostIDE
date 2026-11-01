// SPDX-License-Identifier: Apache-2.0
//
// Compaction tests (ADR 0019, D5): GitClassic sliding to the lowest placement, alignment with the
// other side's changes, absorption of touching groups after each step, Sliders::None, the
// single-pass golden (git 2.43 and 2.56 print it), and the B-pass fixed point (a consistency check;
// re-running pass A is deliberately not asserted). Expected hunks were measured on both gits.

#include "compaction.hpp"
#include "diff/diff_test_util.hpp"
#include "diff/support/generators.hpp"
#include "engine.hpp"
#include "histogram.hpp"
#include "interner.hpp"
#include "work_budget.hpp"
#include <gtest/gtest.h>

#include <limits>

namespace frost::diff::test {
namespace {

constexpr std::uint64_t kUnlimited = std::numeric_limits<std::uint64_t>::max();

std::string hunksOf(std::string_view a, std::string_view b, Sliders sliders = Sliders::GitClassic) {
    const LineSequence sa = makeSequence(digitLines(a), LineSplit::Lf);
    const LineSequence sb = makeSequence(digitLines(b), LineSplit::Lf);
    DiffOptions options;
    options.sliders = sliders;
    auto diff = diffLines(sa, sb, options);
    EXPECT_TRUE(diff.has_value());
    if (!diff) {
        return {};
    }
    EXPECT_TRUE(appliesCleanly(sa, sb, *diff, options));
    return formatHunks(hunkTuples(*diff));
}

struct Ids {
    std::vector<std::uint32_t> a;
    std::vector<std::uint32_t> b;
    std::uint32_t count = 0;
};

Ids internBoth(const LineSequence& a, const LineSequence& b) {
    Ids ids;
    std::vector<std::uint32_t> lists[2];
    const LineSequence* sequences[] = {&a, &b};
    detail::WorkBudget budget(kUnlimited, {});
    ids.count = detail::internSequences(sequences, lists, budget);
    ids.a = std::move(lists[0]);
    ids.b = std::move(lists[1]);
    return ids;
}

void flagsFromHunks(const LineDiff& diff, std::vector<std::uint8_t>& changedA, std::vector<std::uint8_t>& changedB) {
    for (const Hunk& hunk : diff.hunks) {
        for (std::uint32_t i = hunk.a.begin.value(); i < hunk.a.end.value(); ++i) {
            changedA[i] = 1;
        }
        for (std::uint32_t j = hunk.b.begin.value(); j < hunk.b.end.value(); ++j) {
            changedB[j] = 1;
        }
    }
}

std::string flagHunks(const std::vector<std::uint8_t>& changedA, const std::vector<std::uint8_t>& changedB) {
    LineDiff diff;
    diff.hunks = detail::hunksFromFlags(changedA, changedB);
    return formatHunks(hunkTuples(diff));
}

TEST(Compaction, SlideDownLowest) {
    EXPECT_EQ(hunksOf("010", "01010"), "[(3,3,3,5)]");
    EXPECT_EQ(hunksOf("01010", "010"), "[(3,5,3,3)]");
    EXPECT_EQ(hunksOf("0110", "011110"), "[(3,3,3,5)]");
    EXPECT_EQ(hunksOf("2", "22222"), "[(1,1,1,5)]");
}

TEST(Compaction, AlignWithOtherSide) {
    // Slide-down alone gives [(0,2,0,0),(3,3,1,2)] for the first pair (ADR 0019, Measurements);
    // alignment keeps a change opposite the other side's change.
    EXPECT_EQ(hunksOf("021", "11"), "[(0,2,0,1)]");
    EXPECT_EQ(hunksOf("102", "0502"), "[(0,1,0,2)]");
    EXPECT_EQ(hunksOf("2422", "2452"), "[(2,3,2,3)]");
}

TEST(Compaction, MergesTouchingGroups) {
    // Absorbing touching groups after each slide step (the rule) differs from absorbing after the
    // slide loop: [(1,2,1,1),(3,5,2,2)] and [(0,1,0,0),(2,4,1,2)] respectively (found with the
    // prototype, confirmed on both gits).
    EXPECT_EQ(hunksOf("110200", "100"), "[(1,4,1,1)]");
    EXPECT_EQ(hunksOf("10100", "020"), "[(0,3,0,0),(4,4,1,2)]");
    EXPECT_EQ(hunksOf("0001011", "0011"), "[(2,5,2,2)]");
}

TEST(Compaction, NoneKeepsRaw) {
    // Sliders::None returns exactly the histogram's flags.
    SplitMix64 rng(0xD1FF0005);
    for (std::uint32_t c = 0; c < scaled(2000, 1000, 500); ++c) {
        TextPair pair = generatePair(PairClass::Adversarial, rng);
        const LineSequence sa = makeSequence(pair.a, LineSplit::Lf);
        const LineSequence sb = makeSequence(pair.b, LineSplit::Lf);
        if (sa.size() == 0 || sb.size() == 0 || sa.bytes() == sb.bytes()) {
            continue;
        }
        const Ids ids = internBoth(sa, sb);
        std::vector<std::uint8_t> changedA(ids.a.size(), 0);
        std::vector<std::uint8_t> changedB(ids.b.size(), 0);
        detail::WorkBudget budget(kUnlimited, {});
        (void)detail::histogramDiff(ids.a, ids.b, ids.count, changedA, changedB, budget);
        DiffOptions options;
        options.sliders = Sliders::None;
        auto diff = diffLines(sa, sb, options);
        ASSERT_TRUE(diff.has_value());
        EXPECT_EQ(formatHunks(hunkTuples(*diff)), flagHunks(changedA, changedB))
            << printable(pair.a) << " vs " << printable(pair.b);
    }
}

TEST(Compaction, SinglePassGolden) {
    // git 2.43 and 2.56 both print the single-pass result.
    EXPECT_EQ(hunksOf("011", "001"), "[(1,1,1,2),(2,3,3,3)]");
    // A second run of both passes would move it: compaction is deliberately not a fixed point.
    const LineSequence sa = makeSequence(digitLines("011"), LineSplit::Lf);
    const LineSequence sb = makeSequence(digitLines("001"), LineSplit::Lf);
    auto diff = diffLines(sa, sb);
    ASSERT_TRUE(diff.has_value());
    const Ids ids = internBoth(sa, sb);
    std::vector<std::uint8_t> changedA(ids.a.size(), 0);
    std::vector<std::uint8_t> changedB(ids.b.size(), 0);
    flagsFromHunks(*diff, changedA, changedB);
    detail::WorkBudget budget(kUnlimited, {});
    ASSERT_TRUE(detail::compactGitClassic(ids.a, ids.b, changedA, changedB, budget));
    EXPECT_EQ(flagHunks(changedA, changedB), "[(1,2,1,2)]");
}

TEST(Compaction, BPassFixedPoint) {
    // Consistency check (not an independent oracle): pass B on the final flags changes nothing.
    SplitMix64 rng(0xD1FF0006);
    const std::uint32_t perClass = scaled(1000, 300, 150);
    for (const PairClass kind : kAllPairClasses) {
        for (std::uint32_t c = 0; c < perClass; ++c) {
            TextPair pair = generatePair(kind, rng);
            const LineSequence sa = makeSequence(pair.a, LineSplit::Lf);
            const LineSequence sb = makeSequence(pair.b, LineSplit::Lf);
            auto diff = diffLines(sa, sb);
            ASSERT_TRUE(diff.has_value());
            ASSERT_FALSE(diff->degraded);
            const Ids ids = internBoth(sa, sb);
            std::vector<std::uint8_t> changedA(ids.a.size(), 0);
            std::vector<std::uint8_t> changedB(ids.b.size(), 0);
            flagsFromHunks(*diff, changedA, changedB);
            const std::vector<std::uint8_t> before = changedB;
            detail::WorkBudget budget(kUnlimited, {});
            ASSERT_TRUE(detail::slidePass(ids.b, changedB, changedA, budget));
            ASSERT_EQ(changedB, before) << pairClassName(kind) << " case " << c << ": " << printable(pair.a) << " vs "
                                        << printable(pair.b);
        }
    }
}

} // namespace
} // namespace frost::diff::test
