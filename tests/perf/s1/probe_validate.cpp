// SPDX-License-Identifier: Apache-2.0
//
// s1_probe_validate: validates the backend-neutral on-screen probes by injected
// delays (PLAN.md section 7, M0 spike S1; ADR 0007).
//
// A small topmost GDI window (no Qt; it never takes focus) paints a frame
// marker. Two phases, about 12 s on screen in total:
//   A. Latency. For each injected delay D in {0, 8, 25} ms, `--trials` times
//      (interleaved): wait for a vblank, wait a phase spread evenly over one
//      refresh period, take the trigger time, wait D, paint the next marker and
//      flush. The Desktop Duplication probe reports when the marker reached the
//      screen. The mean latency must shift by D within 1.5 ms, no trial may be
//      seen before its flush or later than 3 refresh periods after it, and none
//      may be lost (frost/perf/probe_validation.hpp).
//   B. Stalls and cadence. A steady animation paints one frame per vblank for
//      720 frames (5 s), with 10 stalls each of 8 ms and 25 ms. At least 60 % of
//      each must be measured, the median screen interval of each stall length must
//      lie in [max(D, period), D + 2 periods], the B6 interval check must flag
//      every measured 25 ms stall (> 16.7 ms), and every other interval must count
//      exactly one refresh per frame step, so a frame the desktop dropped costs one
//      missed refresh (cadence consistency >= 95 %). DWM's refresh period must
//      match the measured vblank interval.
// The fraction of frames the desktop dropped in phase B is reported as the
// environment floor: it is a property of the machine (other windows composing),
// not of the probe.
//
//   s1_probe_validate [--out report.json] [--trials N] [--diagnose]
// Exit status: 0 validated, 1 validation failed, 2 setup error, 3 invalid session
// (locked, remote or monitor off; the run is neither pass nor fail).

#include "frost/perf/frame_analysis.hpp"
#include "frost/perf/frame_marker.hpp"
#include "frost/perf/json_writer.hpp"
#include "frost/perf/probe_validation.hpp"
#include "frost/perf/report.hpp"
#include "frost/perf/win/screen_probes.hpp"
#include "frost/perf/win/system_session_state.hpp"
#include "frost/perf/win/timing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>

#include <shellapi.h>

namespace {

using namespace frost::perf;
using namespace frost::perf::win;

constexpr int kWindowWidth = 320;
constexpr int kWindowHeight = 48;
constexpr int kMarkerX = 8; // Client coordinates of the marker strip.
constexpr int kMarkerY = 8;
constexpr int kCellPx = 4;
constexpr std::array<double, 3> kDelaysMs{0.0, 8.0, 25.0};
constexpr int kSteadyFrames = 720;
constexpr int kStallEvery = 36; // Frames 18, 54, ...: 20 stalls, alternating 8 and 25 ms.
constexpr std::array<double, 2> kStallMs{8.0, 25.0};
constexpr double kMinCadenceConsistency = 0.95;
// A stalled frame is unmeasured only when the desktop drops it; at the measured
// floor of about 20 % drops, 60 % measured leaves margin.
constexpr double kMinStallsMeasured = 0.6;

struct Options {
    std::wstring out;
    int trials = 48;
    bool diagnose = false; // Print raw capture data for one second instead of validating.
};

// The test window: a topmost, non-activating tool window that never enters the
// taskbar or takes focus from the user.
class MarkerWindow {
public:
    MarkerWindow() = default;
    MarkerWindow(const MarkerWindow&) = delete ("owns a window handle");
    MarkerWindow& operator=(const MarkerWindow&) = delete ("owns a window handle");
    ~MarkerWindow() {
        if (m_hwnd != nullptr) {
            DestroyWindow(m_hwnd);
        }
    }

