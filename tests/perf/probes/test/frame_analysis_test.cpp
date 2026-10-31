// SPDX-License-Identifier: Apache-2.0
//
// Tests for the frame-pacing analysis (frost/perf/frame_analysis.hpp) on
// synthetic 144 Hz timelines: perfect pacing, missed refreshes, repeated
// frames, merged captures, frame matching and DWM counter deltas.

#include "frost/perf/frame_analysis.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using namespace frost::perf;

// 10 MHz ticks (100 ns), the usual QPC frequency on Windows 10/11.
constexpr std::int64_t kRate = 10'000'000;
constexpr std::int64_t kPeriod = kRate / 144; // 69444 ticks = 6.944 ms.

std::vector<ScreenUpdate> steadyTimeline(int frames, std::int64_t start = 1'000'000) {
    std::vector<ScreenUpdate> updates;
    for (int i = 0; i < frames; ++i) {
        updates.push_back(ScreenUpdate{start + i * kPeriod, static_cast<std::uint16_t>(i), 1});
    }
    return updates;
}

TEST(FrameAnalysis, TickRateConvertsBothWays) {
    const TickRate rate(kRate);
    EXPECT_DOUBLE_EQ(rate.toMs(kRate), 1000.0);
    EXPECT_EQ(rate.fromMs(16.7), 167'000);
}

TEST(FrameAnalysis, PerfectPacingMissesNothing) {
    const auto updates = steadyTimeline(1440);
    const auto stats = analyseContent(updates, 0, updates.back().presentTicks, kPeriod, TickRate(kRate));
    EXPECT_EQ(stats.updates, 1440u);
    EXPECT_EQ(stats.refreshes, 1439u);
    EXPECT_EQ(stats.missedRefreshes, 0u);
    EXPECT_DOUBLE_EQ(stats.missedFraction, 0.0);
    EXPECT_NEAR(stats.maxIntervalMs, 6.944, 0.001);
    EXPECT_EQ(stats.intervalsMs.count, 1439u);
}

TEST(FrameAnalysis, GapOfThreePeriodsMissesTwoRefreshes) {
    auto updates = steadyTimeline(100);
    // Frames 50.. arrive two periods late: one interval of 3 periods.
    for (std::size_t i = 50; i < updates.size(); ++i) {
        updates[i].presentTicks += 2 * kPeriod;
    }
    const auto stats = analyseContent(updates, 0, updates.back().presentTicks, kPeriod, TickRate(kRate));
    EXPECT_EQ(stats.missedRefreshes, 2u);
    EXPECT_EQ(stats.refreshes, 101u);
    EXPECT_NEAR(stats.maxIntervalMs, 3 * 6.944, 0.01);
    EXPECT_NEAR(stats.missedFraction, 2.0 / 101.0, 1e-9);
}

TEST(FrameAnalysis, HalfRateIsHalfMissed) {
    std::vector<ScreenUpdate> updates;
    for (int i = 0; i < 100; ++i) {
        updates.push_back(ScreenUpdate{i * 2 * kPeriod, static_cast<std::uint16_t>(i), 1});
    }
    const auto stats = analyseContent(updates, 0, updates.back().presentTicks, kPeriod, TickRate(kRate));
    EXPECT_EQ(stats.refreshes, 198u);
    EXPECT_EQ(stats.missedRefreshes, 99u);
    EXPECT_NEAR(stats.missedFraction, 0.5, 1e-9);
    EXPECT_NEAR(stats.maxIntervalMs, 13.889, 0.001); // Still under B6's 16.7 ms interval limit.
}

TEST(FrameAnalysis, RepeatedMarkerIsNotNewContent) {
    auto updates = steadyTimeline(10);
    // Another window changed during frame 4: the capture shows marker 4 twice.
    updates.insert(updates.begin() + 5, ScreenUpdate{updates[4].presentTicks + kPeriod / 2, std::uint16_t{4}, 1});
    const auto stats = analyseContent(updates, 0, updates.back().presentTicks, kPeriod, TickRate(kRate));
    EXPECT_EQ(stats.updates, 10u);
    EXPECT_EQ(stats.missedRefreshes, 0u);
}

TEST(FrameAnalysis, UpdatesOutsideTheSpanAreIgnoredAndMergesCounted) {
    auto updates = steadyTimeline(20);
    updates[10].accumulatedFrames = 2;
    const auto stats =
        analyseContent(updates, updates[5].presentTicks, updates[14].presentTicks, kPeriod, TickRate(kRate));
    EXPECT_EQ(stats.updates, 10u);
    EXPECT_EQ(stats.refreshes, 9u);
    EXPECT_EQ(stats.mergedCaptures, 1u);
}

TEST(FrameAnalysis, UnmarkedUpdatesAlwaysCount) {
    std::vector<ScreenUpdate> updates;
    for (int i = 0; i < 5; ++i) {
        updates.push_back(ScreenUpdate{i * kPeriod, std::nullopt, 1});
    }
    const auto stats = analyseContent(updates, 0, 4 * kPeriod, kPeriod, TickRate(kRate));
    EXPECT_EQ(stats.updates, 5u);
}

TEST(FrameAnalysis, MatchesFramesToTheirFirstScreenUpdate) {
    const auto updates = steadyTimeline(10);
    std::vector<AppFrame> frames;
    for (std::uint32_t seq = 0; seq < 10; ++seq) {
        // Each frame was ready 2 ms before its screen update.
        frames.push_back(AppFrame{seq, updates[seq].presentTicks - 20'000, updates[seq].presentTicks - 30'000});
    }
    frames.push_back(AppFrame{10, updates.back().presentTicks + kPeriod, 0}); // Never shown.
    const auto matches = matchFrames(frames, updates, 0);
    ASSERT_EQ(matches.size(), 11u);
    for (std::uint32_t seq = 0; seq < 10; ++seq) {
        ASSERT_TRUE(matches[seq].screenTicks.has_value());
        EXPECT_EQ(*matches[seq].screenTicks, updates[seq].presentTicks);
    }
    EXPECT_FALSE(matches[10].screenTicks.has_value());

    const auto latency = summariseLatency(frames, matches, TickRate(kRate));
    EXPECT_EQ(latency.shown, 10u);
    EXPECT_EQ(latency.neverShown, 1u);
    EXPECT_DOUBLE_EQ(latency.readyToScreenMs.p50, 2.0);
    EXPECT_DOUBLE_EQ(latency.inputToScreenMs.max, 3.0);
}

TEST(FrameAnalysis, ScreenUpdateBeforeReadyIsNotAMatch) {
    // Marker 7 is on screen from an earlier run of the same sequence; the frame is ready later.
    const std::vector<ScreenUpdate> updates{{100, std::uint16_t{7}, 1}, {500'000, std::uint16_t{7}, 1}};
    const std::vector<AppFrame> frames{{7, 400'000, 0}};
    const auto matches = matchFrames(frames, updates, 0);
    ASSERT_TRUE(matches[0].screenTicks.has_value());
    EXPECT_EQ(*matches[0].screenTicks, 500'000);
}

TEST(FrameAnalysis, NextVBlankRoundsUpOnTheGrid) {
    const std::int64_t anchor = 1'000'000;
    EXPECT_EQ(nextVBlank(anchor, anchor, kPeriod), anchor);
    EXPECT_EQ(nextVBlank(anchor + 1, anchor, kPeriod), anchor + kPeriod);
    EXPECT_EQ(nextVBlank(anchor + 3 * kPeriod - 5, anchor, kPeriod), anchor + 3 * kPeriod);
    // An anchor in the future (DWM reports the upcoming vblank) works as well.
    EXPECT_EQ(nextVBlank(anchor - kPeriod - 7, anchor, kPeriod), anchor - kPeriod);
    EXPECT_EQ(nextVBlank(anchor - kPeriod, anchor, kPeriod), anchor - kPeriod);
}

TEST(FrameAnalysis, PresentJitterDisappearsOnTheVBlankGrid) {
    // Presents land anywhere in the period; their display vblanks are one period apart.
    std::vector<ScreenUpdate> updates;
    const std::int64_t jitter[] = {500, 60'000, 3'000, 45'000, 10'000, 68'000};
    for (int i = 0; i < 6; ++i) {
        ScreenUpdate update;
        update.presentTicks = i * kPeriod + jitter[i];
        update.displayTicks = nextVBlank(update.presentTicks, 0, kPeriod);
        update.marker = static_cast<std::uint16_t>(i);
        updates.push_back(update);
    }
    const auto stats = analyseContent(updates, 0, 10 * kPeriod, kPeriod, TickRate(kRate));
    EXPECT_EQ(stats.missedRefreshes, 0u);
    EXPECT_NEAR(stats.intervalsMs.min, 6.944, 0.001);
    EXPECT_NEAR(stats.intervalsMs.max, 6.944, 0.001);
}

TEST(FrameAnalysis, CollapseKeepsTheLastPresentBeforeEachVBlank) {
    std::vector<ScreenUpdate> updates{
        {100, std::uint16_t{1}, 1, kPeriod},
        {kPeriod + 100, std::uint16_t{2}, 1, 2 * kPeriod},
        {kPeriod + 40'000, std::uint16_t{3}, 1, 2 * kPeriod + 3}, // Supersedes marker 2 (grid jitter).
        {2 * kPeriod + 100, std::uint16_t{4}, 1, 3 * kPeriod},
        {2 * kPeriod + 200, std::nullopt, 1, 0}, // No grid: kept.
    };
    const auto displayed = collapseToDisplay(updates, kPeriod);
    ASSERT_EQ(displayed.size(), 4u);
    EXPECT_EQ(displayed[1].marker, std::uint16_t{3});
    EXPECT_EQ(displayed[2].marker, std::uint16_t{4});
    EXPECT_EQ(displayed[3].screenTicks(), 2 * kPeriod + 200);
}

TEST(FrameAnalysis, DwmDeltaComputesFractions) {
    DwmCounters before;
    before.refreshCount = 1000;
    before.compositionCount = 900;
    before.framesMissed = 5;
    DwmCounters after = before;
    after.refreshCount = 2440;
    after.compositionCount = 2330;
    after.framesMissed = 19;
    const auto delta = dwmDelta(before, after);
    EXPECT_EQ(delta.refreshes, 1440u);
    EXPECT_EQ(delta.compositions, 1430u);
    EXPECT_EQ(delta.missed, 14u);
    EXPECT_NEAR(delta.missedFraction, 14.0 / 1440.0, 1e-12);
}

TEST(FrameAnalysis, DwmDeltaTreatsResetCountersAsUnchanged) {
    DwmCounters before;
    before.refreshCount = 10;
    before.framesDropped = 7;
    DwmCounters after;
    after.refreshCount = 20;
    after.framesDropped = 0;
    const auto delta = dwmDelta(before, after);
    EXPECT_EQ(delta.dropped, 0u);
    EXPECT_EQ(delta.refreshes, 10u);
}

} // namespace
