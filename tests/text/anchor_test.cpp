// SPDX-License-Identifier: Apache-2.0
//
// TextAnchor: decision 1's mapping rule for one edit and for batches (ADR 0017), against tables
// and against sequential application by the oracle; also the private BatchMapper of the sweeps.

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/anchor.hpp"

#include "batch_mapper.hpp"
#include "text/support/undo_oracle.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

using frost::ByteOffset;
using frost::sched::detail::SplitMix64;
using frost::text::Bias;
using frost::text::ByteRange;
using frost::text::mapOffset;
using frost::text::RawEdit;
using frost::text::detail::BatchMapper;
using frost::text::test::oracleMapBatch;

RawEdit edit(std::uint64_t a, std::uint64_t b, std::uint64_t n) {
    return RawEdit{ByteRange{ByteOffset{a}, ByteOffset{b}}, n};
}

std::uint64_t map(std::uint64_t x, Bias bias, const RawEdit& e) { return mapOffset(ByteOffset{x}, bias, e).value(); }
std::uint64_t map(std::uint64_t x, Bias bias, const std::vector<RawEdit>& batch) {
    return mapOffset(ByteOffset{x}, bias, batch).value();
}

TEST(TextAnchor, MapOffsetTable) {
    struct Case {
        std::uint64_t a, b, n, x, left, right;
    };
    const Case cases[] = {
        // Replace [3, 6) by 2 bytes: before, at the start, inside, at the end, after.
        {3, 6, 2, 0, 0, 0},
        {3, 6, 2, 2, 2, 2},
        {3, 6, 2, 3, 3, 5},
        {3, 6, 2, 4, 3, 5},
        {3, 6, 2, 6, 3, 5},
        {3, 6, 2, 7, 6, 6},
        {3, 6, 2, 10, 9, 9},
        // Insert 4 bytes at 5.
        {5, 5, 4, 4, 4, 4},
        {5, 5, 4, 5, 5, 9},
        {5, 5, 4, 6, 10, 10},
        // Delete [2, 8).
        {2, 8, 0, 1, 1, 1},
        {2, 8, 0, 2, 2, 2},
        {2, 8, 0, 5, 2, 2},
        {2, 8, 0, 8, 2, 2},
        {2, 8, 0, 9, 3, 3},
        // A no-op maps everything to itself.
        {4, 4, 0, 3, 3, 3},
        {4, 4, 0, 4, 4, 4},
        {4, 4, 0, 5, 5, 5},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(map(c.x, Bias::Left, edit(c.a, c.b, c.n)), c.left) << c.a << " " << c.b << " " << c.n << " " << c.x;
        EXPECT_EQ(map(c.x, Bias::Right, edit(c.a, c.b, c.n)), c.right) << c.a << " " << c.b << " " << c.n << " " << c.x;
        // The one-edit batch agrees.
        EXPECT_EQ(map(c.x, Bias::Left, std::vector{edit(c.a, c.b, c.n)}), c.left);
        EXPECT_EQ(map(c.x, Bias::Right, std::vector{edit(c.a, c.b, c.n)}), c.right);
    }
    static_assert(mapOffset(ByteOffset{4}, Bias::Right, RawEdit{ByteRange{ByteOffset{4}, ByteOffset{4}}, 3}) ==
                  ByteOffset{7});
}