    bool create() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &MarkerWindow::proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"FrostS1ProbeValidate";
        RegisterClassExW(&wc);
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int x = work.right - kWindowWidth - 24;
        const int y = work.bottom - kWindowHeight - 24;
        m_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                                 L"FrostIDE S1 probe validation", WS_POPUP, x, y, kWindowWidth, kWindowHeight, nullptr,
                                 nullptr, wc.hInstance, nullptr);
        if (m_hwnd == nullptr) {
            return false;
        }
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(m_hwnd);
        return true;
    }

    // The marker strip's top-left pixel in desktop coordinates.
    [[nodiscard]] POINT markerOrigin() const {
        POINT p{kMarkerX, kMarkerY};
        ClientToScreen(m_hwnd, &p);
        return p;
    }

    [[nodiscard]] ScreenRect screenRect() const {
        RECT r{};
        GetWindowRect(m_hwnd, &r);
        return ScreenRect{r.left, r.top, r.right, r.bottom};
    }

    // Paints marker `seq` straight into the window (outside WM_PAINT) and flushes GDI.
    void paint(std::uint16_t seq) {
        m_seq = seq;
        HDC dc = GetDC(m_hwnd);
        drawMarker(dc, seq);
        ReleaseDC(m_hwnd, dc);
        GdiFlush();
    }

    // Dispatches pending messages without blocking, so the window never looks hung
    // (DWM would replace a hung window with a ghost after 5 s).
    static void pump() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

private:
    static void drawMarker(HDC dc, std::uint16_t seq) {
        const auto cells = encodeMarker(seq);
        HBRUSH white = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        for (int cell = 0; cell < kMarkerCells; ++cell) {
            const RECT r{kMarkerX + cell * kCellPx, kMarkerY, kMarkerX + (cell + 1) * kCellPx, kMarkerY + kCellPx};
            FillRect(dc, &r, cells[static_cast<std::size_t>(cell)] ? white : black);
        }
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<MarkerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (message) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            HBRUSH background = CreateSolidBrush(RGB(40, 44, 52));
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect(dc, &client, background);
            DeleteObject(background);
            drawMarker(dc, self != nullptr ? self->m_seq : std::uint16_t{0});
            EndPaint(hwnd, &ps);
            return 0;
        }
        default: break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    HWND m_hwnd = nullptr;
    std::uint16_t m_seq = 0;
};

Options parseOptions() {
    Options options;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; argv != nullptr && i < count; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--out" && i + 1 < count) {
            options.out = argv[++i];
        } else if (arg == L"--trials" && i + 1 < count) {
            options.trials = std::max(4, _wtoi(argv[++i]));
        } else if (arg == L"--diagnose") {
            options.diagnose = true;
        }
    }
    LocalFree(argv);
    return options;
}

void writeReport(const Options& options, const std::string& json) {
    if (!options.out.empty()) {
        std::ofstream file(options.out, std::ios::binary);
        file << json << '\n';
    }
}

// Pumps messages for `ms` milliseconds (lets DWM finish the window's show animation).
void settle(double ms) {
    const TickRate rate = qpcRate();
    const auto deadline = qpcNow() + rate.fromMs(ms);
    while (qpcNow() < deadline) {
        MarkerWindow::pump();
        Sleep(1);
    }
}

// --diagnose: paints one marker per vblank for one second and lists every desktop
// present with its dirty-region bounding box, to identify other windows that
// make DWM compose (ADR 0007 "Environment").
int diagnose(MarkerWindow& window, DuplicationProbe& probe, VBlankClock& clock, const TickRate& rate) {
    const auto t0 = qpcNow();
    std::vector<double> vblankIntervals;
    std::int64_t last = 0;
    for (std::uint16_t seq = 2000; seq < 2144; ++seq) {
        MarkerWindow::pump();
        const auto vblank = clock.wait();
        if (vblank && last != 0) {
            vblankIntervals.push_back(rate.toMs(*vblank - last));
        }
        last = vblank.value_or(0);
        window.paint(seq);
    }
    settle(200.0);
    probe.stop();
    const auto frames = probe.frames();
    std::size_t ours = 0;
    std::size_t fullScreen = 0;
    for (const CaptureFrame& f : frames) {
        ScreenRect box{1 << 30, 1 << 30, -(1 << 30), -(1 << 30)};
        for (const ScreenRect& r : f.dirty) {
            box = ScreenRect{std::min(box.left, r.left), std::min(box.top, r.top), std::max(box.right, r.right),
                             std::max(box.bottom, r.bottom)};
        }
        ours += f.update.marker && *f.update.marker >= 2000 ? 1 : 0;
        fullScreen += box.contains(probe.output().desktop) ? 1 : 0;
        std::printf("  present %+9.3f ms acc %u ours %d marker %5d dirty %zu rects bbox %d,%d-%d,%d\n",
                    rate.toMs(f.update.presentTicks - t0), f.update.accumulatedFrames, f.watchedDirty ? 1 : 0,
                    f.update.marker ? static_cast<int>(*f.update.marker) : -1, f.dirty.size(), box.left, box.top,
                    box.right, box.bottom);
    }
    const Summary v = summarize(vblankIntervals);
    std::printf("vblank intervals: n %zu p50 %.3f min %.3f max %.3f ms\n", v.count, v.p50, v.min, v.max);
    std::printf("captures: %zu, %zu showing this window, %zu with a full-output dirty region, desktop format %d\n",
                frames.size(), ours, fullScreen, probe.desktopFormat());
    return 0;
}

