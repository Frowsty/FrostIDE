// SPDX-License-Identifier: Apache-2.0
//
// SelectionSet (ADR 0017, decision 4): the merge rules, the primary selection, directions,
// origins, the invariants, transforms through raw batches, and comparisons with the oracle's
// brute-force normalisation and sequential mapping.

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/selection.hpp"

#include "text/support/undo_ops.hpp"
#include "text/support/undo_oracle.hpp"
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using frost::ByteOffset;
using frost::sched::detail::SplitMix64;
using frost::text::ByteRange;
using frost::text::RawEdit;
using frost::text::Selection;
using frost::text::SelectionSet;
using frost::text::test::oracleNormalise;
using frost::text::test::OracleSelection;
using frost::text::test::OracleSelections;
using frost::text::test::oracleTransform;
using frost::text::test::toOracle;

Selection sel(std::uint64_t anchor, std::uint64_t head) { return Selection{ByteOffset{anchor}, ByteOffset{head}}; }
RawEdit edit(std::uint64_t a, std::uint64_t b, std::uint64_t n) {
    return RawEdit{ByteRange{ByteOffset{a}, ByteOffset{b}}, n};
}

std::vector<Selection> items(const SelectionSet& set) { return {set.all().begin(), set.all().end()}; }

TEST(SelectionSet, MergeRules) {
    struct Case {
        const char* name;
        std::vector<Selection> input;
        std::vector<Selection> expected;
    };
    const Case cases[] = {
        {"overlapping selections merge", {sel(0, 5), sel(3, 8)}, {sel(0, 8)}},
        {"touching selections stay separate", {sel(0, 3), sel(3, 6)}, {sel(0, 3), sel(3, 6)}},
        {"a caret inside a selection is absorbed", {sel(2, 8), sel(5, 5)}, {sel(2, 8)}},
        {"a caret at a selection's end is absorbed", {sel(2, 5), sel(5, 5)}, {sel(2, 5)}},
        {"a caret at a selection's start is absorbed", {sel(5, 5), sel(5, 9)}, {sel(5, 9)}},
        {"identical carets merge", {sel(4, 4), sel(4, 4), sel(4, 4)}, {sel(4, 4)}},
        {"separate carets stay", {sel(1, 1), sel(2, 2)}, {sel(1, 1), sel(2, 2)}},
        {"unsorted input is sorted", {sel(9, 9), sel(1, 3), sel(5, 5)}, {sel(1, 3), sel(5, 5), sel(9, 9)}},
        {"a chain merges into one", {sel(0, 2), sel(1, 4), sel(3, 6), sel(6, 6)}, {sel(0, 6)}},
        {"contained selection", {sel(0, 10), sel(2, 3)}, {sel(0, 10)}},
    };
    for (const Case& c : cases) {
        const SelectionSet set = SelectionSet::fromSelections(c.input, 0);
        EXPECT_EQ(items(set), c.expected) << c.name;
        EXPECT_TRUE(set.checkInvariants().has_value()) << c.name;
        std::vector<OracleSelection> raw;
        for (const Selection& s : c.input) {
            raw.push_back(OracleSelection{s.anchor.value(), s.head.value()});
        }
        EXPECT_EQ(toOracle(set), oracleNormalise(raw, 0)) << c.name;
    }
    EXPECT_TRUE(SelectionSet().empty());
    EXPECT_EQ(SelectionSet().maxOffset(), ByteOffset{0});
    EXPECT_EQ(SelectionSet::fromSelections(std::vector{sel(3, 7), sel(1, 1)}, 0).maxOffset(), ByteOffset{7});
}

TEST(SelectionSet, PrimaryTracking) {
    // The primary follows its selection through sorting and merging.
    const SelectionSet sorted = SelectionSet::fromSelections(std::vector{sel(9, 9), sel(1, 1), sel(5, 5)}, 2);
    EXPECT_EQ(sorted.primaryIndex(), 1u);
    const SelectionSet merged = SelectionSet::fromSelections(std::vector{sel(1, 1), sel(4, 8), sel(6, 6)}, 2);
    EXPECT_EQ(merged.size(), 2u);
    EXPECT_EQ(merged.primaryIndex(), 1u); // [4, 8) absorbed the primary caret.
    // Through a transform.
    const SelectionSet moved = sorted.transformed(std::vector{edit(0, 6, 0)});
    EXPECT_EQ(items(moved), (std::vector{sel(0, 0), sel(3, 3)}));
    EXPECT_EQ(moved.primaryIndex(), 0u); // 1 and 5 merged at 0, with the primary 5.
    EXPECT_EQ(SelectionSet::caret(ByteOffset{4}).primaryIndex(), 0u);
}

