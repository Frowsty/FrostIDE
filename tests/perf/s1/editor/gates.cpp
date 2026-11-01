// SPDX-License-Identifier: Apache-2.0
//
// Implementation of the s1_editor verdict rules (see gates.hpp).

#include "gates.hpp"

#include <algorithm>
#include <cmath>

namespace frost::s1 {

Gates evaluateGates(const RunCounts& counts, const RunMetrics& metrics, bool cpuInjected) {
    // Budget: B5, B6.
    Gates gates;
    gates.decodedEnough = counts.flingFrames > 0 && counts.decodedUpdates >= kMinDecodedUpdates &&
                          counts.decodedUpdates * 4 >= counts.flingFrames &&
                          counts.undecodedCaptures <= std::max<std::size_t>(10, counts.decodedUpdates / 10);
    if (!cpuInjected && counts.cpuIntervals >= kMinCpuIntervals) {
        gates.b6Cpu = metrics.cpuP99Ms <= 5.0;
    }
    if (counts.decodedUpdates >= kMinDecodedUpdates) {
        gates.b6Missed = metrics.missedFraction <= 0.01;
        gates.b6Interval = metrics.maxIntervalMs <= 16.7;
    }
    if (counts.keysSent > 0 && counts.keysPainted == counts.keysSent) {
        gates.b5 = metrics.b5P50Ms <= 3.0 && metrics.b5P99Ms <= 8.0;
    }
    return gates;
}

CpuInjectionCheck checkCpuInjection(std::span<const double> spunMs, std::span<const double> plainMs, double injectedMs,
                                    double toleranceFraction) {
    CpuInjectionCheck check;
    check.injectedMs = injectedMs;
    check.spunMs = perf::summarize(spunMs);
    check.plainMs = perf::summarize(plainMs);
    check.measuredDeltaMs = check.spunMs.p50 - check.plainMs.p50;
    if (spunMs.size() >= kMinInjectionSamples && plainMs.size() >= kMinInjectionSamples) {
        check.pass = std::abs(check.measuredDeltaMs - injectedMs) <= toleranceFraction * injectedMs;
    }
    return check;
}

} // namespace frost::s1
