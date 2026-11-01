// SPDX-License-Identifier: Apache-2.0
//
// s1_first_frame: B1 (first frame, empty workspace) for frost.exe, measured on
// screen (ADR 0007). The same harness measures s1_editor --first-frame, whose
// window has frost.exe's size, to give each editor backend's own B1.
//
//   s1_first_frame --exe <absolute path to frost.exe> [--args "<child arguments>"]
//                  [--mode warm|cold] [--runs N] [--out report.json]
//
// Each launch starts the target through CreateProcessW (child_process.hpp; --args
// is passed verbatim, already quoted) with its first window placed by
// STARTF_USEPOSITION in the middle of the primary work area and shown without
// activation, over a solid topmost backdrop window of our own (so a video or
// animated wallpaper behind it cannot fake a change). A window
// shown without activation by a background process opens below the foreground
// window, where it is not on screen; so a WinEvent hook (EVENT_OBJECT_CREATE, out
// of context) makes each new top-level window of the child topmost as soon as it
// exists, normally before Qt shows it (the report says when it was too late). The Desktop
// Duplication probe reads five sample pixels inside the expected client area from
// every desktop present:
//   first frame   the first present after process start in which at least three
//                 samples moved more than 24 levels away from the backdrop colour.
//                 During DWM's fade-in that is not the window's first composed
//                 frame: in six diagnosed launches (ADR 0007) it was the third,
//                 12.1 to 18.5 ms after the first present that differed at all, so
//                 the endpoint is late by two or three refreshes;
//   settled       the first present in which at least four samples are within 6
//                 levels of the colour the window shows once idle;
//   window seen   when EnumWindows first returned a visible, uncloaked window of
//                 the process (polled between Sleep(1) calls, so every 1 to 16 ms
//                 depending on the system timer resolution; context only).
// The window is closed with WM_CLOSE 700 ms after it appeared. Times are from the
// QPC reading just before CreateProcessW to the present's display vblank.
//
// --mode warm launches the build-tree target once to prime the file cache and
// then N times. --mode cold copies the target and its DLLs into a fresh directory
// next to it with unbuffered I/O (COPY_FILE_NO_BUFFERING) before each launch, so
// the copies have no prefetch trace and the copy itself does not leave them in
// the file cache. That they are read from disk at launch is not verified:
// Defender may scan the new files when they are closed, which reads them into the
// cache. System DLLs stay cached. A true post-reboot cold start is not possible
// from an agent session (no reboot, and a standby-list purge needs elevation), so
// "cold" here is that proxy, with a bias of unknown direction. Exit codes:
// 0 valid, 2 setup error, 3 invalid run (session, access lost or capture failure;
// the report says which).

#include "frost/perf/frame_analysis.hpp"
#include "frost/perf/json_writer.hpp"
#include "frost/perf/report.hpp"
#include "frost/perf/win/screen_probes.hpp"
#include "frost/perf/win/system_session_state.hpp"
#include "frost/perf/win/timing.hpp"

#include "child_process.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>

using namespace frost::perf;
using namespace frost::perf::win;

namespace {

constexpr int kChangeLevels = 24;
constexpr int kSettledLevels = 6;
constexpr double kSettleMs = 700.0;
constexpr double kWindowTimeoutMs = 8000.0;
// QMainWindow::resize(1280, 800) in src/app/src/main_window.cpp, plus the frame.
constexpr int kClientWidth = 1280;
constexpr int kClientHeight = 800;
constexpr COLORREF kBackdrop = RGB(0x20, 0x60, 0x20); // Far from light and dark Qt palettes.

// Dispatches this thread's window messages for `ms` milliseconds (the backdrop
// repaints where frost.exe's window was).
void pumpFor(double ms) {
    const TickRate rate = qpcRate();
    const std::int64_t deadline = qpcNow() + rate.fromMs(ms);
    while (qpcNow() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    }
}

// A solid, non-activating popup behind the place frost.exe's window opens.
class Backdrop {
public:
    explicit Backdrop(const ScreenRect& rect) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hbrBackground = CreateSolidBrush(kBackdrop);
        windowClass.lpszClassName = L"FrostS1Backdrop";
        RegisterClassExW(&windowClass);
        m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, windowClass.lpszClassName,
                                 L"S1 backdrop", WS_POPUP, rect.left, rect.top, rect.width(), rect.height(), nullptr,
                                 nullptr, windowClass.hInstance, nullptr);
        if (m_hwnd != nullptr) {
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            UpdateWindow(m_hwnd);
        }
    }
    ~Backdrop() {
        if (m_hwnd != nullptr) {
            DestroyWindow(m_hwnd);
        }
    }
    Backdrop(const Backdrop&) = delete ("owns a window");
    Backdrop& operator=(const Backdrop&) = delete ("owns a window");
    [[nodiscard]] bool ok() const noexcept { return m_hwnd != nullptr; }