TEST(SelectionSet, DirectionOfMerged) {
    // The first non-empty member in sweep order gives the direction.
    const SelectionSet reversed = SelectionSet::fromSelections(std::vector{sel(5, 0), sel(3, 8)}, 0);
    EXPECT_EQ(items(reversed), (std::vector{sel(8, 0)}));
    const SelectionSet forward = SelectionSet::fromSelections(std::vector{sel(3, 8), sel(5, 0)}, 0);
    EXPECT_EQ(items(forward), (std::vector{sel(8, 0)})); // (0, 5) sorts first either way.
    const SelectionSet caretFirst = SelectionSet::fromSelections(std::vector{sel(2, 2), sel(6, 2)}, 0);
    EXPECT_EQ(items(caretFirst), (std::vector{sel(6, 2)})); // The caret has no direction.
    EXPECT_TRUE(items(caretFirst)[0].reversed());
}

TEST(SelectionSet, Origins) {
    std::vector<std::uint32_t> origins;
    const SelectionSet set =
        SelectionSet::fromSelections(std::vector{sel(9, 9), sel(4, 6), sel(1, 1), sel(5, 5), sel(0, 1)}, 0, &origins);
    EXPECT_EQ(items(set), (std::vector{sel(0, 1), sel(4, 6), sel(9, 9)}));
    EXPECT_EQ(origins, (std::vector<std::uint32_t>{4, 1, 0})); // The first member in sweep order.
    const SelectionSet moved = set.transformed(std::vector{edit(0, 9, 0)}, &origins);
    EXPECT_EQ(items(moved), (std::vector{sel(0, 0)}));
    EXPECT_EQ(origins, (std::vector<std::uint32_t>{0}));
    (void)SelectionSet().transformed(std::vector{edit(0, 0, 1)}, &origins);
    EXPECT_TRUE(origins.empty());
}

TEST(SelectionSet, InvariantsL1L3) {
    SplitMix64 rng(42);
    for (int round = 0; round < 1000; ++round) {
        std::vector<Selection> input;
        for (std::uint64_t i = 0, n = 1 + rng.below(10); i < n; ++i) {
            input.push_back(sel(rng.below(30), rng.below(3) == 0 ? rng.below(30) : 0));
        }
        const SelectionSet set = SelectionSet::fromSelections(input, rng.below(input.size()));
        ASSERT_TRUE(set.checkInvariants().has_value()) << set.checkInvariants().error().describe();
        ASSERT_LT(set.primaryIndex(), set.size());
    }
    EXPECT_TRUE(SelectionSet().checkInvariants().has_value());
    EXPECT_EQ(SelectionSet().primaryIndex(), 0u);
}

TEST(SelectionSet, TransformBiases) {
    // A caret is pushed by an insert at it.
    EXPECT_EQ(items(SelectionSet::caret(ByteOffset{3}).transformed(std::vector{edit(3, 3, 2)})),
              (std::vector{sel(5, 5)}));
    // A selection grows when text is inserted at either edge, in both directions.
    EXPECT_EQ(items(SelectionSet::fromSelections(std::vector{sel(3, 6)}, 0).transformed(std::vector{edit(3, 3, 2)})),
              (std::vector{sel(3, 8)}));
    EXPECT_EQ(items(SelectionSet::fromSelections(std::vector{sel(3, 6)}, 0).transformed(std::vector{edit(6, 6, 2)})),
              (std::vector{sel(3, 8)}));
    EXPECT_EQ(items(SelectionSet::fromSelections(std::vector{sel(6, 3)}, 0).transformed(std::vector{edit(3, 3, 1)})),
              (std::vector{sel(7, 3)}));
    // A fully deleted selection becomes a caret at the deletion point.
    EXPECT_EQ(items(SelectionSet::fromSelections(std::vector{sel(3, 6)}, 0).transformed(std::vector{edit(2, 7, 0)})),
              (std::vector{sel(2, 2)}));
    // Replacing exactly the selection: it covers the replacement.
    EXPECT_EQ(items(SelectionSet::fromSelections(std::vector{sel(3, 6)}, 0).transformed(std::vector{edit(3, 6, 4)})),
              (std::vector{sel(3, 7)}));
}

