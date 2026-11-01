// SPDX-License-Identifier: Apache-2.0
//
// Verdicts of one s1_editor run (ADR 0007): which budget gates (B5, B6) were
// measured at all, whether they pass, and whether the run's screen data is good
// enough to count. A gate without enough samples is "not measured" (nullopt),
// never a pass. Also the UI-CPU probe check (--inject-cpu-ms) and the run
// watchdog's allowance. Pure functions, no Qt and no Windows, so the rules have
// headless tests.
#pragma once

#include "frost/perf/stats.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace frost::s1 {

// What a run collected.
struct RunCounts {
    std::size_t flingFrames = 0;       // Frames the fling painted.
    std::size_t cpuIntervals = 0;      // Tick-to-tick UI CPU samples.
    std::size_t decodedUpdates = 0;    // Distinct fling markers seen on screen (ContentStats::updates).
    std::size_t undecodedCaptures = 0; // Display updates whose marker did not decode.
    std::size_t keysSent = 0;          // Synthetic key presses.
    std::size_t keysPainted = 0;       // Key presses whose marker a paint drew.
};

// The measured values the gates compare with their budgets.
struct RunMetrics {
    double cpuP99Ms = 0;
    double missedFraction = 0; // 0..1.
    double maxIntervalMs = 0;
    double b5P50Ms = 0;
    double b5P99Ms = 0;
};

struct Gates {
    // Enough of the fling's markers decoded for the screen statistics to mean
    // anything: decoded updates at least a quarter of the painted frames (the worst
    // desktop measured dropped about half) and undecoded captures at most a tenth
    // of the decoded ones (or 10). A run without it is invalid.
    bool decodedEnough = false;
    std::optional<bool> b6Cpu;      // UI CPU per frame p99 <= 5 ms; needs kMinCpuIntervals.
    std::optional<bool> b6Missed;   // Missed refreshes <= 1 %; needs kMinDecodedUpdates.
    std::optional<bool> b6Interval; // Max content interval <= 16.7 ms; needs kMinDecodedUpdates.
    std::optional<bool> b5;         // Input to paint end p50 <= 3 ms and p99 <= 8 ms; every key painted.
};

inline constexpr std::size_t kMinCpuIntervals = 100; // So p99 is not the maximum of a handful.
inline constexpr std::size_t kMinDecodedUpdates = 2; // An interval needs two updates.

// Budget: B5, B6 (tests/perf/budgets.json). `cpuInjected`: the run spun on
// purpose (--inject-cpu-ms), so its UI CPU says nothing about B6 and, because the
// spin also delays QRhiWidget's deferred render, neither do its missed refreshes
// and intervals: an injected run reports no B6 verdict (decodedEnough still
// decides its validity). O(1).
[[nodiscard]] Gates evaluateGates(const RunCounts& counts, const RunMetrics& metrics, bool cpuInjected);

// The UI-CPU probe check: tick-to-tick intervals that contained an injected spin
// of `injectedMs` against those that did not. The probe passes when the median
// difference is within `toleranceFraction` of the injection.
struct CpuInjectionCheck {
    double injectedMs = 0;
    perf::Summary spunMs;
    perf::Summary plainMs;
    double measuredDeltaMs = 0; // spunMs.p50 - plainMs.p50.
    std::optional<bool> pass;   // nullopt: fewer than kMinInjectionSamples of either kind.
};

inline constexpr std::size_t kMinInjectionSamples = 50;

// Precondition: injectedMs > 0, toleranceFraction > 0. O(n log n).
[[nodiscard]] CpuInjectionCheck checkCpuInjection(std::span<const double> spunMs, std::span<const double> plainMs,
                                                  double injectedMs, double toleranceFraction);

// Slack of the run watchdog beyond the planned fling and typing time: the first
// vblank tick, timer jitter and slow paints.
inline constexpr std::int64_t kWatchdogSlackMs = 4000;

// The watchdog allowance of s1_editor's fling and typing loop, in milliseconds:
// `seconds` of fling, then for keys > 0 the `keys` timeouts `keyIntervalMs` apart
// that each send a key plus the one that ends typing, and kWatchdogSlackMs. A run
// the watchdog ends early stops typing at once. O(1).
[[nodiscard]] std::int64_t runWatchdogMs(double seconds, int keys, int keyIntervalMs);

} // namespace frost::s1
