// SPDX-License-Identifier: Apache-2.0
//
// Backend-neutral on-screen probes (PLAN.md section 2, "Editor rendering", and
// section 4, Measurement), Windows only, Win32/DXGI/DWM and no Qt:
//
//   readDwmCounters()   DWM's global composition counters and refresh period
//                       (DwmGetCompositionTimingInfo);
//   DuplicationProbe    a Desktop Duplication capture thread that timestamps
//                       every desktop present touching a watched window region
//                       and decodes the window's frame marker
//                       (frost/perf/frame_marker.hpp) from the composed image;
//   VBlankClock         waits for the vertical blank of the output showing a
//                       window, to pace test animations at the display rate.
//
// They observe the composed desktop, not the application's swap chain, so they
// work for raster Qt (GDI BitBlt into the DWM redirection surface, which
// present-based tools such as PresentMon cannot see) and for QRhi alike.
// Coordinates are desktop coordinates in physical pixels; callers must be
// per-monitor DPI aware: embed the FrostIDE manifest (frost.exe and frost_add_test
// targets do; harness executables must add it, as tests/perf/s1 does).
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/perf/frame_analysis.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace frost::perf::win {

// A rectangle in desktop coordinates (physical pixels), right/bottom exclusive.
struct ScreenRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    [[nodiscard]] int width() const noexcept { return right - left; }
    [[nodiscard]] int height() const noexcept { return bottom - top; }
    [[nodiscard]] bool empty() const noexcept { return right <= left || bottom <= top; }
    [[nodiscard]] bool intersects(const ScreenRect& other) const noexcept {
        return left < other.right && other.left < right && top < other.bottom && other.top < bottom;
    }
    [[nodiscard]] bool contains(const ScreenRect& other) const noexcept {
        return left <= other.left && top <= other.top && other.right <= right && other.bottom <= bottom;
    }
};

// Reads DWM's composition counters for the whole desktop (Windows 8.1+ ignores
// per-window timing, so the window handle is always null). Thread-safe.
// Measured on Windows 11 build 26300 (ADR 0007): only the refresh period and the
// vblank time are real. cRefresh and cFrame advance by one per call and
// cFramesMissed stays 0, so missed compositions come from DuplicationProbe.
// Errors: Unsupported when DWM refuses (no composition, session 0).
[[nodiscard]] Result<DwmCounters> readDwmCounters();

// The monitor connection a probe is attached to.
struct OutputInfo {
    std::string deviceName; // UTF-8, e.g. "\\.\DISPLAY1".
    ScreenRect desktop;     // The output's desktop rectangle.
};

// A pixel in desktop coordinates (physical pixels).
struct ScreenPoint {
    int x = 0;
    int y = 0;
};

// One captured desktop present.
struct CaptureFrame {
    ScreenUpdate update;            // Present time, decoded marker, merged presents.
    std::int64_t acquiredTicks = 0; // When the capture thread received it (QPC).
    bool watchedDirty = false;      // Its dirty or move rectangles touch the watched region.
    std::vector<ScreenRect> dirty;  // Desktop coordinates; filled only with keepDirtyRects.
    // The colour (0xRRGGBB, 8-bit sRGB) of each DuplicationOptions::samples point in
    // this image, in the same order; empty when no samples were requested or the
    // read failed. FP16 desktops are converted from linear scRGB.
    std::vector<std::uint32_t> samples;
};

struct DuplicationOptions {
    ScreenRect watch;                 // Region whose changes are recorded (may be empty until setWatch).
    std::optional<ScreenRect> marker; // Top-left pixel of the frame-marker strip (only left/top are used).
    int markerCellPx = 4;             // Marker cell size in physical pixels; at least 1.
    bool keepDirtyRects = false;      // Store every frame's dirty rectangles (B1 needs them after the fact).
    bool keepAllFrames = false;       // Record frames that do not touch the watched region too.
    // Pixels read from every recorded frame (B1 detects a window's first frame by
    // its colour). At most 64 points; points outside the output read as 0.
    std::vector<ScreenPoint> samples;
    // The output to duplicate is the one containing this point (desktop coordinates).
    int outputX = 0;
    int outputY = 0;
};

