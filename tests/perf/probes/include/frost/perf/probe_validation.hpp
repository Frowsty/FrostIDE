// SPDX-License-Identifier: Apache-2.0
//
// Validation of the on-screen probes by injected delays (PLAN.md section 7, M0
// spike S1). A test window waits a known delay D between a trigger and its paint;
// the probes must see the paint on screen D later than with no delay. This file
// holds the scoring, so the acceptance rules are unit-tested; the Windows test
// window lives in tests/perf/s1/probe_validate.cpp.
//
// Acceptance (ADR 0007):
//   causality  no trial is seen on screen before its paint was flushed
//              (screen - drawn >= -slack);
//   bounded    every trial is seen within `maxResidualPeriods` refresh periods of
//              its flush, so the probe reports no stale frame;
//   accuracy   mean(latency at D) - mean(latency at 0) is within `toleranceMs` of
//              D. Trigger phases are uniform over a refresh period, so vsync
//              quantisation averages out of the mean;
//   complete   every trial was seen.
#pragma once

#include "frost/perf/frame_analysis.hpp"
#include "frost/perf/stats.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace frost::perf {

// One trial of the delay test.
struct DelayTrial {
    double injectedMs = 0;                   // D.
    std::int64_t triggerTicks = 0;           // The simulated input.
    std::int64_t drawnTicks = 0;             // The paint was flushed (trigger + D + paint time).
    std::optional<std::int64_t> screenTicks; // The probe saw it on screen.
};

struct ValidationTolerance {
    double toleranceMs = 1.5; // Accuracy bound on the mean shift.
    double slackMs = 0.5;     // Clock and timestamp slack for causality.
    // Bound on screen - drawn, in refresh periods: up to one period until DWM
    // composes, one to the display vblank, and two compositions the desktop may
    // skip (ADR 0007 measured one in five skipped on a busy desktop).
    double maxResidualPeriods = 4.0;
};

// Results for one injected delay.
struct DelayResult {
    double injectedMs = 0;
    std::size_t trials = 0;
    std::size_t lost = 0;    // Trials never seen on screen.
    Summary latencyMs;       // screen - trigger.
    Summary residualMs;      // screen - drawn.
    double meanShiftMs = 0;  // mean latency minus the D = 0 mean latency.
    double shiftErrorMs = 0; // meanShiftMs - injectedMs.
};

struct ValidationReport {
    bool passed = false;
    std::vector<DelayResult> delays; // Ascending by injected delay; the first must be D = 0.
    std::vector<std::string> problems;
};

// Scores `trials` against `tolerance`. Precondition: refreshPeriodMs > 0. A run
// without D = 0 trials cannot be scored and fails. O(n log n).
[[nodiscard]] ValidationReport evaluateDelayTrials(std::span<const DelayTrial> trials, double refreshPeriodMs,
                                                   const TickRate& rate, const ValidationTolerance& tolerance = {});

// One injected stall during a steady animation: frame `seq` was delayed by `stallMs`.
struct StallProbe {
    std::uint32_t seq = 0;
    double stallMs = 0;
    std::optional<double> measuredIntervalMs; // screen(seq) - screen(seq - 1), when both were seen.
};

// Fills measuredIntervalMs for each stall from matched frames (seq -> screen time):
// screen(seq) - screen(prev) - (seq - prev - 1) periods, where prev is the last
// shown frame before seq (normally seq - 1; earlier when the desktop dropped
// frames, which on a one-frame-per-refresh animation each took one period).
// Stalls whose frame was never shown stay unmeasured. Frames must be consecutive
// sequence numbers starting at matches.front().seq.
void measureStalls(std::span<StallProbe> stalls, std::span<const FrameMatch> matches, std::int64_t refreshPeriodTicks,
                   const TickRate& rate);

// Per injected stall length: how often it was measured and the median interval.
struct StallSummary {
    double stallMs = 0;
    std::size_t injected = 0;
    std::size_t measured = 0;
    Summary intervalMs;
};

// Groups stalls by length (ascending). O(n log n).
[[nodiscard]] std::vector<StallSummary> summariseStalls(std::span<const StallProbe> stalls);

// A stall of D ms should leave the interval between max(D, one refresh period)
// and D + 2 refresh periods: the stalled frame lands on the first vsync after its
// paint, and a stall shorter than a period may still make its vsync. Individual
// stalls can land one period later when the desktop drops a composition, so the
// check is statistical: for every stall length, at least `minMeasuredFraction`
// of the stalls must be measured (the stalled frame and its predecessor shown)
// and the median interval must lie in that window widened by `slackMs`. Appends
// one problem per failed stall length.
void checkStalls(std::span<const StallProbe> stalls, double refreshPeriodMs, double slackMs,
                 std::vector<std::string>& problems, double minMeasuredFraction = 0.8);

// Cadence consistency for an animation that paints one frame per refresh: for
// consecutive shown frames a and b with no stall between them, the screen
// interval in refresh periods, round((screen(b) - screen(a)) / period), must
// equal b - a, so every frame the desktop dropped costs exactly one refresh.
struct CadenceCheck {
    std::size_t intervals = 0;  // Pairs compared.
    std::size_t consistent = 0; // Pairs whose period count matched the sequence gap.
    std::size_t dropped = 0;    // Frames between compared pairs that never reached the screen.
    double consistentFraction = 0;
};

// `excluded` lists sequence numbers whose interval to the previous shown frame
// is not compared (stalled frames and the frames right after them).
[[nodiscard]] CadenceCheck checkCadence(std::span<const FrameMatch> matches, std::span<const std::uint32_t> excluded,
                                        std::int64_t refreshPeriodTicks);

} // namespace frost::perf