private:
    HWND m_hwnd = nullptr;
};

struct Options {
    std::wstring exe;
    std::wstring arguments; // --args, verbatim.
    bool cold = false;
    int runs = 8;
    std::wstring out;
};

Options parseOptions() {
    Options options;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; argv != nullptr && i < count; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--exe" && i + 1 < count) {
            options.exe = argv[++i];
        } else if (arg == L"--args" && i + 1 < count) {
            options.arguments = argv[++i];
        } else if (arg == L"--mode" && i + 1 < count) {
            options.cold = std::wstring(argv[++i]) == L"cold";
        } else if (arg == L"--runs" && i + 1 < count) {
            options.runs = std::clamp(_wtoi(argv[++i]), 1, 12); // Keeps one invocation under 20 s on screen.
        } else if (arg == L"--out" && i + 1 < count) {
            options.out = argv[++i];
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

int channelDistance(std::uint32_t a, std::uint32_t b) {
    int distance = 0;
    for (const unsigned shift : {0U, 8U, 16U}) {
        distance = std::max(distance,
                            std::abs(static_cast<int>((a >> shift) & 0xFFU) - static_cast<int>((b >> shift) & 0xFFU)));
    }
    return distance;
}

struct FoundWindow {
    HWND hwnd = nullptr;
    ScreenRect bounds;
};

// A visible, uncloaked top-level window of `pid`, if there is one.
std::optional<FoundWindow> findWindow(DWORD pid) {
    struct Search {
        DWORD pid;
        std::optional<FoundWindow> found;
    } search{pid, std::nullopt};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(param);
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner != s.pid || IsWindowVisible(hwnd) == 0 || GetWindow(hwnd, GW_OWNER) != nullptr) {
                return TRUE;
            }
            DWORD cloaked = 0;
            DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (cloaked != 0) {
                return TRUE;
            }
            RECT rect{};
            if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
                GetWindowRect(hwnd, &rect);
            }
            s.found = FoundWindow{hwnd, ScreenRect{rect.left, rect.top, rect.right, rect.bottom}};
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// Copies frost.exe's directory (executables and DLLs, one level of plugin
// folders) to `target` with unbuffered I/O, so the copies are not in the cache.
bool copyUncached(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code error;
    std::filesystem::create_directories(target, error);
    const auto copyFile = [](const std::filesystem::path& from, const std::filesystem::path& to) {
        return CopyFileExW(from.c_str(), to.c_str(), nullptr, nullptr, nullptr, COPY_FILE_NO_BUFFERING) != 0;
    };
    for (const auto& entry : std::filesystem::directory_iterator(source, error)) {
        const auto& path = entry.path();
        if (entry.is_directory()) {
            std::filesystem::create_directories(target / path.filename(), error);
            for (const auto& nested : std::filesystem::directory_iterator(path, error)) {
                if (nested.path().extension() == L".dll" &&
                    !copyFile(nested.path(), target / path.filename() / nested.path().filename())) {
                    return false;
                }
            }
        } else if ((path.extension() == L".exe" || path.extension() == L".dll") &&
                   !copyFile(path, target / path.filename())) {
            return false;
        }
    }
    return !error;
}

struct LaunchResult {
    bool cold = false;
    bool priming = false;
    bool valid = false;
    std::string problem;
    double windowSeenMs = 0;
    std::optional<double> firstFrameMs;
    std::optional<double> settledMs;
    ScreenRect bounds;
    std::uint32_t firstColour = 0; // Sample 2 (centre) in the first frame, 0xRRGGBB.
    std::uint32_t idleColour = 0;  // Sample 2 once idle.
    bool raised = false;           // The hook made the window topmost...
    bool raisedBeforeShow = false; // ...while it was still hidden.
};