TEST(SelectionSet, TransformCausesMerge) {
    // Two carets around a deleted range end at one offset and merge.
    const SelectionSet set = SelectionSet::fromSelections(std::vector{sel(2, 2), sel(5, 5)}, 1);
    const SelectionSet moved = set.transformed(std::vector{edit(2, 5, 0)});
    EXPECT_EQ(items(moved), (std::vector{sel(2, 2)}));
    EXPECT_EQ(moved.primaryIndex(), 0u);
    // A selection deleted down to its start meets the caret after it.
    const SelectionSet touching = SelectionSet::fromSelections(std::vector{sel(1, 4), sel(6, 6)}, 0);
    EXPECT_EQ(items(touching.transformed(std::vector{edit(4, 6, 0)})), (std::vector{sel(1, 4)}));
}

TEST(SelectionSet, TransformReorders) {
    // A caret inside a replaced range goes to its end, after a selection whose start lay later
    // inside the same range: the order changes and the sweep must sort again.
    const SelectionSet set = SelectionSet::fromSelections(std::vector{sel(3, 3), sel(5, 9)}, 1);
    ASSERT_EQ(set.size(), 2u);
    const SelectionSet moved = set.transformed(std::vector{edit(2, 6, 3)});
    EXPECT_EQ(items(moved), (std::vector{sel(2, 8)}));
    EXPECT_EQ(moved.primaryIndex(), 0u);
    EXPECT_TRUE(moved.checkInvariants().has_value());
    EXPECT_EQ(toOracle(moved), oracleTransform(toOracle(set), std::vector{edit(2, 6, 3)}));
}

TEST(SelectionSet, NormaliseMatchesBruteForce) {
    SplitMix64 rng(0xB00F);
    for (int round = 0; round < 10'000; ++round) {
        const std::uint64_t span = 1 + rng.below(40);
        std::vector<Selection> input;
        std::vector<OracleSelection> oracleInput;
        for (std::uint64_t i = 0, n = 1 + rng.below(12); i < n; ++i) {
            const std::uint64_t anchor = rng.below(span + 1);
            const std::uint64_t head = rng.below(2) == 0 ? anchor : rng.below(span + 1);
            input.push_back(sel(anchor, head));
            oracleInput.push_back(OracleSelection{anchor, head});
        }
        const std::size_t primary = rng.below(input.size());
        std::vector<std::uint32_t> origins;
        std::vector<std::uint32_t> oracleOrigins;
        const SelectionSet set = SelectionSet::fromSelections(input, primary, &origins);
        const OracleSelections expected = oracleNormalise(oracleInput, primary, &oracleOrigins);
        ASSERT_EQ(toOracle(set), expected) << "round " << round;
        ASSERT_EQ(origins, oracleOrigins) << "round " << round;
    }
}

TEST(SelectionSet, TransformMatchesSequentialOracle) {
    SplitMix64 rng(0x7A45);
    for (int round = 0; round < 10'000; ++round) {
        const std::uint64_t size = 1 + rng.below(40);
        std::vector<Selection> input;
        for (std::uint64_t i = 0, n = 1 + rng.below(8); i < n; ++i) {
            const std::uint64_t anchor = rng.below(size + 1);
            input.push_back(sel(anchor, rng.below(2) == 0 ? anchor : rng.below(size + 1)));
        }
        const SelectionSet set = SelectionSet::fromSelections(input, rng.below(input.size()));
        std::vector<RawEdit> batch;
        std::uint64_t cursor = 0;
        for (std::uint64_t i = 0, n = 1 + rng.below(4); i < n; ++i) {
            const std::uint64_t a = std::min(size, cursor + rng.below(6));
            const std::uint64_t b = std::min(size, a + rng.below(5));
            batch.push_back(edit(a, b, rng.below(4)));
            cursor = b;
        }
        std::vector<std::uint32_t> origins;
        std::vector<std::uint32_t> oracleOrigins;
        const SelectionSet moved = set.transformed(batch, &origins);
        const OracleSelections expected = oracleTransform(toOracle(set), batch, &oracleOrigins);
        ASSERT_EQ(toOracle(moved), expected) << "round " << round;
        ASSERT_EQ(origins, oracleOrigins) << "round " << round;
        ASSERT_TRUE(moved.checkInvariants().has_value());
    }
}

} // namespace
