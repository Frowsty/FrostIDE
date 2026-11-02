// SPDX-License-Identifier: Apache-2.0
//
// Myers.MinimalVsDp (ADR 0019, D4): the linear-space Myers implementation produces minimal edit
// scripts, checked against the O(NM) dynamic-programming LCS (an independent oracle), directly on
// id sequences and through diffLines with Algorithm::Myers, with and without sliders.

#include "diff/diff_test_util.hpp"
#include "diff/support/generators.hpp"
#include "engine.hpp"
#include "myers.hpp"
#include "work_budget.hpp"
#include <gtest/gtest.h>

#include <limits>

namespace frost::diff::test {
namespace {

std::vector<std::uint32_t> randomIds(SplitMix64& rng, std::uint32_t alphabet, std::uint32_t maxLength) {
    std::vector<std::uint32_t> ids(rng.below(maxLength + 1));
    for (std::uint32_t& id : ids) {
        id = static_cast<std::uint32_t>(rng.below(alphabet));
    }
    return ids;
}

// The script is consistent: unchanged lines pair up in order and are equal.
::testing::AssertionResult consistent(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b,
                                      const std::vector<std::uint8_t>& changedA,
                                      const std::vector<std::uint8_t>& changedB) {
    std::size_t j = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (changedA[i] != 0) {
            continue;
        }
        while (j < b.size() && changedB[j] != 0) {
            ++j;
        }
        if (j == b.size() || a[i] != b[j]) {
            return ::testing::AssertionFailure() << "unchanged line " << i << " of A has no equal partner";
        }
        ++j;
    }
    while (j < b.size() && changedB[j] != 0) {
        ++j;
    }
    if (j != b.size()) {
        return ::testing::AssertionFailure() << "B has more unchanged lines than A";
    }
    return ::testing::AssertionSuccess();
}

std::string idText(const std::vector<std::uint32_t>& ids) {
    std::string out;
    for (const std::uint32_t id : ids) {
        out += std::format("L{}\n", id);
    }
    return out;
}

TEST(Myers, MinimalVsDp) {
    SplitMix64 rng(0xD1FF0003);
    const std::uint32_t cases = scaled(20000, 20000, 5000);
    for (std::uint32_t c = 0; c < cases; ++c) {
        const auto alphabet = static_cast<std::uint32_t>(2 + rng.below(5));
        const std::vector<std::uint32_t> a = randomIds(rng, alphabet, 40);
        const std::vector<std::uint32_t> b = randomIds(rng, alphabet, 40);
        const std::uint64_t distance = editDistance(a, b);

        std::vector<std::uint8_t> changedA(a.size(), 0);
        std::vector<std::uint8_t> changedB(b.size(), 0);
        detail::WorkBudget budget(std::numeric_limits<std::uint64_t>::max(), {});
        ASSERT_TRUE(detail::myersDiff(
            a, b, detail::Region{0, static_cast<std::uint32_t>(a.size()), 0, static_cast<std::uint32_t>(b.size())},
            changedA, changedB, budget));
        std::uint64_t changed = 0;
        for (const std::uint8_t flag : changedA) {
            changed += flag;
        }
        for (const std::uint8_t flag : changedB) {
            changed += flag;
        }
        ASSERT_EQ(changed, distance) << "case " << c << ": " << printable(idText(a)) << " vs " << printable(idText(b));
        ASSERT_TRUE(consistent(a, b, changedA, changedB)) << "case " << c;

        // Through the public API, with and without sliders (sliding keeps the counts).
        if (c % 4 == 0) {
            const LineSequence sa = makeSequence(idText(a), LineSplit::Lf);
            const LineSequence sb = makeSequence(idText(b), LineSplit::Lf);
            for (const Sliders sliders : {Sliders::GitClassic, Sliders::None}) {
                DiffOptions options;
                options.algorithm = Algorithm::Myers;
                options.sliders = sliders;
                auto diff = diffLines(sa, sb, options);
                ASSERT_TRUE(diff.has_value());
                ASSERT_EQ(diff->deletedLines() + diff->insertedLines(), distance) << "case " << c;
                ASSERT_TRUE(appliesCleanly(sa, sb, *diff, options)) << "case " << c;
            }
        }
    }
}

TEST(Myers, ExhaustedBudgetStillValid) {
    // A budget that runs out mid-way leaves whole replaces: valid, never better than minimal.
    SplitMix64 rng(0xD1FF0004);
    for (std::uint32_t c = 0; c < 2000; ++c) {
        const std::vector<std::uint32_t> a = randomIds(rng, 3, 60);
        const std::vector<std::uint32_t> b = randomIds(rng, 3, 60);
        std::vector<std::uint8_t> changedA(a.size(), 0);
        std::vector<std::uint8_t> changedB(b.size(), 0);
        detail::WorkBudget budget(rng.below(200), {});
        (void)detail::myersDiff(
            a, b, detail::Region{0, static_cast<std::uint32_t>(a.size()), 0, static_cast<std::uint32_t>(b.size())},
            changedA, changedB, budget);
        ASSERT_TRUE(consistent(a, b, changedA, changedB)) << "case " << c;
    }
}

} // namespace
} // namespace frost::diff::test