// Raises every new top-level window of one process to the topmost band (see the
// header comment). Out-of-context hooks run on this thread while it pumps messages.
class TopmostHook {
public:
    explicit TopmostHook(DWORD pid) {
        s_pid = pid;
        s_raised = false;
        s_beforeShow = false;
        m_hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, &TopmostHook::onEvent, pid, 0,
                                 WINEVENT_OUTOFCONTEXT);
    }
    ~TopmostHook() {
        if (m_hook != nullptr) {
            UnhookWinEvent(m_hook);
        }
        s_pid = 0;
    }
    TopmostHook(const TopmostHook&) = delete ("owns a hook");
    TopmostHook& operator=(const TopmostHook&) = delete ("owns a hook");
    [[nodiscard]] static bool raised() noexcept { return s_raised; }
    [[nodiscard]] static bool raisedBeforeShow() noexcept { return s_beforeShow; }

private:
    static void CALLBACK onEvent(HWINEVENTHOOK /*hook*/, DWORD /*event*/, HWND hwnd, LONG object, LONG child,
                                 DWORD /*thread*/, DWORD /*time*/) {
        if (object != OBJID_WINDOW || child != CHILDID_SELF || hwnd == nullptr ||
            GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow()) {
            return; // Child, message-only or non-window objects.
        }
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner != s_pid || (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
            return;
        }
        const bool hidden = IsWindowVisible(hwnd) == 0;
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        if (!s_raised) {
            s_raised = true;
            s_beforeShow = hidden;
        }
    }

    static inline DWORD s_pid = 0;
    static inline bool s_raised = false;
    static inline bool s_beforeShow = false;
    HWINEVENTHOOK m_hook = nullptr;
};

class Runner {
public:
    Runner(DuplicationProbe& probe, std::vector<ScreenPoint> samples, std::pair<int, int> position,
           std::wstring arguments)
        : m_probe(probe), m_samples(std::move(samples)), m_position(position), m_arguments(std::move(arguments)) {}

    LaunchResult launch(const std::filesystem::path& exe, bool cold, bool priming) {
        LaunchResult result;
        result.cold = cold;
        result.priming = priming;
        const TickRate rate = qpcRate();
        frost::s1::LaunchOptions request;
        request.executable = exe.wstring();
        request.arguments = m_arguments;
        request.windowPosition = m_position;
        auto child = frost::s1::ChildProcess::launch(request);
        if (!child) {
            result.problem = child.error().message();
            return result;
        }
        const TopmostHook hook(child->pid());
        const std::int64_t start = child->startTicks();
        std::optional<FoundWindow> window;
        while (!window && rate.toMs(qpcNow() - start) < kWindowTimeoutMs) {
            window = findWindow(child->pid());
            if (!window) {
                pumpFor(2.0);
            }
        }
        if (!window) {
            child->terminate();
            result.problem = "no visible window";
            return result;
        }
        result.windowSeenMs = rate.toMs(qpcNow() - start);
        pumpFor(kSettleMs);
        // Bounds after settling (the first reading can precede the final placement).
        if (const auto settled = findWindow(child->pid())) {
            window = settled;
        }
        result.bounds = window->bounds;
        result.raised = TopmostHook::raised();
        result.raisedBeforeShow = TopmostHook::raisedBeforeShow();
        PostMessageW(window->hwnd, WM_CLOSE, 0, 0);
        if (!child->wait(3000)) {
            child->terminate();
        }
        analyse(start, result);
        return result;
    }

private:
    void analyse(std::int64_t start, LaunchResult& result) const {
        const TickRate rate = qpcRate();
        for (const ScreenPoint& point : m_samples) {
            if (!result.bounds.contains(ScreenRect{point.x, point.y, point.x + 1, point.y + 1})) {
                result.problem = "the window did not open at the requested position";
                return;
            }
        }
        const std::vector<CaptureFrame> frames = m_probe.frames();
        if (GetEnvironmentVariableW(L"FROST_S1_DIAGNOSE", nullptr, 0) != 0) {
            for (const CaptureFrame& frame : frames) {
                const double at = rate.toMs(frame.update.presentTicks - start);
                if (at > -100.0 && at < 1500.0 && frame.samples.size() == m_samples.size()) {
                    std::printf("    %+8.1f ms acc %u  %06x %06x %06x %06x %06x\n", at, frame.update.accumulatedFrames,
                                frame.samples[0], frame.samples[1], frame.samples[2], frame.samples[3],
                                frame.samples[4]);
                }
            }
        }
        // The desktop image at process start: the last present before it.
        const CaptureFrame* before = nullptr;
        const CaptureFrame* last = nullptr;
        for (const CaptureFrame& frame : frames) {
            if (frame.samples.size() != m_samples.size()) {
                continue;
            }
            if (frame.update.presentTicks <= start) {
                before = &frame;
            }
            last = &frame;
        }
        if (before == nullptr || last == nullptr) {
            result.problem = "no desktop image before the launch";
            return;
        }
        // The idle window's colours: the last present before WM_CLOSE took effect,
        // i.e. the newest frame whose samples were not back to the pre-launch colours
        // and not yet the closing animation. Take the frame closest to settle time.
        const std::int64_t settleTicks = start + rate.fromMs(result.windowSeenMs + kSettleMs - 50.0);
        const CaptureFrame* idle = before;
        for (const CaptureFrame& frame : frames) {
            if (frame.samples.size() == m_samples.size() && frame.update.presentTicks <= settleTicks) {
                idle = &frame;
            }
        }
        std::size_t moved = 0;
        for (std::size_t i = 0; i < m_samples.size(); ++i) {
            moved += channelDistance(idle->samples[i], before->samples[i]) > kChangeLevels ? 1 : 0;
        }
        if (moved < 3) {
            result.problem = "the window colours match the desktop behind it";
            return;
        }
        result.idleColour = idle->samples[2];
        for (const CaptureFrame& frame : frames) {
            if (frame.samples.size() != m_samples.size() || frame.update.presentTicks <= start) {
                continue;
            }
            std::size_t changed = 0;
            std::size_t settled = 0;
            for (std::size_t i = 0; i < m_samples.size(); ++i) {
                changed += channelDistance(frame.samples[i], before->samples[i]) > kChangeLevels ? 1 : 0;
                settled += channelDistance(frame.samples[i], idle->samples[i]) <= kSettledLevels ? 1 : 0;
            }
            const double ms = rate.toMs(frame.update.screenTicks() - start);
            if (!result.firstFrameMs && changed >= 3) {
                result.firstFrameMs = ms;
                result.firstColour = frame.samples[2];
            }
            if (!result.settledMs && settled >= 4 && changed >= 3) {
                result.settledMs = ms;
                break;
            }
        }
        result.valid = result.firstFrameMs.has_value();
        if (!result.valid) {
            result.problem = "no present showed the window";
        }
    }

