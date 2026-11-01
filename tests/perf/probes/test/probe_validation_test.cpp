// SPDX-License-Identifier: Apache-2.0
//
// Tests for the probe-validation scoring (frost/perf/probe_validation.hpp) on a
// simulated vsync display: an ideal probe passes at 0, 8 and 25 ms injected
// delay, and a biased, acausal, lossy or baseline-less probe fails.

#include "frost/perf/probe_validation.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using namespace frost::perf;

constexpr std::int64_t kRate = 10'000'000;
constexpr std::int64_t kPeriod = kRate / 144;
constexpr double kPeriodMs = 1000.0 / 144.0;

// The first vsync strictly after `ticks`.
std::int64_t nextVsync(std::int64_t ticks) { return (ticks / kPeriod + 1) * kPeriod; }

// Simulates trials on a display that shows a paint at the vsync after it was
// flushed plus one period of composition latency. Phases are low-discrepancy
// (golden-ratio) over one period, as the validation window randomises them.
std::vector<DelayTrial> simulate(const std::vector<double>& delaysMs, int trialsPerDelay, double extraMs = 0.0) {
    const TickRate rate(kRate);
    std::vector<DelayTrial> trials;
    double phase = 0.0;
    std::int64_t base = 100 * kPeriod;
    for (int i = 0; i < trialsPerDelay; ++i) {
        for (const double delay : delaysMs) {
            phase = std::fmod(phase + 0.6180339887498949, 1.0);
            DelayTrial trial;
            trial.injectedMs = delay;
            trial.triggerTicks = base + static_cast<std::int64_t>(phase * static_cast<double>(kPeriod));
            trial.drawnTicks = trial.triggerTicks + rate.fromMs(delay + 0.2);
            const double extra = delay > 0.0 ? extraMs : 0.0;
            trial.screenTicks = nextVsync(trial.drawnTicks) + kPeriod + rate.fromMs(extra);
            trials.push_back(trial);
            base += 8 * kPeriod;
        }
    }
    return trials;
}

TEST(ProbeValidation, IdealProbePassesAtEveryDelay) {
    const auto trials = simulate({0.0, 8.0, 25.0}, 40);
    const auto report = evaluateDelayTrials(trials, kPeriodMs, TickRate(kRate));
    EXPECT_TRUE(report.passed) << (report.problems.empty() ? "" : report.problems.front());
    ASSERT_EQ(report.delays.size(), 3u);
    EXPECT_EQ(report.delays[0].injectedMs, 0.0);
    EXPECT_EQ(report.delays[2].injectedMs, 25.0);
    for (const auto& delay : report.delays) {
        EXPECT_EQ(delay.trials, 40u);
        EXPECT_EQ(delay.lost, 0u);
        EXPECT_LT(std::abs(delay.shiftErrorMs), 0.5) << delay.injectedMs;
        // Residual (flush to screen) lies within one to two refresh periods.
        EXPECT_GE(delay.residualMs.min, kPeriodMs - 0.01);
        EXPECT_LE(delay.residualMs.max, 2.0 * kPeriodMs + 0.01);
    }
}

TEST(ProbeValidation, BiasedProbeFailsAccuracy) {
    // A probe that reports delayed frames 3 ms late shifts the mean by D + 3.
    const auto trials = simulate({0.0, 8.0}, 40, 3.0);
    const auto report = evaluateDelayTrials(trials, kPeriodMs, TickRate(kRate));
    EXPECT_FALSE(report.passed);
    ASSERT_EQ(report.delays.size(), 2u);
    EXPECT_NEAR(report.delays[1].shiftErrorMs, 3.0, 0.5);
}

TEST(ProbeValidation, FrameSeenBeforeItsFlushFailsCausality) {
    auto trials = simulate({0.0, 8.0}, 10);
    trials[3].screenTicks = trials[3].drawnTicks - TickRate(kRate).fromMs(2.0);
    const auto report = evaluateDelayTrials(trials, kPeriodMs, TickRate(kRate));
    EXPECT_FALSE(report.passed);
}

TEST(ProbeValidation, LateOrLostFramesFail) {
    auto late = simulate({0.0, 25.0}, 10);
    late[5].screenTicks = *late[5].screenTicks + 4 * kPeriod;
    EXPECT_FALSE(evaluateDelayTrials(late, kPeriodMs, TickRate(kRate)).passed);

    auto lost = simulate({0.0, 25.0}, 10);
    lost[2].screenTicks.reset();
    const auto report = evaluateDelayTrials(lost, kPeriodMs, TickRate(kRate));
    EXPECT_FALSE(report.passed);
    EXPECT_EQ(report.delays[0].lost + report.delays[1].lost, 1u);
}

TEST(ProbeValidation, MissingBaselineCannotBeScored) {
    const auto trials = simulate({8.0, 25.0}, 10);
    const auto report = evaluateDelayTrials(trials, kPeriodMs, TickRate(kRate));
    EXPECT_FALSE(report.passed);
    EXPECT_TRUE(report.delays.empty());
}

std::vector<FrameMatch> steadyMatches(std::uint32_t count, std::uint32_t stalledSeq, std::int64_t stallTicks) {
    std::vector<FrameMatch> matches;
    std::int64_t screen = kPeriod * 10;
    for (std::uint32_t seq = 100; seq < 100 + count; ++seq) {
        if (seq == stalledSeq) {
            screen = nextVsync(screen + stallTicks);
        }
        matches.push_back(FrameMatch{seq, screen});
        screen += kPeriod;
    }
    return matches;
}