// Random sorted batches over [0, 40] with adjacent ranges, no-ops and repeated inserts at one
// offset: the closed form and BatchMapper equal sequential application.
TEST(TextAnchor, BatchClosedFormMatchesSequential) {
    SplitMix64 rng(0xA11C);
    for (int round = 0; round < 10'000; ++round) {
        std::vector<RawEdit> batch;
        std::uint64_t at = rng.below(4);
        const std::uint64_t count = 1 + rng.below(6);
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint64_t kind = rng.below(5);
            std::uint64_t a = at;
            if (kind == 0 || batch.empty()) {
                a = at + rng.below(4); // A gap (or none).
            }
            std::uint64_t b = a;
            std::uint64_t n = rng.below(4);
            if (kind == 1) {
                n = 0; // No-op or deletion.
            }
            if (kind != 2 && rng.below(3) != 0) {
                b = a + rng.below(5); // A range; kind 2 is an insert at the same offset.
            }
            batch.push_back(edit(a, b, n));
            at = b;
        }
        const std::uint64_t size = at + 4;
        for (const Bias bias : {Bias::Left, Bias::Right}) {
            BatchMapper mapper(batch);
            for (std::uint64_t x = 0; x <= size; ++x) {
                const std::uint64_t expected = oracleMapBatch(x, bias, batch);
                ASSERT_EQ(map(x, bias, batch), expected) << "round " << round << " x " << x;
                ASSERT_EQ(mapper.map(x, bias), expected) << "mapper, round " << round << " x " << x;
                ASSERT_EQ(mapper.mapRandom(size - x, bias), oracleMapBatch(size - x, bias, batch))
                    << "random access mapper, round " << round << " x " << (size - x);
            }
        }
        std::int64_t delta = 0;
        for (const RawEdit& e : batch) {
            delta += static_cast<std::int64_t>(e.newBytes) - static_cast<std::int64_t>(e.range.size());
        }
        ASSERT_EQ(BatchMapper(batch).totalDelta(), delta);
    }
}

TEST(TextAnchor, AdjacentAndSameOffsetEdits) {
    // [2, 4) -> 2 bytes then [4, 6) -> 1 byte: a Left anchor inside the second range collapses
    // through both (sequentially it lands on 4, then on 2), so the closed form starts the touching
    // run at the first edit (ADR 0017 records this refinement of the spec's a_F + D).
    const std::vector batch{edit(2, 4, 2), edit(4, 6, 1)};
    EXPECT_EQ(map(5, Bias::Left, batch), 2u);
    EXPECT_EQ(map(5, Bias::Right, batch), 5u);
    EXPECT_EQ(map(4, Bias::Left, batch), 2u);
    EXPECT_EQ(map(4, Bias::Right, batch), 5u);
    EXPECT_EQ(map(6, Bias::Right, batch), 5u);
    EXPECT_EQ(map(7, Bias::Left, batch), 6u);
    // Two inserts at one offset keep their order: a Left anchor stays before both, a Right one
    // goes after both.
    const std::vector inserts{edit(5, 5, 2), edit(5, 5, 3)};
    EXPECT_EQ(map(5, Bias::Left, inserts), 5u);
    EXPECT_EQ(map(5, Bias::Right, inserts), 10u);
    EXPECT_EQ(map(4, Bias::Right, inserts), 4u);
    EXPECT_EQ(map(6, Bias::Left, inserts), 11u);
    // An insert at the start of a following range.
    const std::vector insertThenRange{edit(3, 3, 1), edit(3, 6, 0)};
    EXPECT_EQ(map(3, Bias::Left, insertThenRange), 3u);
    EXPECT_EQ(map(3, Bias::Right, insertThenRange), 4u);
    EXPECT_EQ(map(5, Bias::Left, insertThenRange), 3u);
    EXPECT_EQ(map(6, Bias::Right, insertThenRange), 4u);
    // No-ops between touching edits change nothing.
    const std::vector withNoOp{edit(0, 2, 0), edit(2, 2, 0), edit(2, 4, 0)};
    EXPECT_EQ(map(3, Bias::Left, withNoOp), 0u);
    EXPECT_EQ(map(3, Bias::Right, withNoOp), 0u);
    EXPECT_EQ(map(5, Bias::Left, withNoOp), 1u);
    for (const auto* b : {&batch, &inserts, &insertThenRange, &withNoOp}) {
        for (std::uint64_t x = 0; x <= 12; ++x) {
            EXPECT_EQ(map(x, Bias::Left, *b), oracleMapBatch(x, Bias::Left, *b));
            EXPECT_EQ(map(x, Bias::Right, *b), oracleMapBatch(x, Bias::Right, *b));
        }
    }
}

} // namespace