    DuplicationProbe& m_probe;
    std::vector<ScreenPoint> m_samples;
    std::pair<int, int> m_position;
    std::wstring m_arguments;
};

int run(const Options& options) {
    const DisplayRequiredGuard awake;
    const SystemSessionState session;
    JsonWriter json;
    json.beginObject();
    json.field("tool", "s1_first_frame");
    json.field("mode", options.cold ? "cold-proxy" : "warm");
    const auto utf8 = [](const std::wstring& text) {
        const std::u8string bytes = std::filesystem::path(text).u8string(); // UTF-16 to UTF-8, no code page.
        return std::string(bytes.begin(), bytes.end());
    };
    json.field("target", utf8(std::filesystem::path(options.exe).filename().wstring()));
    json.field("arguments", utf8(options.arguments));
    const auto fail = [&](const char* status, const std::string& reason, int code) {
        json.field("status", status);
        json.field("reason", reason);
        json.endObject();
        writeReport(options, json.str());
        std::printf("s1_first_frame: %s: %s\n", status, reason.c_str());
        return code;
    };

    const std::filesystem::path exe(options.exe);
    if (!exe.is_absolute() || !std::filesystem::exists(exe)) {
        return fail("setup-error", "--exe must name an existing absolute path", 2);
    }
    const SessionVerdict before = precheck(session);
    if (!before.valid) {
        json.key("sessionStart");
        writeVerdict(json, before);
        return fail("invalid", before.problems.front(), 3);
    }

    // The middle of the primary work area; samples inside the expected client area.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - (kClientWidth + 16)) / 2;
    const int y = work.top + ((work.bottom - work.top) - (kClientHeight + 40)) / 2;
    const std::vector<ScreenPoint> samples{
        {x + 330, y + 300}, {x + 970, y + 300}, {x + 650, y + 450}, {x + 330, y + 620}, {x + 970, y + 620}};

    DuplicationOptions capture;
    capture.keepAllFrames = true;
    capture.samples = samples;
    capture.outputX = x + 650;
    capture.outputY = y + 450;
    auto probe = DuplicationProbe::open(capture);
    if (!probe) {
        return fail("setup-error", probe.error().message(), 2);
    }
    const Backdrop backdrop(ScreenRect{x - 60, y - 60, x + kClientWidth + 76, y + kClientHeight + 100});
    if (!backdrop.ok()) {
        return fail("setup-error", "the backdrop window could not be created", 2);
    }
    (*probe)->start();
    pumpFor(400.0); // The backdrop is drawn and captured.

    Runner runner(**probe, samples, {x, y}, options.arguments);
    std::vector<LaunchResult> results;
    const std::filesystem::path coldRoot = exe.parent_path().parent_path() / L"s1-cold";
    if (!options.cold) {
        results.push_back(runner.launch(exe, false, true));
    }
    for (int i = 0; i < options.runs; ++i) {
        pumpFor(400.0); // The backdrop repaints where the last window was.
        if (!options.cold) {
            results.push_back(runner.launch(exe, false, false));
            continue;
        }
        const std::filesystem::path copy =
            coldRoot / std::to_wstring(GetCurrentProcessId() * 100U + static_cast<unsigned>(i));
        if (!copyUncached(exe.parent_path(), copy)) {
            LaunchResult failed;
            failed.cold = true;
            failed.problem = "copying the binaries failed";
            results.push_back(failed);
            continue;
        }
        pumpFor(200.0);
        results.push_back(runner.launch(copy / exe.filename(), true, false));
        std::error_code ignored;
        std::filesystem::remove_all(copy, ignored);
    }
    pumpFor(200.0);
    (*probe)->stop();
    const std::uint32_t lost = (*probe)->accessLost();
    const std::optional<std::string> captureFailure = (*probe)->failure();
    const SessionVerdict after = precheck(session);
    std::error_code ignored;
    std::filesystem::remove(coldRoot, ignored); // Only when empty.
    std::string reason;
    if (!after.valid) {
        reason = after.problems.empty() ? std::string("the session became invalid") : after.problems.front();
    } else if (lost != 0) {
        reason = "Desktop Duplication access was lost";
    } else if (captureFailure) {
        reason = "capture failed: " + *captureFailure;
    }
    const bool valid = reason.empty();

    std::vector<double> firstFrames;
    std::vector<double> settled;
    std::vector<double> seen;
    json.field("status", valid ? "valid" : "invalid");
    json.field("reason", reason);
    json.field("captureFailure", captureFailure.value_or(std::string()));
    json.key("sessionStart");
    writeVerdict(json, before);
    json.key("sessionEnd");
    writeVerdict(json, after);
    json.field("accessLost", static_cast<std::uint64_t>(lost));
    json.key("launches").beginArray();
    for (const LaunchResult& r : results) {
        json.beginObject();
        json.field("priming", r.priming).field("valid", r.valid).field("problem", r.problem);
        json.field("windowSeenMs", r.windowSeenMs);
        json.field("raised", r.raised).field("raisedBeforeShow", r.raisedBeforeShow);
        json.key("firstFrameMs");
        r.firstFrameMs ? json.value(*r.firstFrameMs) : json.null();
        json.key("settledMs");
        r.settledMs ? json.value(*r.settledMs) : json.null();
        json.field("firstColour", std::format("#{:06x}", r.firstColour));
        json.field("idleColour", std::format("#{:06x}", r.idleColour));
        json.key("bounds").beginArray().value(r.bounds.left).value(r.bounds.top).value(r.bounds.right);
        json.value(r.bounds.bottom).endArray();
        json.endObject();
        if (r.valid && !r.priming) {
            firstFrames.push_back(*r.firstFrameMs);
            seen.push_back(r.windowSeenMs);
            if (r.settledMs) {
                settled.push_back(*r.settledMs);
            }
        }
    }
    json.endArray();
    const Summary first = summarize(firstFrames);
    json.key("firstFrameMs").value(first);
    json.key("settledMs").value(summarize(settled));
    json.key("windowSeenMs").value(summarize(seen));
    json.endObject();
    writeReport(options, json.str());

    std::printf("s1_first_frame %s: %zu valid of %d; first frame p50 %.1f p99 %.1f max %.1f ms; settled p50 %.1f ms; "
                "window seen p50 %.1f ms\n",
                options.cold ? "cold-proxy" : "warm", firstFrames.size(), options.runs, first.p50, first.p99, first.max,
                summarize(settled).p50, summarize(seen).p50);
    for (const LaunchResult& r : results) {
        if (!r.valid) {
            std::printf("  launch problem: %s\n", r.problem.c_str());
        }
    }
    if (!valid) {
        std::printf("  INVALID: %s\n", reason.c_str());
    }
    return valid ? 0 : 3;
}

} // namespace

int main() { return run(parseOptions()); }
