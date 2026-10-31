// SPDX-License-Identifier: Apache-2.0
//
// Implementation of the frame-pacing analysis (see frost/perf/frame_analysis.hpp).

#include "frost/perf/frame_analysis.hpp"

#include "frost/core/assert.hpp"

#include <cmath>

namespace frost::perf {

TickRate::TickRate(std::int64_t ticksPerSecond) noexcept : m_ticksPerSecond(ticksPerSecond) {
    FROST_PRE(ticksPerSecond > 0);
}

double TickRate::toMs(std::int64_t ticks) const noexcept {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(m_ticksPerSecond);
}

std::int64_t TickRate::fromMs(double ms) const noexcept {
    return static_cast<std::int64_t>(std::llround(ms * static_cast<double>(m_ticksPerSecond) / 1000.0));
}

std::int64_t nextVBlank(std::int64_t ticks, std::int64_t anchorTicks, std::int64_t periodTicks) noexcept {
    FROST_PRE(periodTicks > 0);
    const std::int64_t offset = ticks - anchorTicks;
    // Ceiling division that is also right for negative offsets (anchor in the future).
    std::int64_t periods = offset / periodTicks;
    if (offset % periodTicks > 0) {
        ++periods;
    }
    return anchorTicks + periods * periodTicks;
}

std::vector<ScreenUpdate> collapseToDisplay(std::span<const ScreenUpdate> updates, std::int64_t periodTicks) {
    FROST_PRE(periodTicks > 0);
    std::vector<ScreenUpdate> displayed;
    displayed.reserve(updates.size());
    for (const ScreenUpdate& update : updates) {
        if (!displayed.empty() && update.displayTicks != 0 && displayed.back().displayTicks != 0 &&
            update.displayTicks - displayed.back().displayTicks < periodTicks / 2) {
            displayed.back() = update; // Superseded before scan-out.
        } else {
            displayed.push_back(update);
        }
    }
    return displayed;
}

ContentStats analyseContent(std::span<const ScreenUpdate> updates, std::int64_t beginTicks, std::int64_t endTicks,
                            std::int64_t refreshPeriodTicks, const TickRate& rate) {
    FROST_PRE(refreshPeriodTicks > 0);
    ContentStats stats;
    std::vector<double> intervals;
    std::optional<std::int64_t> previousTicks;
    std::optional<std::uint16_t> previousMarker;
    std::int64_t firstTicks = 0;
    std::int64_t lastTicks = 0;
    for (const ScreenUpdate& update : updates) {
        const std::int64_t ticks = update.screenTicks();
        if (ticks < beginTicks || ticks > endTicks) {
            continue;
        }
        if (update.marker && previousMarker && *update.marker == *previousMarker) {
            continue; // The same application frame composed again (another window changed).
        }
        if (update.accumulatedFrames > 1) {
            ++stats.mergedCaptures;
        }
        if (previousTicks) {
            const std::int64_t interval = ticks - *previousTicks;
            intervals.push_back(rate.toMs(interval));
            const auto periods = std::llround(static_cast<double>(interval) / static_cast<double>(refreshPeriodTicks));
            if (periods > 1) {
                stats.missedRefreshes += static_cast<std::size_t>(periods - 1);
            }
        } else {
            firstTicks = ticks;
        }
        lastTicks = ticks;
        previousTicks = ticks;
        previousMarker = update.marker;
        ++stats.updates;
    }
    if (stats.updates > 1) {
        stats.refreshes = static_cast<std::size_t>(
            std::llround(static_cast<double>(lastTicks - firstTicks) / static_cast<double>(refreshPeriodTicks)));
    }
    stats.intervalsMs = summarize(intervals);
    stats.maxIntervalMs = stats.intervalsMs.max;
    if (stats.refreshes > 0) {
        stats.missedFraction = static_cast<double>(stats.missedRefreshes) / static_cast<double>(stats.refreshes);
    }
    return stats;
}

std::vector<FrameMatch> matchFrames(std::span<const AppFrame> frames, std::span<const ScreenUpdate> updates,
                                    std::int64_t slackTicks) {
    FROST_PRE(frames.size() < 65536);
    std::vector<FrameMatch> matches;
    matches.reserve(frames.size());
    // For each 16-bit marker value, the capture indices that show it, in order.
    constexpr std::size_t kValues = 65536;
    std::vector<std::vector<std::size_t>> byMarker(kValues);
    for (std::size_t i = 0; i < updates.size(); ++i) {
        if (updates[i].marker) {
            byMarker[*updates[i].marker].push_back(i);
        }
    }
    for (const AppFrame& frame : frames) {
        FrameMatch match;
        match.seq = frame.seq;
        const auto& candidates = byMarker[frame.seq & 0xFFFFU];
        for (const std::size_t index : candidates) {
            if (updates[index].presentTicks >= frame.readyTicks - slackTicks) {
                match.screenTicks = updates[index].screenTicks();
                break;
            }
        }
        matches.push_back(match);
    }
    return matches;
}

LatencyStats summariseLatency(std::span<const AppFrame> frames, std::span<const FrameMatch> matches,
                              const TickRate& rate) {
    FROST_PRE(frames.size() == matches.size());
    LatencyStats stats;
    std::vector<double> ready;
    std::vector<double> input;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (!matches[i].screenTicks) {
            ++stats.neverShown;
            continue;
        }
        ++stats.shown;
        ready.push_back(rate.toMs(*matches[i].screenTicks - frames[i].readyTicks));
        if (frames[i].inputTicks != 0) {
            input.push_back(rate.toMs(*matches[i].screenTicks - frames[i].inputTicks));
        }
    }
    stats.readyToScreenMs = summarize(ready);
    stats.inputToScreenMs = summarize(input);
    return stats;
}

DwmDelta dwmDelta(const DwmCounters& before, const DwmCounters& after) noexcept {
    FROST_PRE(after.refreshCount >= before.refreshCount);
    DwmDelta delta;
    // Counters other than the refresh count may be unsupported (always 0) or reset;
    // a decrease is reported as no change rather than wrapping.
    const auto diff = [](std::uint64_t a, std::uint64_t b) { return b >= a ? b - a : 0; };
    delta.refreshes = after.refreshCount - before.refreshCount;
    delta.compositions = diff(before.compositionCount, after.compositionCount);
    delta.missed = diff(before.framesMissed, after.framesMissed);
    delta.dropped = diff(before.framesDropped, after.framesDropped);
    delta.late = diff(before.framesLate, after.framesLate);
    if (delta.refreshes > 0) {
        const auto refreshes = static_cast<double>(delta.refreshes);
        delta.missedFraction = static_cast<double>(delta.missed) / refreshes;
        delta.compositionFraction = static_cast<double>(delta.compositions) / refreshes;
    }
    return delta;
}

} // namespace frost::perf
