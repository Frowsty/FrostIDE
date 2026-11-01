// SPDX-License-Identifier: Apache-2.0
//
// Backend-neutral frame-pacing analysis for B1, B5 and B6 (tests/perf/budgets.json).
// The inputs are plain timestamps, so the same code scores a raster window, a
// QRhi window or the GDI validation window, and unit tests feed it synthetic
// timelines. Timestamps are ticks of one monotonic clock (QueryPerformanceCounter
// on Windows) and TickRate converts them.
//
// Definitions used by the S1 report (ADR 0007):
//   screen update     a Desktop Duplication frame whose dirty region touches the
//                     watched window and, when the window paints a frame marker,
//                     whose marker shows a sequence number not seen before. Its
//                     time is the first vblank after DWM presented it (DWM's
//                     present times jitter across the refresh period, so raw
//                     present intervals do not count refreshes); of several
//                     presents before one vblank only the last reaches the screen;
//   content interval  the time between two consecutive screen updates;
//   missed refresh    a display refresh between the first and last screen update
//                     inside the measured span that showed no new content (refreshes
//                     before the first or after the last update are not counted):
//                     each interval of k refresh periods (rounded) contributes k - 1;
//   DWM counters      DwmGetCompositionTimingInfo's refresh, composition and
//                     missed-composition counters, differenced over the span
//                     (kept for other Windows builds; on build 26300 they are
//                     synthetic, see frost/perf/win/screen_probes.hpp).
#pragma once

#include "frost/perf/stats.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace frost::perf {

// Converts clock ticks to milliseconds. Value type; thread-safe.
class TickRate {
public:
    // Precondition: ticksPerSecond > 0.
    explicit TickRate(std::int64_t ticksPerSecond) noexcept;

    [[nodiscard]] std::int64_t ticksPerSecond() const noexcept { return m_ticksPerSecond; }
    [[nodiscard]] double toMs(std::int64_t ticks) const noexcept;
    [[nodiscard]] std::int64_t fromMs(double ms) const noexcept;

private:
    std::int64_t m_ticksPerSecond;
};

// One frame seen by the Desktop Duplication probe.
struct ScreenUpdate {
    std::int64_t presentTicks = 0;       // When DWM presented the desktop image (DXGI LastPresentTime).
    std::optional<std::uint16_t> marker; // The decoded frame marker, if the window paints one.
    std::uint32_t accumulatedFrames = 1; // Desktop presents merged into this capture (1 = none merged).
    // The first vertical blank at or after presentTicks, on DWM's vblank grid: when
    // scan-out of the image can start. 0 when the grid was unknown.
    std::int64_t displayTicks = 0;

    // The time used for intervals: displayTicks when known, else presentTicks.
    [[nodiscard]] std::int64_t screenTicks() const noexcept { return displayTicks != 0 ? displayTicks : presentTicks; }
};

// The first vblank at or after `ticks` on the grid {anchor + n * period}.
// Precondition: period > 0. O(1).
[[nodiscard]] std::int64_t nextVBlank(std::int64_t ticks, std::int64_t anchorTicks, std::int64_t periodTicks) noexcept;

// Keeps, of consecutive updates that share a display vblank, only the last one:
// DWM presented again before scan-out, so the earlier image never reached the
// screen. Vblanks closer than half a period count as one (each capture anchors
// the grid on DWM's latest vblank reading, which jitters by a few ticks).
// Updates without displayTicks are kept. Precondition: periodTicks > 0. O(n).
[[nodiscard]] std::vector<ScreenUpdate> collapseToDisplay(std::span<const ScreenUpdate> updates,
                                                          std::int64_t periodTicks);

// Pacing of a window's on-screen content over [beginTicks, endTicks].
struct ContentStats {
    std::size_t updates = 0;         // Screen updates with new content inside the span.
    std::size_t refreshes = 0;       // Refresh periods between the first and last update (rounded).
    std::size_t missedRefreshes = 0; // Refreshes in that range that showed no new content.
    double missedFraction = 0;       // missedRefreshes / refreshes (0 when refreshes is 0).
    double maxIntervalMs = 0;        // Longest content interval.
    Summary intervalsMs;             // All content intervals.
    std::size_t mergedCaptures = 0;  // Updates whose capture merged several desktop presents.
};

