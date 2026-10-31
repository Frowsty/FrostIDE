// SPDX-License-Identifier: Apache-2.0
//
// Tests for LaneSelector: strict priority without contention, aging after the
// documented number of passes, the starvation bound for every combination of
// saturated lanes and under random eligibility, and lane names.

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/sched/lane_policy.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace {

using frost::sched::kAllLanes;
using frost::sched::kLaneCount;
using frost::sched::Lane;
using frost::sched::laneBit;
using frost::sched::laneIndex;
using frost::sched::LaneMask;
using frost::sched::LaneSelector;

constexpr LaneMask kAll = 0x0F;

TEST(LaneSelector, PrefersHighestPriorityLane) {
    const LaneSelector selector;
    EXPECT_EQ(selector.choose(kAll), Lane::Interactive);
    EXPECT_EQ(selector.choose(laneBit(Lane::Idle) | laneBit(Lane::UserVisible)), Lane::UserVisible);
    EXPECT_EQ(selector.choose(laneBit(Lane::Idle)), Lane::Idle);
    const LaneSelector::Order order = selector.order(laneBit(Lane::Background) | laneBit(Lane::Interactive));
    ASSERT_EQ(order.count, 2U);
    EXPECT_EQ(order.lanes[0], Lane::Interactive);
    EXPECT_EQ(order.lanes[1], Lane::Background);
    EXPECT_EQ(selector.order(0).count, 0U);
}

TEST(LaneSelector, AgesLowerLaneAfterThresholdPasses) {
    LaneSelector selector;
    const LaneMask mask = laneBit(Lane::Interactive) | laneBit(Lane::Background);
    const std::uint32_t threshold = LaneSelector::kAgingPasses[laneIndex(Lane::Background)];
    for (std::uint32_t i = 0; i < threshold; ++i) {
        ASSERT_EQ(selector.choose(mask), Lane::Interactive) << "dispatch " << i;
        selector.dispatched(Lane::Interactive, mask);
    }
    EXPECT_TRUE(selector.aged(Lane::Background));
    EXPECT_EQ(selector.choose(mask), Lane::Background);
    const LaneSelector::Order order = selector.order(mask);
    EXPECT_EQ(order.lanes[0], Lane::Background);
    EXPECT_EQ(order.lanes[1], Lane::Interactive);
    selector.dispatched(Lane::Background, mask);
    EXPECT_EQ(selector.passes(Lane::Background), 0U);
    EXPECT_EQ(selector.choose(mask), Lane::Interactive);
}

TEST(LaneSelector, OnlyEligibleLanesArePassedOver) {
    LaneSelector selector;
    // Idle had no work during these dispatches, so it is not passed over.
    for (int i = 0; i < 100; ++i) {
        selector.dispatched(Lane::Interactive, laneBit(Lane::Interactive));
    }
    EXPECT_EQ(selector.passes(Lane::Idle), 0U);
    EXPECT_EQ(selector.passes(Lane::UserVisible), 0U);
    // A dispatch never passes over higher lanes.
    selector.dispatched(Lane::Idle, kAll);
    EXPECT_EQ(selector.passes(Lane::Interactive), 0U);
    EXPECT_EQ(selector.passes(Lane::UserVisible), 0U);
}

// Every subset of saturated lanes, run for many dispatches: no saturated lane is
// passed over more than LaneSelector::maxWait() consecutive times.
TEST(LaneSelector, StarvationBoundHoldsForEverySaturatedSubset) {
    for (LaneMask mask = 1; mask <= kAll; ++mask) {
        LaneSelector selector;
        std::array<std::uint32_t, kLaneCount> waited{};
        std::array<std::uint32_t, kLaneCount> worst{};
        for (int dispatch = 0; dispatch < 10'000; ++dispatch) {
            const Lane chosen = selector.choose(mask);
            ASSERT_NE(mask & laneBit(chosen), 0) << "chose a lane without work";
            selector.dispatched(chosen, mask);
            for (const Lane lane : kAllLanes) {
                if ((mask & laneBit(lane)) == 0) {
                    continue;
                }
                std::uint32_t& wait = waited[laneIndex(lane)];
                wait = lane == chosen ? 0 : wait + 1;
                worst[laneIndex(lane)] = std::max(worst[laneIndex(lane)], wait);
            }
        }
        for (const Lane lane : kAllLanes) {
            if ((mask & laneBit(lane)) != 0) {
                EXPECT_LE(worst[laneIndex(lane)], LaneSelector::maxWait(lane))
                    << "lane " << frost::sched::toString(lane) << " mask " << int{mask};
            }
        }
    }
}

// The lane under test always has work; the others come and go at random rates.
TEST(LaneSelector, StarvationBoundHoldsUnderRandomEligibility) {
    frost::sched::detail::SplitMix64 rng(2026);
    for (const Lane target : kAllLanes) {
        for (int trial = 0; trial < 300; ++trial) {
            LaneSelector selector;
            const std::uint64_t density = 1 + rng.below(9); // Each other lane has work with probability density/10.
            std::uint32_t wait = 0;
            std::uint32_t worst = 0;
            for (int dispatch = 0; dispatch < 500; ++dispatch) {
                LaneMask mask = laneBit(target);
                for (const Lane lane : kAllLanes) {
                    if (rng.below(10) < density) {
                        mask = static_cast<LaneMask>(mask | laneBit(lane));
                    }
                }
                const Lane chosen = selector.choose(mask);
                selector.dispatched(chosen, mask);
                wait = chosen == target ? 0 : wait + 1;
                worst = std::max(worst, wait);
            }
            ASSERT_LE(worst, LaneSelector::maxWait(target)) << frost::sched::toString(target) << " trial " << trial;
        }
    }
}

TEST(LaneSelector, InteractiveKeepsMostTurnsWhenEverythingIsSaturated) {
    LaneSelector selector;
    std::array<int, kLaneCount> served{};
    constexpr int kDispatches = 100'000;
    for (int i = 0; i < kDispatches; ++i) {
        const Lane lane = selector.choose(kAll);
        selector.dispatched(lane, kAll);
        ++served[laneIndex(lane)];
    }
    // UserVisible ages every 4 dispatches, so Interactive keeps at least ~3/5 even with
    // Background and Idle aging too; every lane gets some share.
    EXPECT_GT(served[0], kDispatches * 6 / 10);
    for (const int count : served) {
        EXPECT_GT(count, 0);
    }
}

TEST(Lane, NamesAndHelpers) {
    EXPECT_EQ(frost::sched::toString(Lane::Interactive), "Interactive");
    EXPECT_EQ(frost::sched::toString(Lane::UserVisible), "UserVisible");
    EXPECT_EQ(frost::sched::toString(Lane::Background), "Background");
    EXPECT_EQ(frost::sched::toString(Lane::Idle), "Idle");
    EXPECT_FALSE(frost::sched::isLowLane(Lane::UserVisible));
    EXPECT_TRUE(frost::sched::isLowLane(Lane::Background));
    EXPECT_TRUE(frost::sched::isLowLane(Lane::Idle));
}

} // namespace