int run(const Options& options) {
    const DisplayRequiredGuard awake;
    const SystemSessionState session;
    const TickRate rate = qpcRate();
    JsonWriter json;
    json.beginObject();
    json.field("tool", "s1_probe_validate");

    const SessionVerdict before = precheck(session);
    if (!before.valid) {
        json.field("status", "invalid");
        json.key("sessionStart");
        writeVerdict(json, before);
        json.endObject();
        writeReport(options, json.str());
        std::printf("s1_probe_validate: INVALID session: %s\n", before.problems.front().c_str());
        return 3;
    }

    MarkerWindow window;
    if (!window.create()) {
        std::printf("s1_probe_validate: cannot create the test window (error %lu)\n", GetLastError());
        return 2;
    }
    window.paint(0);
    settle(400.0);

    const POINT marker = window.markerOrigin();
    DuplicationOptions probeOptions;
    probeOptions.watch = window.screenRect();
    probeOptions.marker = ScreenRect{marker.x, marker.y, marker.x, marker.y};
    probeOptions.markerCellPx = kCellPx;
    probeOptions.outputX = marker.x;
    probeOptions.outputY = marker.y;
    probeOptions.keepAllFrames = options.diagnose;
    probeOptions.keepDirtyRects = options.diagnose;
    auto probe = DuplicationProbe::open(probeOptions);
    if (!probe) {
        std::printf("s1_probe_validate: %s\n", probe.error().describe().c_str());
        return 2;
    }
    auto clock = VBlankClock::forPoint(marker.x, marker.y);
    const auto dwm = readDwmCounters();
    if (!clock || !dwm) {
        std::printf("s1_probe_validate: vblank clock or DWM timing unavailable\n");
        return 2;
    }
    const std::int64_t period = dwm->refreshPeriodTicks;
    const double periodMs = rate.toMs(period);
    (*probe)->start();
    settle(100.0);
    if (options.diagnose) {
        return diagnose(window, **probe, *clock, rate);
    }

    // Phase A: injected delays, interleaved so drift affects every delay alike.
    std::vector<DelayTrial> trials;
    std::uint16_t seq = 1;
    std::array<double, kDelaysMs.size()> phase{0.0, 0.33, 0.67};
    const std::int64_t phaseAStart = qpcNow();
    for (int i = 0; i < options.trials; ++i) {
        for (std::size_t d = 0; d < kDelaysMs.size(); ++d) {
            MarkerWindow::pump();
            // Golden-ratio steps spread each delay's trigger phases evenly over one period.
            phase[d] = std::fmod(phase[d] + 0.6180339887498949, 1.0);
            auto vblank = clock->wait();
            if (!vblank) {
                break;
            }
            spinUntil(*vblank + static_cast<std::int64_t>(phase[d] * static_cast<double>(period)));
            DelayTrial trial;
            trial.injectedMs = kDelaysMs[d];
            trial.triggerTicks = qpcNow();
            spinUntil(trial.triggerTicks + rate.fromMs(kDelaysMs[d]));
            window.paint(seq);
            trial.drawnTicks = qpcNow();
            if (auto seen = (*probe)->waitForMarker(seq, trial.triggerTicks, std::chrono::milliseconds(250))) {
                trial.screenTicks = seen->update.screenTicks();
            }
            trials.push_back(trial);
            ++seq;
        }
    }
    const std::int64_t phaseAEnd = qpcNow();
    const ValidationReport latency = evaluateDelayTrials(trials, periodMs, rate);

    // Phase B: one frame per vblank with injected stalls.
    settle(50.0);
    std::vector<AppFrame> frames;
    std::vector<StallProbe> stalls;
    std::vector<std::uint32_t> stallAffected; // Stalled frames and their successors.
    std::vector<double> vblankIntervals;
    const std::uint32_t firstSeq = seq;
    const std::int64_t phaseBStart = qpcNow();
    std::int64_t lastVBlank = 0;
    for (int frame = 0; frame < kSteadyFrames; ++frame) {
        MarkerWindow::pump();
        auto vblank = clock->wait();
        if (!vblank) {
            break;
        }
        if (lastVBlank != 0) {
            vblankIntervals.push_back(rate.toMs(*vblank - lastVBlank));
        }
        lastVBlank = *vblank;
        const auto frameSeq = firstSeq + static_cast<std::uint32_t>(frame);
        if (frame % kStallEvery == kStallEvery / 2) {
            const double stallMs = kStallMs[static_cast<std::size_t>(frame / kStallEvery) % kStallMs.size()];
            stalls.push_back(StallProbe{frameSeq, stallMs, std::nullopt});
            stallAffected.push_back(frameSeq);
            stallAffected.push_back(frameSeq + 1);
            spinUntil(*vblank + rate.fromMs(stallMs));
        }
        window.paint(static_cast<std::uint16_t>(frameSeq));
        frames.push_back(AppFrame{frameSeq, qpcNow(), *vblank});
    }
    const std::int64_t phaseBEnd = qpcNow();
    settle(100.0);
    (*probe)->stop();
    const SessionVerdict after = precheck(session);
    const std::uint32_t accessLost = (*probe)->accessLost();

    std::vector<ScreenUpdate> captured;
    for (const CaptureFrame& frame : (*probe)->frames()) {
        captured.push_back(frame.update);
    }
    const std::vector<ScreenUpdate> updates = collapseToDisplay(captured, period);
    const auto matches = matchFrames(frames, updates, rate.fromMs(0.5));
    const LatencyStats steadyLatency = summariseLatency(frames, matches, rate);
    measureStalls(stalls, matches, period, rate);
    std::vector<std::string> problems = latency.problems;
    checkStalls(stalls, periodMs, 0.5, problems, kMinStallsMeasured);
    const auto stallSummaries = summariseStalls(stalls);
    for (const StallSummary& summary : stallSummaries) {
        // B6 detection: 25 ms stalls must exceed the 16.7 ms content-interval limit.
        if (summary.stallMs > 16.7 && summary.measured > 0 && summary.intervalMs.min <= 16.7) {
            problems.push_back(std::format("a {} ms stall was measured at {:.3f} ms, not above 16.7 ms",
                                           summary.stallMs, summary.intervalMs.min));
        }
    }
    const CadenceCheck cadence = checkCadence(matches, stallAffected, period);
    if (cadence.consistentFraction < kMinCadenceConsistency) {
        problems.push_back(std::format("cadence: only {} of {} screen intervals matched their frame step",
                                       cadence.consistent, cadence.intervals));
    }

    // Phase B content statistics: the environment floor for any one-frame-per-vblank window.
    std::vector<ScreenUpdate> steadyUpdates;
    for (const ScreenUpdate& update : updates) {
        if (update.marker && *update.marker >= static_cast<std::uint16_t>(firstSeq)) {
            steadyUpdates.push_back(update);
        }
    }
    const ContentStats steady = analyseContent(steadyUpdates, phaseBStart, phaseBEnd + rate.fromMs(50.0), period, rate);
    // WaitForVBlank wake-ups against DWM's grid: the phase must be small and stable
    // for the vblank-quantised screen times to be on the real refresh grid.
    std::vector<double> wakePhases;
    if (const auto grid = readDwmCounters(); grid && grid->vblankTicks > 0) {
        for (const AppFrame& frame : frames) {
            const std::int64_t at = nextVBlank(frame.inputTicks, grid->vblankTicks, period);
            double phaseMs = rate.toMs(frame.inputTicks - (at - period));
            if (phaseMs >= periodMs / 2.0) {
                phaseMs -= periodMs; // Woke just before the grid vblank.
            }
            wakePhases.push_back(phaseMs);
        }
    }
    const Summary wake = summarize(wakePhases);
    const Summary vblanks = summarize(vblankIntervals);
    if (std::abs(vblanks.p50 - periodMs) > 0.05) {
        problems.push_back(std::format("DWM's refresh period {:.3f} ms differs from the median vblank interval "
                                       "{:.3f} ms",
                                       periodMs, vblanks.p50));
    }
    if (accessLost > 0) {
        problems.push_back(std::format("desktop duplication lost access {} times", accessLost));
    }
    if (auto failure = (*probe)->failure()) {
        problems.push_back(*failure);
    }

    const bool invalid = !after.valid || accessLost > 0;
    const char* status = invalid ? "invalid" : (problems.empty() ? "pass" : "fail");
    json.field("status", status);
    json.key("sessionStart");
    writeVerdict(json, before);
    json.key("sessionEnd");
    writeVerdict(json, after);
    json.key("output").beginObject();
    json.field("device", std::string_view((*probe)->output().deviceName));
    json.field("width", (*probe)->output().desktop.width());
    json.field("height", (*probe)->output().desktop.height());
    json.field("desktopFormat", (*probe)->desktopFormat());
    json.endObject();
    json.field("refreshPeriodMs", periodMs);
    json.key("vblankIntervalMs").value(vblanks);
    json.key("vblankWakePhaseMs").value(wake);
    json.field("phaseASeconds", rate.toMs(phaseAEnd - phaseAStart) / 1000.0);
    json.field("phaseBSeconds", rate.toMs(phaseBEnd - phaseBStart) / 1000.0);
    json.key("delays").beginArray();
    for (const DelayResult& result : latency.delays) {
        writeDelay(json, result);
    }
    json.endArray();
    json.key("stalls").beginArray();
    for (const StallSummary& summary : stallSummaries) {
        json.beginObject();
        json.field("stallMs", summary.stallMs);
        json.field("injected", static_cast<std::uint64_t>(summary.injected));
        json.field("measured", static_cast<std::uint64_t>(summary.measured));
        json.key("intervalMs").value(summary.intervalMs);
        json.endObject();
    }
    json.endArray();
    json.key("cadence").beginObject();
    json.field("intervals", static_cast<std::uint64_t>(cadence.intervals));
    json.field("consistent", static_cast<std::uint64_t>(cadence.consistent));
    json.field("consistentFraction", cadence.consistentFraction);
    json.field("droppedFrames", static_cast<std::uint64_t>(cadence.dropped));
    json.endObject();
    json.key("environmentFloor");
    writeContent(json, steady);
    json.key("steadyLatency");
    writeLatency(json, steadyLatency);
    json.key("problems");
    writeStrings(json, problems);
    json.endObject();
    writeReport(options, json.str());

    std::printf("s1_probe_validate: %s (period %.3f ms, vblank p50 %.3f ms, wake phase vs DWM grid p50 %.3f "
                "[%.3f, %.3f] ms)\n",
                status, periodMs, vblanks.p50, wake.p50, wake.min, wake.max);
    for (const DelayResult& result : latency.delays) {
        std::printf("  D=%4.1f ms  latency p50 %6.2f  mean %6.2f  shift %6.2f (error %+5.2f)  residual %5.2f..%5.2f  "
                    "lost %zu/%zu\n",
                    result.injectedMs, result.latencyMs.p50, result.latencyMs.mean, result.meanShiftMs,
                    result.shiftErrorMs, result.residualMs.min, result.residualMs.max, result.lost, result.trials);
    }
    for (const StallSummary& summary : stallSummaries) {
        std::printf("  stall %4.1f ms: %zu/%zu measured, interval median %6.2f (min %6.2f, max %6.2f) ms\n",
                    summary.stallMs, summary.measured, summary.injected, summary.intervalMs.p50, summary.intervalMs.min,
                    summary.intervalMs.max);
    }
    std::printf("  cadence: %zu/%zu intervals consistent, %zu frames dropped by the desktop\n", cadence.consistent,
                cadence.intervals, cadence.dropped);
    std::printf("  environment floor: %zu updates, %zu refreshes, missed %.1f %%, max interval %.2f ms\n",
                steady.updates, steady.refreshes, 100.0 * steady.missedFraction, steady.maxIntervalMs);
    for (const std::string& problem : problems) {
        std::printf("  problem: %s\n", problem.c_str());
    }
    if (invalid) {
        return 3;
    }
    return problems.empty() ? 0 : 1;
}

} // namespace

int main() { return run(parseOptions()); }