// Analyses `updates` (in capture order, normally collapsed with collapseToDisplay)
// by their screenTicks(). Only updates inside the span count; an update whose
// marker equals the previous counted marker is a repaint of the same frame and
// is skipped. Updates without a marker always count. Precondition:
// refreshPeriodTicks > 0. O(n).
[[nodiscard]] ContentStats analyseContent(std::span<const ScreenUpdate> updates, std::int64_t beginTicks,
                                          std::int64_t endTicks, std::int64_t refreshPeriodTicks, const TickRate& rate);

// One frame the application finished (painted and flushed or presented).
struct AppFrame {
    std::uint32_t seq = 0;       // Sequence number; the marker shows its low 16 bits.
    std::int64_t readyTicks = 0; // When the frame was handed to the OS (flush or Present returned).
    std::int64_t inputTicks = 0; // The input or tick that started the frame (0 if none).
};

// When each application frame reached the screen.
struct FrameMatch {
    std::uint32_t seq = 0;
    std::optional<std::int64_t> screenTicks; // The first screen update showing the frame; nullopt if never shown.
};

// Matches application frames to the first screen update showing their marker
// whose presentTicks is at or after the frame's readyTicks minus `slackTicks`
// (clock-domain slack); the match records that update's screenTicks(). Frames
// must be in submission order and their sequence numbers must span fewer than
// 65536 values (distinct low 16 bits), so the 16-bit marker is unambiguous; the
// precondition check covers only frames.size() < 65536. O(frames + updates).
[[nodiscard]] std::vector<FrameMatch> matchFrames(std::span<const AppFrame> frames,
                                                  std::span<const ScreenUpdate> updates, std::int64_t slackTicks);

// Ready-to-screen latency of matched frames, in milliseconds, plus how many never showed.
struct LatencyStats {
    Summary readyToScreenMs;
    Summary inputToScreenMs; // Only frames with inputTicks != 0.
    std::size_t shown = 0;
    std::size_t neverShown = 0;
};

// Summarises the latency of frames[i] against matches[i] (normally matchFrames(frames, ...)).
// Precondition: frames.size() == matches.size(). O(n log n). Pure.
[[nodiscard]] LatencyStats summariseLatency(std::span<const AppFrame> frames, std::span<const FrameMatch> matches,
                                            const TickRate& rate);

// A snapshot of DWM's global composition counters (DwmGetCompositionTimingInfo with a null window).
struct DwmCounters {
    std::uint64_t refreshCount = 0;     // cRefresh: display refreshes since DWM started.
    std::uint64_t compositionCount = 0; // cFrame: compositions (DWM frames).
    std::uint64_t framesMissed = 0;     // cFramesMissed: compositions DWM missed.
    std::uint64_t framesDropped = 0;    // cFramesDropped.
    std::uint64_t framesLate = 0;       // cFramesLate.
    std::int64_t refreshPeriodTicks = 0;
    std::int64_t vblankTicks = 0;    // qpcVBlank: a recent vertical blank.
    std::uint32_t rateNumerator = 0; // rateRefresh, e.g. 144000/1000 (some drivers report 0/0).
    std::uint32_t rateDenominator = 0;
};

// Counter differences between two snapshots.
struct DwmDelta {
    std::uint64_t refreshes = 0;
    std::uint64_t compositions = 0;
    std::uint64_t missed = 0;
    std::uint64_t dropped = 0;
    std::uint64_t late = 0;
    double missedFraction = 0;      // missed / refreshes.
    double compositionFraction = 0; // compositions / refreshes (desktop-wide, not per window).
};

// Precondition: `after` was read no earlier than `before` (refreshCount is monotonic).
// The other counters may be unsupported or reset; a decrease is reported as 0. O(1).
[[nodiscard]] DwmDelta dwmDelta(const DwmCounters& before, const DwmCounters& after) noexcept;

} // namespace frost::perf