// Desktop Duplication capture of one output on a background thread.
//
// Lifetime: create with open(), start(), stop() (or destroy, which stops). The
// capture thread runs at THREAD_PRIORITY_HIGHEST and releases each frame within
// about a millisecond, so it keeps up with 144 Hz; merged presents are counted
// in ScreenUpdate::accumulatedFrames. Thread-safe: frames(), waitForMarker(),
// setWatch() and the counters may be called from any thread while it runs.
class DuplicationProbe {
public:
    // Opens a D3D11 device on the adapter that drives the output and duplicates the
    // output. Errors: NotFound (no attached output contains the point), InvalidArgument
    // (the marker strip is not inside that output), Unsupported (duplication refused:
    // secure desktop, rotated output, too many duplicators), IoError (other DXGI
    // failures). Precondition: options.markerCellPx >= 1.
    [[nodiscard]] static Result<std::unique_ptr<DuplicationProbe>> open(const DuplicationOptions& options);

    ~DuplicationProbe();
    DuplicationProbe(const DuplicationProbe&) = delete ("owns a capture thread and DXGI objects");
    DuplicationProbe& operator=(const DuplicationProbe&) = delete ("owns a capture thread and DXGI objects");

    // Starts the capture thread. Precondition: not started.
    void start();
    // Stops and joins the capture thread. Idempotent.
    void stop();

    // Replaces the watched region; frames captured afterwards use it.
    void setWatch(const ScreenRect& watch);

    // A copy of the frames recorded so far, in capture order.
    [[nodiscard]] std::vector<CaptureFrame> frames() const;

    // Waits up to `timeout` for a frame showing marker `seq` presented at or after
    // `notBeforeTicks`; returns it, or nullopt on timeout.
    [[nodiscard]] std::optional<CaptureFrame> waitForMarker(std::uint16_t seq, std::int64_t notBeforeTicks,
                                                            std::chrono::milliseconds timeout);

    [[nodiscard]] const OutputInfo& output() const noexcept;
    // DXGI_ERROR_ACCESS_LOST events (desktop switch, mode change, lock). Any loss invalidates a run.
    [[nodiscard]] std::uint32_t accessLost() const noexcept;
    // The capture thread's last fatal error, if it stopped on one.
    [[nodiscard]] std::optional<std::string> failure() const;

    // Diagnostics: the DXGI_FORMAT of the last desktop image a marker strip was read
    // from, and the raw bytes of the centre row of the last marker strip read, in that
    // format. Unsupported formats are not read, so the format is 0 until a strip is first
    // read and keeps the last supported format read after the desktop turns unsupported.
    [[nodiscard]] int desktopFormat() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> lastMarkerRow() const;

    struct Impl;

private:
    explicit DuplicationProbe(std::unique_ptr<Impl> impl);
    void run();

    std::unique_ptr<Impl> m_impl;
    mutable Mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<CaptureFrame> m_frames FROST_GUARDED_BY(m_mutex);
    ScreenRect m_watch FROST_GUARDED_BY(m_mutex);
    std::optional<std::string> m_failure FROST_GUARDED_BY(m_mutex);
    std::atomic<bool> m_stop{false};
    std::atomic<std::uint32_t> m_accessLost{0};
    std::thread m_thread;
};

// Waits for vertical blanks of the output containing a point (IDXGIOutput::WaitForVBlank).
// Movable, not copyable; use from one thread at a time.
class VBlankClock {
public:
    // Errors: NotFound when no attached output contains the point; IoError when DXGI
    // cannot be initialised.
    [[nodiscard]] static Result<VBlankClock> forPoint(int x, int y);

    VBlankClock(VBlankClock&& other) noexcept;
    VBlankClock& operator=(VBlankClock&& other) noexcept;
    ~VBlankClock();
    VBlankClock(const VBlankClock&) = delete ("owns a DXGI output reference");
    VBlankClock& operator=(const VBlankClock&) = delete ("owns a DXGI output reference");

    // Blocks until the next vertical blank; returns qpcNow() on wake-up.
    // Errors: IoError when WaitForVBlank fails. Precondition: not moved from.
    [[nodiscard]] Result<std::int64_t> wait();

private:
    explicit VBlankClock(void* output) noexcept : m_output(output) {}
    void* m_output = nullptr; // IDXGIOutput*, owned (one reference).
};

} // namespace frost::perf::win