TEST(ProbeValidation, StallsLengthenTheirIntervalAsExpected) {
    const TickRate rate(kRate);
    for (const double stallMs : {8.0, 25.0}) {
        const auto matches = steadyMatches(50, 120, rate.fromMs(stallMs));
        std::vector<StallProbe> stalls{{120, stallMs, std::nullopt}};
        measureStalls(stalls, matches, kPeriod, rate);
        ASSERT_TRUE(stalls[0].measuredIntervalMs.has_value());
        std::vector<std::string> problems;
        checkStalls(stalls, kPeriodMs, 0.5, problems);
        EXPECT_TRUE(problems.empty()) << stallMs << ": " << (problems.empty() ? "" : problems.front());
    }
}

TEST(ProbeValidation, StallIntervalBridgesADroppedPredecessor) {
    const TickRate rate(kRate);
    auto matches = steadyMatches(50, 120, rate.fromMs(25.0));
    const auto expected = *matches[20].screenTicks - *matches[19].screenTicks;
    matches[19].screenTicks.reset(); // Frame 119 was dropped by the desktop.
    std::vector<StallProbe> stalls{{120, 25.0, std::nullopt}};
    measureStalls(stalls, matches, kPeriod, rate);
    ASSERT_TRUE(stalls[0].measuredIntervalMs.has_value());
    EXPECT_NEAR(*stalls[0].measuredIntervalMs, rate.toMs(expected), 1e-9);
}

TEST(ProbeValidation, OneLateStallOfFiveDoesNotFailTheMedian) {
    const TickRate rate(kRate);
    std::vector<StallProbe> stalls;
    for (std::uint32_t i = 0; i < 5; ++i) {
        // 27.8 ms is the expected interval for 25 ms; one stall landed a period later.
        stalls.push_back(StallProbe{100 + i, 25.0, i == 2 ? 41.7 : 27.8});
    }
    std::vector<std::string> problems;
    checkStalls(stalls, kPeriodMs, 0.5, problems);
    EXPECT_TRUE(problems.empty());
    const auto summaries = summariseStalls(stalls);
    ASSERT_EQ(summaries.size(), 1u);
    EXPECT_EQ(summaries[0].measured, 5u);
    EXPECT_NEAR(summaries[0].intervalMs.p50, 27.8, 1e-9);
}

TEST(ProbeValidation, TooFewMeasuredStallsFail) {
    std::vector<StallProbe> stalls{{1, 8.0, 13.9}, {2, 8.0, std::nullopt}, {3, 8.0, std::nullopt}};
    std::vector<std::string> problems;
    checkStalls(stalls, kPeriodMs, 0.5, problems);
    EXPECT_EQ(problems.size(), 1u);
}

TEST(ProbeValidation, CadenceCountsDroppedFramesAsOneRefreshEach) {
    // Frames 0..19 one per period; frame 7 dropped, frame 12 shown with jitter of 2 ms.
    std::vector<FrameMatch> matches;
    for (std::uint32_t seq = 0; seq < 20; ++seq) {
        std::optional<std::int64_t> screen = static_cast<std::int64_t>(seq) * kPeriod;
        if (seq == 7) {
            screen.reset();
        }
        if (seq == 12) {
            *screen += 20'000;
        }
        matches.push_back(FrameMatch{seq, screen});
    }
    const auto check = checkCadence(matches, {}, kPeriod);
    EXPECT_EQ(check.intervals, 18u);
    EXPECT_EQ(check.consistent, 18u);
    EXPECT_EQ(check.dropped, 1u);
    EXPECT_DOUBLE_EQ(check.consistentFraction, 1.0);
}

TEST(ProbeValidation, CadenceFlagsIntervalsThatDisagreeWithTheSequence) {
    std::vector<FrameMatch> matches;
    for (std::uint32_t seq = 0; seq < 10; ++seq) {
        matches.push_back(FrameMatch{seq, static_cast<std::int64_t>(seq) * kPeriod});
    }
    // Frame 5 appears two periods late although no frame was dropped before it.
    *matches[5].screenTicks += 2 * kPeriod;
    const std::vector<std::uint32_t> none;
    const auto check = checkCadence(matches, none, kPeriod);
    EXPECT_EQ(check.intervals, 9u);
    EXPECT_EQ(check.consistent, 7u); // 4->5 and 5->6 disagree.

    // Excluding frame 5 and 6 (a stall and its successor) leaves only consistent pairs.
    const std::vector<std::uint32_t> stalled{5, 6};
    const auto excluded = checkCadence(matches, stalled, kPeriod);
    EXPECT_EQ(excluded.intervals, 7u);
    EXPECT_EQ(excluded.consistent, 7u);
}

TEST(ProbeValidation, MissedStallIsReported) {
    const TickRate rate(kRate);
    // The probe saw no lengthening at all for a 25 ms stall: it must be flagged.
    const auto matches = steadyMatches(50, 0, 0);
    std::vector<StallProbe> stalls{{120, 25.0, std::nullopt}, {500, 8.0, std::nullopt}};
    measureStalls(stalls, matches, kPeriod, rate);
    std::vector<std::string> problems;
    checkStalls(stalls, kPeriodMs, 0.5, problems);
    EXPECT_EQ(problems.size(), 2u); // One too short, one not measured (outside the run).
}

} // namespace
