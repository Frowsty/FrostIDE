// SPDX-License-Identifier: Apache-2.0
//
// Implementation of the probe-validation scoring (see frost/perf/probe_validation.hpp).

#include "frost/perf/probe_validation.hpp"

#include "frost/core/assert.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

namespace frost::perf {

ValidationReport evaluateDelayTrials(std::span<const DelayTrial> trials, double refreshPeriodMs, const TickRate& rate,
                                     const ValidationTolerance& tolerance) {
    FROST_PRE(refreshPeriodMs > 0);
    ValidationReport report;
    // Group by injected delay, keyed in microseconds so 8.0 and 8.0000001 group together.
    std::map<std::int64_t, std::vector<const DelayTrial*>> groups;
    for (const DelayTrial& trial : trials) {
        groups[std::llround(trial.injectedMs * 1000.0)].push_back(&trial);
    }
    if (groups.empty() || groups.begin()->first != 0) {
        report.problems.emplace_back("no D = 0 trials: the latency baseline is missing");
        return report;
    }

    const double maxResidualMs = tolerance.maxResidualPeriods * refreshPeriodMs;
    for (const auto& [key, group] : groups) {
        DelayResult result;
        result.injectedMs = static_cast<double>(key) / 1000.0;
        result.trials = group.size();
        std::vector<double> latency;
        std::vector<double> residual;
        for (const DelayTrial* trial : group) {
            if (!trial->screenTicks) {
                ++result.lost;
                continue;
            }
            latency.push_back(rate.toMs(*trial->screenTicks - trial->triggerTicks));
            residual.push_back(rate.toMs(*trial->screenTicks - trial->drawnTicks));
        }
        result.latencyMs = summarize(latency);
        result.residualMs = summarize(residual);
        if (result.lost > 0) {
            report.problems.push_back(std::format("D = {} ms: {} of {} trials never reached the probe",
                                                  result.injectedMs, result.lost, result.trials));
        }
        if (result.residualMs.count > 0 && result.residualMs.min < -tolerance.slackMs) {
            report.problems.push_back(
                std::format("D = {} ms: a trial was seen {:.3f} ms before its paint was flushed (causality)",
                            result.injectedMs, -result.residualMs.min));
        }
        if (result.residualMs.count > 0 && result.residualMs.max > maxResidualMs) {
            report.problems.push_back(std::format("D = {} ms: a trial took {:.3f} ms from flush to screen, more than "
                                                  "{} refresh periods",
                                                  result.injectedMs, result.residualMs.max,
                                                  tolerance.maxResidualPeriods));
        }
        report.delays.push_back(result);
    }

    const double baseline = report.delays.front().latencyMs.mean;
    for (DelayResult& result : report.delays) {
        result.meanShiftMs = result.latencyMs.mean - baseline;
        result.shiftErrorMs = result.meanShiftMs - result.injectedMs;
        if (result.latencyMs.count > 0 && std::abs(result.shiftErrorMs) > tolerance.toleranceMs) {
            report.problems.push_back(
                std::format("D = {} ms: the mean latency shifted by {:.3f} ms, outside {} +/- {} ms", result.injectedMs,
                            result.meanShiftMs, result.injectedMs, tolerance.toleranceMs));
        }
    }
    report.passed = report.problems.empty();
    return report;
}

void measureStalls(std::span<StallProbe> stalls, std::span<const FrameMatch> matches, std::int64_t refreshPeriodTicks,
                   const TickRate& rate) {
    if (matches.empty()) {
        return;
    }
    const std::uint32_t first = matches.front().seq;
    for (StallProbe& stall : stalls) {
        stall.measuredIntervalMs.reset();
        if (stall.seq <= first || stall.seq - first >= matches.size()) {
            continue;
        }
        const FrameMatch& current = matches[stall.seq - first];
        if (!current.screenTicks) {
            continue;
        }
        // The last shown frame before the stall (at most 8 back, so a long dropout is not bridged).
        for (std::uint32_t back = 1; back <= 8 && back <= stall.seq - first; ++back) {
            const FrameMatch& previous = matches[stall.seq - first - back];
            if (previous.screenTicks) {
                const std::int64_t skipped = static_cast<std::int64_t>(back - 1) * refreshPeriodTicks;
                stall.measuredIntervalMs = rate.toMs(*current.screenTicks - *previous.screenTicks - skipped);
                break;
            }
        }
    }
}

std::vector<StallSummary> summariseStalls(std::span<const StallProbe> stalls) {
    std::map<std::int64_t, std::pair<std::size_t, std::vector<double>>> groups;
    for (const StallProbe& stall : stalls) {
        auto& [injected, intervals] = groups[std::llround(stall.stallMs * 1000.0)];
        ++injected;
        if (stall.measuredIntervalMs) {
            intervals.push_back(*stall.measuredIntervalMs);
        }
    }
    std::vector<StallSummary> summaries;
    for (const auto& [key, group] : groups) {
        StallSummary summary;
        summary.stallMs = static_cast<double>(key) / 1000.0;
        summary.injected = group.first;
        summary.measured = group.second.size();
        summary.intervalMs = summarize(group.second);
        summaries.push_back(summary);
    }
    return summaries;
}

void checkStalls(std::span<const StallProbe> stalls, double refreshPeriodMs, double slackMs,
                 std::vector<std::string>& problems, double minMeasuredFraction) {
    for (const StallSummary& summary : summariseStalls(stalls)) {
        const double needed = minMeasuredFraction * static_cast<double>(summary.injected);
        if (static_cast<double>(summary.measured) < needed || summary.measured == 0) {
            problems.push_back(std::format("stalls of {} ms: only {} of {} intervals were measured", summary.stallMs,
                                           summary.measured, summary.injected));
            continue;
        }
        // The stalled frame is painted D after the vsync that released it, so it is
        // shown on the first vsync after that (up to one period later), plus up to
        // one period of composition latency.
        const double low = std::max(summary.stallMs, refreshPeriodMs) - slackMs;
        const double high = summary.stallMs + 2.0 * refreshPeriodMs + slackMs;
        const double median = summary.intervalMs.p50;
        if (median < low || median > high) {
            problems.push_back(std::format("stalls of {} ms: median interval {:.3f} ms is outside [{:.3f}, {:.3f}] ms",
                                           summary.stallMs, median, low, high));
        }
    }
}

CadenceCheck checkCadence(std::span<const FrameMatch> matches, std::span<const std::uint32_t> excluded,
                          std::int64_t refreshPeriodTicks) {
    FROST_PRE(refreshPeriodTicks > 0);
    CadenceCheck check;
    const FrameMatch* previous = nullptr;
    for (const FrameMatch& match : matches) {
        if (!match.screenTicks) {
            continue;
        }
        const bool skip = std::ranges::find(excluded, match.seq) != excluded.end();
        if (previous != nullptr && !skip) {
            const std::uint32_t gap = match.seq - previous->seq;
            const auto periods = std::llround(static_cast<double>(*match.screenTicks - *previous->screenTicks) /
                                              static_cast<double>(refreshPeriodTicks));
            ++check.intervals;
            check.dropped += gap - 1;
            if (periods == static_cast<long long>(gap)) {
                ++check.consistent;
            }
        }
        previous = &match;
    }
    if (check.intervals > 0) {
        check.consistentFraction = static_cast<double>(check.consistent) / static_cast<double>(check.intervals);
    }
    return check;
}

} // namespace frost::perf
