// SPDX-License-Identifier: Apache-2.0
//
// s1_editor: one S1 run of one editor backend (ADR 0007).
//
//   s1_editor --backend raster|rhi [--content text|fill] [--scroll-blit] [--seconds 10]
//             [--keys 50] [--size WxH] [--full-output] [--inject-cpu-ms MS] [--out report.json]
//   s1_editor --backend raster|rhi --validate-delays [--trials 48] [--size WxH] [--full-output]
//             [--out report.json]
//   s1_editor --backend raster|rhi --first-frame [--content text|fill] [--out timings.jsonl]
//
// The fling run opens a frameless, topmost window over the work area of the
// primary screen (5120 x 1392 on REF-W1, not exclusive fullscreen, shown without
// activation so it never takes focus), then:
//   fling   for --seconds, a vblank thread (IDXGIOutput::WaitForVBlank) posts one
//           tick per refresh; the GUI thread advances the scroll along the fling
//           curve and repaints. UI CPU per frame is the GUI thread's cycle time
//           (QueryThreadCycleTime) from one tick to the next, so it includes work
//           Qt defers to the event loop (QRhiWidget renders and presents on the
//           window's next update, not inside repaint()). It does not include
//           Direct3D or driver worker threads;
//   typing  --keys synthetic key presses, 23 ms apart (not vblank-aligned), each
//           inserting a character and repainting the dirty line (raster) or the
//           frame (RHI); B5 is input to paint end, with flush end and screen too;
//   probes  the Desktop Duplication probe watches the window and decodes the
//           frame marker at its top-left corner, giving content intervals and
//           missed refreshes (B6) and frame-to-screen latency.
// --content fill paints only the background and the marker. --scroll-blit
// (raster) moves the backing store and paints only the exposed strip, as a
// shipping raster editor would. --size runs in a smaller window at the work
// area's top-left, to see which costs scale with the window area. --full-output
// covers the whole output instead of the work area (5120 x 1440, over the
// taskbar), where DWM may flip a borderless flip-model swap chain directly
// instead of composing it; whether it did is not observed (no PresentMon). A 10 s
// fling is on screen for about 13 s; the window is closed before analysis.
//
// Probe validation for both backends (s1_probe_validate covers a plain GDI window):
//   --inject-cpu-ms MS  every other fling tick spins MS on the GUI thread once its
//                       frame is ready. The median tick-to-tick CPU of intervals
//                       with the spin must exceed the others' by MS within 10 %.
//                       Such a run reports no B6 CPU verdict.
//   --validate-delays   s1_probe_validate's phase A through this backend's present
//                       path (for RHI, Qt's flip-model swap chain): per trial, wait
//                       for a vblank and a phase spread over the period, then paint
//                       a new marker while the paint (paintEvent or
//                       QRhiWidget::render) spins D in {0, 8, 25} ms after drawing.
//                       The mean screen latency must shift by D within 1.5 ms, with
//                       no trial lost or outside its causal bound. About 10 s.
// --first-frame is the target s1_first_frame launches to measure this backend's
// B1: a normal 1280 x 800 window like frost.exe's main window, placed and shown
// by the launcher, drawing a 2,000-line buffer once and idling until it is closed
// (10 s at most). With --out it appends one JSON line of in-process timings.
//
// Exit codes: 0 the run is valid (pass or fail of the budgets is in the report),
// 1 a validation mode failed, 2 setup error, 3 invalid run (locked, monitor off,
// access lost, capture failure, or too few markers decoded).

#include "frost/perf/frame_analysis.hpp"
#include "frost/perf/frame_marker.hpp"
#include "frost/perf/json_writer.hpp"
#include "frost/perf/probe_validation.hpp"
#include "frost/perf/report.hpp"
#include "frost/perf/win/screen_probes.hpp"
#include "frost/perf/win/system_session_state.hpp"
#include "frost/perf/win/timing.hpp"

#include "corpus.hpp"
#include "gates.hpp"
#include "views.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QScreen>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

using namespace frost::perf;
using namespace frost::perf::win;
using frost::s1::FrameView;
using frost::s1::ViewState;

namespace {

constexpr std::size_t kCorpusLines = 200'000;
// --first-frame draws a small buffer, so B1 measures the backend, not corpus generation.
constexpr std::size_t kFirstFrameLines = 2'000;
constexpr std::uint64_t kCorpusSeed = 0x51'2026;
constexpr int kFontPixels = 15;
constexpr int kKeyIntervalMs = 23;
// The UI-CPU probe must see an injected spin within 10 % (ADR 0007).
constexpr double kInjectTolerance = 0.10;
constexpr std::array<double, 3> kDelaysMs{0.0, 8.0, 25.0};
// frost.exe's main window: QMainWindow::resize(1280, 800) in src/app/src/main_window.cpp.
constexpr int kFirstFrameWidth = 1280;
constexpr int kFirstFrameHeight = 800;

struct Options {
    bool rhi = false;
    bool fillOnly = false;
    bool scrollBlit = false;
    double seconds = 10.0;
    int keys = 50;
    QSize size; // Empty: the whole work area (or output).
    bool fullOutput = false;
    double injectCpuMs = 0;
    bool validateDelays = false;
    int trials = 48; // Per injected delay.
    bool firstFrame = false;
    std::wstring out;
};

Options parseOptions(const QStringList& args) {
    Options options;
    for (qsizetype i = 1; i < args.size(); ++i) {
        const QString& arg = args[i];
        const bool hasValue = i + 1 < args.size();
        const auto is = [](const QString& text, const char* literal) { return text == QLatin1String(literal); };
        if (is(arg, "--backend") && hasValue) {
            options.rhi = is(args[++i], "rhi");
        } else if (is(arg, "--content") && hasValue) {
            options.fillOnly = is(args[++i], "fill");
        } else if (is(arg, "--scroll-blit")) {
            options.scrollBlit = true;
        } else if (is(arg, "--seconds") && hasValue) {
            options.seconds = std::clamp(args[++i].toDouble(), 1.0, 12.0); // Keeps a run under 20 s on screen.
        } else if (is(arg, "--keys") && hasValue) {
            options.keys = std::clamp(args[++i].toInt(), 0, 200);
        } else if (is(arg, "--size") && hasValue) {
            const QStringList parts = args[++i].split(QLatin1Char('x'));
            if (parts.size() == 2) {
                options.size = QSize(parts[0].toInt(), parts[1].toInt());
            }
        } else if (is(arg, "--full-output")) {
            options.fullOutput = true;
        } else if (is(arg, "--inject-cpu-ms") && hasValue) {
            options.injectCpuMs = std::clamp(args[++i].toDouble(), 0.0, 4.0);
        } else if (is(arg, "--validate-delays")) {
            options.validateDelays = true;
        } else if (is(arg, "--trials") && hasValue) {
            options.trials = std::clamp(args[++i].toInt(), 4, 64); // 64 x 3 trials stay under 15 s.
        } else if (is(arg, "--first-frame")) {
            options.firstFrame = true;
        } else if (is(arg, "--out") && hasValue) {
            options.out = args[++i].toStdWString();
        }
    }
    return options;
}

void writeReport(const Options& options, const std::string& json) {
    if (!options.out.empty()) {
        std::ofstream file(options.out, std::ios::binary);
        file << json << '\n';
    }
}

const char* verdictText(const std::optional<bool>& pass) {
    if (!pass) {
        return "not measured";
    }
    return *pass ? "pass" : "FAIL";
}

// Writes `name`: true, false or null (not measured).
void writeVerdictField(JsonWriter& json, const char* name, const std::optional<bool>& pass) {
    json.key(name);
    pass ? json.value(*pass) : json.null();
}

// Processes Qt events for `ms` milliseconds.
void pump(QApplication& app, int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        app.processEvents(QEventLoop::AllEvents, 5);
        Sleep(1);
    }
}

// The view's client area in desktop pixels.
ScreenRect clientRect(QWidget* widget) {
    const auto hwnd = reinterpret_cast<HWND>(widget->winId());
    RECT client{};
    GetClientRect(hwnd, &client);
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    return {origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
}

// The view the options ask for; `raster` or `rhi` points at it.
struct Views {
    std::unique_ptr<FrameView> view;
    frost::s1::RasterView* raster = nullptr;
    frost::s1::RhiView* rhi = nullptr;
};

Views makeView(const Options& options, const frost::s1::TextModel& model, const frost::s1::TextFont& font,
               const frost::s1::Theme& theme) {
    Views views;
    if (options.rhi) {
        auto created = std::make_unique<frost::s1::RhiView>(model, font, theme);
        views.rhi = created.get();
        views.view = std::move(created);
    } else {
        auto created = std::make_unique<frost::s1::RasterView>(model, font, theme);
        created->setScrollBlit(options.scrollBlit);
        views.raster = created.get();
        views.view = std::move(created);
    }
    return views;
}

std::uint64_t fileTimeTicks(const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
}

/* --first-frame: the B1 target. The launcher (s1_first_frame) places the first
   window (STARTF_USEPOSITION), shows it without activation, measures its first
   frame on screen and closes it with WM_CLOSE; the last window closing ends the
   event loop. The JSON line written with --out is in-process context only: from
   process creation to main, and from main to the first paint. */
int runFirstFrame(QApplication& app, const Options& options, std::int64_t mainTicks, const FILETIME& mainTime) {
    const TickRate rate = qpcRate();
    const std::int64_t appTicks = qpcNow();
    const frost::s1::TextFont font = frost::s1::TextFont::make(kFontPixels);
    const frost::s1::Theme theme;
    const frost::s1::TextModel model(frost::s1::makeCorpus(kFirstFrameLines, kCorpusSeed));
    const Views views = makeView(options, model, font, theme);
    QWidget* const window = views.view->widget();
    window->setWindowTitle(QStringLiteral("FrostIDE S1 first frame"));
    window->setAttribute(Qt::WA_ShowWithoutActivating);
    window->resize(kFirstFrameWidth, kFirstFrameHeight);
    ViewState state;
    state.fillOnly = options.fillOnly;
    state.marker = 1;
    views.view->setState(state);
    const std::int64_t createdTicks = qpcNow();
    window->show();
    const std::int64_t shownTicks = qpcNow();
    QTimer::singleShot(10'000, &app, &QCoreApplication::quit); // Never outlives a lost launcher.
    const int code = QApplication::exec();

    if (!options.out.empty()) {
        FILETIME creation{};
        FILETIME exited{};
        FILETIME kernel{};
        FILETIME user{};
        GetProcessTimes(GetCurrentProcess(), &creation, &exited, &kernel, &user);
        const auto sinceMain = [&](std::int64_t ticks) { return rate.toMs(ticks - mainTicks); };
        JsonWriter json;
        json.beginObject();
        json.field("tool", "s1_editor --first-frame");
        json.field("backend", options.rhi ? "rhi" : "raster");
        json.field("contentMode", options.fillOnly ? "fill" : "text");
        json.field("creationToMainMs",
                   static_cast<double>(fileTimeTicks(mainTime) - fileTimeTicks(creation)) / 10'000.0);
        json.field("mainToApplicationMs", sinceMain(appTicks));
        json.field("mainToViewCreatedMs", sinceMain(createdTicks));
        json.field("mainToShowReturnedMs", sinceMain(shownTicks));
        const auto& paints = views.view->paints();
        json.key("mainToFirstPaintBeginMs");
        paints.empty() ? json.null() : json.value(sinceMain(paints.front().beginTicks));
        json.key("mainToFirstPaintEndMs");
        paints.empty() ? json.null() : json.value(sinceMain(paints.front().endTicks));
        json.field("paints", static_cast<std::uint64_t>(paints.size()));
        if (views.rhi != nullptr) {
            json.field("pipelineMs", views.rhi->pipelineMs());
            json.field("rhiError", views.rhi->error().toStdString());
        }
        json.endObject();
        std::ofstream file(options.out, std::ios::binary | std::ios::app);
        file << json.str() << '\n';
    }
    return code;
}

// The newest paint, if it drew `marker`.
std::optional<frost::s1::PaintRecord> paintOf(const FrameView& view, std::uint16_t marker) {
    const auto& paints = view.paints();
    if (paints.empty() || paints.back().marker != marker) {
        return std::nullopt;
    }
    return paints.back();
}

// --validate-delays: the delay trials, interleaved so drift affects every delay
// alike. `seq` is the last marker used before and after.
std::vector<DelayTrial> runDelayTrials(FrameView& view, DuplicationProbe& probe, VBlankClock& clock,
                                       std::int64_t period, int trials, std::uint32_t& seq) {
    const TickRate rate = qpcRate();
    std::vector<DelayTrial> out;
    std::array<double, kDelaysMs.size()> phase{0.0, 0.33, 0.67};
    for (int i = 0; i < trials; ++i) {
        for (std::size_t d = 0; d < kDelaysMs.size(); ++d) {
            QCoreApplication::processEvents(); // Drains without waiting.
            // Golden-ratio steps spread each delay's trigger phases evenly over one period.
            phase[d] = std::fmod(phase[d] + 0.6180339887498949, 1.0);
            const auto vblank = clock.wait();
            if (!vblank) {
                return out;
            }
            spinUntil(*vblank + static_cast<std::int64_t>(phase[d] * static_cast<double>(period)));
            ViewState next = view.state();
            next.marker = static_cast<std::uint16_t>(++seq);
            next.injectDelayMs = kDelaysMs[d];
            DelayTrial trial;
            trial.injectedMs = kDelaysMs[d];
            trial.triggerTicks = qpcNow();
            view.setState(next);
            view.repaintNow({});
            const std::int64_t deadline = trial.triggerTicks + rate.fromMs(250.0);
            // QRhiWidget renders on the window's next update, not inside repaint().
            std::optional<frost::s1::PaintRecord> painted = paintOf(view, next.marker);
            while (!painted && qpcNow() < deadline) {
                QCoreApplication::processEvents();
                painted = paintOf(view, next.marker);
            }
            trial.drawnTicks = painted ? painted->endTicks : qpcNow();
            std::optional<CaptureFrame> seen;
            while (!seen && qpcNow() < deadline) {
                seen = probe.waitForMarker(next.marker, trial.triggerTicks, std::chrono::milliseconds(2));
                if (!seen) {
                    QCoreApplication::processEvents(); // Keeps a deferred present moving.
                }
            }
            if (seen) {
                trial.screenTicks = seen->update.screenTicks();
            }
            out.push_back(trial);
        }
    }
    ViewState idle = view.state();
    idle.injectDelayMs = 0;
    view.setState(idle);
    return out;
}

struct TypingSample {
    std::uint32_t seq = 0;
    std::int64_t inputTicks = 0;
    std::int64_t flushEndTicks = 0;
};

// Turns key presses on the view into edits, the way an editor's keyPressEvent would.
class TypingFilter final : public QObject {
public:
    TypingFilter(frost::s1::TextModel& model, FrameView& view, int lineHeight, std::uint32_t& seq)
        : m_model(model), m_view(view), m_lineHeight(lineHeight), m_seq(seq) {}

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::KeyPress || watched != m_view.widget()) {
            return QObject::eventFilter(watched, event);
        }
        // Budget: B5.
        const auto* key = static_cast<QKeyEvent*>(event);
        const QString text = key->text();
        if (text.isEmpty()) {
            return true;
        }
        ViewState state = m_view.state();
        const frost::s1::VisibleLines visible =
            frost::s1::visibleLines(state, m_view.widget()->height(), m_lineHeight, m_model.lineCount());
        const std::size_t line = visible.first + std::min<std::size_t>(20, visible.count / 2);
        m_column = m_model.insert(line, m_column, static_cast<char>(text.front().toLatin1()));
        state.marker = static_cast<std::uint16_t>(++m_seq);
        m_view.setState(state);
        const int top = visible.topPx + static_cast<int>(line - visible.first) * m_lineHeight;
        QRegion dirty(0, top, m_view.widget()->width(), m_lineHeight);
        dirty += QRect(0, 0, 4 * kMarkerCells, 4);
        m_view.repaintNow(dirty);
        return true;
    }

private:
    frost::s1::TextModel& m_model;
    FrameView& m_view;
    int m_lineHeight;
    std::uint32_t& m_seq;
    std::size_t m_column = 4;
};

// Why a finished run is invalid, or empty when it is valid.
std::string invalidReason(const SessionVerdict& after, std::uint32_t lost,
                          const std::optional<std::string>& captureFailure) {
    if (!after.valid) {
        return after.problems.empty() ? std::string("the session became invalid") : after.problems.front();
    }
    if (lost != 0) {
        return "Desktop Duplication access was lost " + std::to_string(lost) + " times";
    }
    if (captureFailure) {
        return "capture failed: " + *captureFailure;
    }
    return {};
}

int run(QApplication& app, const Options& options) {
    const DisplayRequiredGuard awake;
    const SystemSessionState session;
    const TickRate rate = qpcRate();
    JsonWriter json;
    json.beginObject();
    json.field("tool", "s1_editor");
    json.field("backend", options.rhi ? "rhi" : "raster");
    json.field("contentMode", options.fillOnly ? "fill" : "text");
    json.field("mode", options.validateDelays ? "validate-delays" : "fling");
    json.field("scrollBlit", options.scrollBlit);

    const auto fail = [&](const char* status, const std::string& reason, int code) {
        json.field("status", status);
        json.field("reason", reason);
        json.endObject();
        writeReport(options, json.str());
        std::printf("s1_editor: %s: %s\n", status, reason.c_str());
        return code;
    };

    const SessionVerdict before = precheck(session);
    if (!before.valid) {
        json.key("sessionStart");
        writeVerdict(json, before);
        return fail("invalid", before.problems.front(), 3);
    }
    QScreen* const screen = QGuiApplication::primaryScreen();
    if (screen == nullptr || screen->devicePixelRatio() != 1.0) {
        return fail("setup-error", "the spike needs a primary screen at devicePixelRatio 1", 2);
    }

    const frost::s1::TextFont font = frost::s1::TextFont::make(kFontPixels);
    const frost::s1::Theme theme;
    frost::s1::TextModel model(frost::s1::makeCorpus(kCorpusLines, kCorpusSeed));
    const Views views = makeView(options, model, font, theme);
    FrameView* const view = views.view.get();
    QWidget* const window = view->widget();
    window->setWindowTitle(QStringLiteral("FrostIDE S1 spike"));
    window->setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    window->setAttribute(Qt::WA_ShowWithoutActivating);
    window->setGeometry(
        frost::s1::windowRect(screen->availableGeometry(), screen->geometry(), options.size, options.fullOutput));
    ViewState state;
    state.fillOnly = options.fillOnly;
    view->setState(state);
    window->show();
    pump(app, 600); // Show animation and first paint.
    const auto closeWindow = [&] {
        window->close();
        pump(app, 50);
    };
    if (views.rhi != nullptr && !views.rhi->error().isEmpty()) {
        closeWindow();
        return fail("setup-error", views.rhi->error().toStdString(), 2);
    }

    const ScreenRect client = clientRect(window);
    DuplicationOptions capture;
    capture.watch = client;
    capture.marker = ScreenRect{client.left, client.top, client.left + 4 * kMarkerCells, client.top + 4};
    capture.outputX = client.left + client.width() / 2;
    capture.outputY = client.top + client.height() / 2;
    auto probe = DuplicationProbe::open(capture);
    auto clock = VBlankClock::forPoint(capture.outputX, capture.outputY);
    const auto dwm = readDwmCounters();
    if (!probe || !clock || !dwm || dwm->refreshPeriodTicks <= 0) {
        closeWindow();
        return fail("setup-error", "Desktop Duplication, vblank clock or DWM timing unavailable", 2);
    }
    const std::int64_t period = dwm->refreshPeriodTicks;
    const double periodMs = rate.toMs(period);
    (*probe)->start();
    const auto writeWindow = [&] {
        json.key("window").beginObject();
        json.field("left", client.left).field("top", client.top).field("width", client.width());
        json.field("height", client.height());
        json.field("area", options.fullOutput ? "output" : "work-area");
        json.endObject();
        json.field("refreshPeriodMs", periodMs);
    };

    if (options.validateDelays) {
        std::uint32_t seq = 0;
        const std::vector<DelayTrial> trials = runDelayTrials(*view, **probe, *clock, period, options.trials, seq);
        pump(app, 100); // The last trial's frame has reached the screen.
        (*probe)->stop();
        const std::uint32_t lost = (*probe)->accessLost();
        const std::optional<std::string> captureFailure = (*probe)->failure();
        const SessionVerdict after = precheck(session);
        closeWindow();
        const ValidationReport report = evaluateDelayTrials(trials, periodMs, rate);
        std::string reason = invalidReason(after, lost, captureFailure);
        if (reason.empty() && trials.size() != kDelaysMs.size() * static_cast<std::size_t>(options.trials)) {
            reason = "the vblank clock failed before every trial ran";
        }
        const bool valid = reason.empty();
        json.field("status", valid ? "valid" : "invalid");
        json.field("reason", reason);
        json.field("captureFailure", captureFailure.value_or(std::string()));
        json.key("sessionStart");
        writeVerdict(json, before);
        json.key("sessionEnd");
        writeVerdict(json, after);
        json.field("accessLost", static_cast<std::uint64_t>(lost));
        writeWindow();
        json.field("trialsPerDelay", options.trials);
        json.field("passed", report.passed);
        json.key("delays").beginArray();
        for (const DelayResult& result : report.delays) {
            writeDelay(json, result);
        }
        json.endArray();
        json.key("problems");
        writeStrings(json, report.problems);
        json.endObject();
        writeReport(options, json.str());

        std::printf("s1_editor %s delay validation: %s%s%s\n", options.rhi ? "rhi" : "raster",
                    !valid ? "INVALID" : (report.passed ? "pass" : "FAIL"), valid ? "" : ": ", reason.c_str());
        for (const DelayResult& result : report.delays) {
            std::printf("  D = %4.1f ms: %zu trials, %zu lost; shift %+.2f ms (error %+.2f); residual max %.2f ms\n",
                        result.injectedMs, result.trials, result.lost, result.meanShiftMs, result.shiftErrorMs,
                        result.residualMs.max);
        }
        for (const std::string& problem : report.problems) {
            std::printf("  problem: %s\n", problem.c_str());
        }
        if (!valid) {
            return 3;
        }
        return report.passed ? 0 : 1;
    }

    // The run state shared by the tick handler and the typing timer (GUI thread only).
    enum class Phase { Fling, Typing, Done };
    Phase phase = Phase::Fling;
    std::uint32_t seq = 0;
    std::int64_t flingStart = 0;
    const frost::s1::Fling fling;
    const ThreadCycleMeter meter;
    std::vector<AppFrame> flingFrames;
    std::vector<double> cpuMs;      // Tick to tick, all GUI-thread work.
    std::vector<bool> cpuAfterSpin; // Per cpuMs entry: the interval held an injected spin.
    std::vector<double> syncCpuMs;  // Inside the tick handler only.
    std::uint64_t previousCycles = 0;
    bool spunThisTick = false;
    std::vector<double> tickToReadyMs;
    std::vector<double> dispatchMs;
    std::vector<TypingSample> typing;
    std::uint32_t firstFlingSeq = 0;
    std::uint32_t lastFlingSeq = 0;

    // One tick per vblank; while the GUI thread is still busy with the previous
    // tick, further vblanks are counted as coalesced instead of queued.
    std::atomic<bool> stopTicker{false};
    std::atomic<bool> tickPending{false};
    std::atomic<std::int64_t> latestTick{0};
    std::atomic<std::uint32_t> vblanks{0};
    std::atomic<std::uint32_t> coalesced{0};
    std::uint32_t flingCoalesced = 0; // `coalesced` when the fling ended.

    TypingFilter filter(model, *view, font.lineHeight, seq);
    window->installEventFilter(&filter);
    QTimer keyTimer;
    keyTimer.setTimerType(Qt::PreciseTimer);
    keyTimer.setInterval(kKeyIntervalMs);
    QObject::connect(&keyTimer, &QTimer::timeout, window, [&] {
        if (phase != Phase::Typing || static_cast<int>(typing.size()) >= options.keys) {
            phase = Phase::Done;
            keyTimer.stop();
            return;
        }
        TypingSample sample;
        sample.inputTicks = qpcNow();
        QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
        QCoreApplication::sendEvent(window, &press);
        sample.flushEndTicks = qpcNow(); // Raster: flush end. RHI: the render is still to come.
        sample.seq = seq;
        typing.push_back(sample);
    });

    const auto onTick = [&](std::int64_t tick) {
        if (phase != Phase::Fling) {
            return;
        }
        // Budget: B6.
        const std::int64_t handled = qpcNow();
        const std::uint64_t tickCycles = meter.cyclesNow();
        if (previousCycles != 0) {
            cpuMs.push_back(meter.toMs(tickCycles - previousCycles));
            cpuAfterSpin.push_back(spunThisTick);
        }
        previousCycles = tickCycles;
        spunThisTick = false;
        if (flingStart == 0) {
            flingStart = tick;
        }
        const double t = rate.toMs(tick - flingStart) / 1000.0;
        if (t > options.seconds) {
            flingCoalesced = coalesced.load();
            phase = options.keys > 0 ? Phase::Typing : Phase::Done;
            if (phase == Phase::Typing) {
                keyTimer.start();
            }
            return;
        }
        ViewState next = view->state();
        next.scrollPx = std::round(fling.offsetPx(t));
        next.marker = static_cast<std::uint16_t>(++seq);
        view->setState(next);
        view->repaintNow({});
        const std::int64_t ready = qpcNow();
        syncCpuMs.push_back(meter.toMs(meter.cyclesNow() - tickCycles));
        tickToReadyMs.push_back(rate.toMs(ready - tick));
        dispatchMs.push_back(rate.toMs(handled - tick));
        flingFrames.push_back(AppFrame{seq, ready, tick});
        firstFlingSeq = firstFlingSeq == 0 ? seq : firstFlingSeq;
        lastFlingSeq = seq;
        if (options.injectCpuMs > 0 && seq % 2 == 0) {
            // Known GUI-thread work in this tick-to-tick interval, after the frame is
            // ready (for RHI it delays the deferred render, so such a run's latency and
            // pacing are not B6 samples).
            spinUntil(qpcNow() + rate.fromMs(options.injectCpuMs));
            spunThisTick = true;
        }
    };

    std::thread ticker([&] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        while (!stopTicker.load()) {
            const auto tick = clock->wait();
            if (!tick) {
                break;
            }
            latestTick.store(*tick);
            vblanks.fetch_add(1);
            if (tickPending.exchange(true)) {
                coalesced.fetch_add(1);
                continue;
            }
            static_cast<void>(QMetaObject::invokeMethod(
                window,
                [&] {
                    tickPending.store(false);
                    onTick(latestTick.load());
                },
                Qt::QueuedConnection));
        }
    });

    QElapsedTimer watchdog;
    watchdog.start();
    const qint64 limitMs = static_cast<qint64>(options.seconds * 1000.0) + 4000;
    while (phase != Phase::Done && watchdog.elapsed() < limitMs) {
        // The one-argument overload blocks for events; the (flags, maxTime) overload
        // drops WaitForMoreEvents and would spin, which UI CPU per frame would count.
        // The vblank thread posts an event every refresh, so the loop always wakes.
        app.processEvents(QEventLoop::WaitForMoreEvents);
    }
    if (phase == Phase::Fling) {
        flingCoalesced = coalesced.load(); // The watchdog ended the fling.
    }
    pump(app, 150); // Let the last frames reach the screen.
    stopTicker.store(true);
    ticker.join();
    (*probe)->stop();
    window->removeEventFilter(&filter);
    const std::uint32_t lost = (*probe)->accessLost();
    const std::optional<std::string> captureFailure = (*probe)->failure();
    const SessionVerdict after = precheck(session);
    closeWindow();

    // Analysis.
    std::vector<ScreenUpdate> captured;
    for (const CaptureFrame& frame : (*probe)->frames()) {
        captured.push_back(frame.update);
    }
    const std::vector<ScreenUpdate> displayed = collapseToDisplay(captured, period);
    std::vector<ScreenUpdate> flingUpdates;
    std::size_t undecoded = 0;
    for (const ScreenUpdate& update : displayed) {
        if (!update.marker) {
            ++undecoded;
        } else if (*update.marker >= firstFlingSeq && *update.marker <= lastFlingSeq) {
            flingUpdates.push_back(update);
        }
    }
    const ContentStats content = analyseContent(flingUpdates, std::numeric_limits<std::int64_t>::min(),
                                                std::numeric_limits<std::int64_t>::max(), period, rate);
    const auto flingMatches = matchFrames(flingFrames, displayed, rate.fromMs(0.5));
    const LatencyStats flingLatency = summariseLatency(flingFrames, flingMatches, rate);
    // Paint durations of fling frames, and for each key the first paint that drew
    // its marker (the paint log is in paint order; markers are unique per run).
    std::vector<double> paintMs;
    std::vector<std::int64_t> paintEndBySeq(seq + 1, 0);
    for (const frost::s1::PaintRecord& record : view->paints()) {
        if (record.marker >= firstFlingSeq && record.marker <= lastFlingSeq) {
            paintMs.push_back(rate.toMs(record.endTicks - record.beginTicks));
        }
        if (record.marker <= seq && paintEndBySeq[record.marker] == 0) {
            paintEndBySeq[record.marker] = record.endTicks;
        }
    }
    std::vector<AppFrame> typingFrames;
    std::vector<double> inputToPaintEnd;
    std::vector<double> inputToFlushEnd;
    for (const TypingSample& sample : typing) {
        typingFrames.push_back(AppFrame{sample.seq, sample.flushEndTicks, sample.inputTicks});
        if (sample.seq < paintEndBySeq.size() && paintEndBySeq[sample.seq] != 0) {
            inputToPaintEnd.push_back(rate.toMs(paintEndBySeq[sample.seq] - sample.inputTicks));
        }
        inputToFlushEnd.push_back(rate.toMs(sample.flushEndTicks - sample.inputTicks));
    }
    const auto typingMatches = matchFrames(typingFrames, displayed, rate.fromMs(0.5));
    const LatencyStats typingLatency = summariseLatency(typingFrames, typingMatches, rate);

    const Summary cpu = summarize(cpuMs);
    const Summary paint = summarize(paintMs);
    const Summary b5 = summarize(inputToPaintEnd);
    std::vector<double> spunCpu;
    std::vector<double> plainCpu;
    for (std::size_t i = 0; i < cpuMs.size(); ++i) {
        (cpuAfterSpin[i] ? spunCpu : plainCpu).push_back(cpuMs[i]);
    }
    const bool injecting = options.injectCpuMs > 0;
    const std::optional<frost::s1::CpuInjectionCheck> injection =
        injecting
            ? std::optional(frost::s1::checkCpuInjection(spunCpu, plainCpu, options.injectCpuMs, kInjectTolerance))
            : std::nullopt;
    const frost::s1::Gates gates = frost::s1::evaluateGates(
        frost::s1::RunCounts{flingFrames.size(), cpuMs.size(), content.updates, undecoded, typing.size(),
                             inputToPaintEnd.size()},
        frost::s1::RunMetrics{cpu.p99, content.missedFraction, content.maxIntervalMs, b5.p50, b5.p99}, injecting);
    std::string reason = invalidReason(after, lost, captureFailure);
    if (reason.empty() && !gates.decodedEnough) {
        reason = "too few fling markers decoded on screen";
    }
    const bool valid = reason.empty();

    json.field("status", valid ? "valid" : "invalid");
    json.field("reason", reason);
    json.field("captureFailure", captureFailure.value_or(std::string()));
    json.field("decodedEnough", gates.decodedEnough);
    json.key("sessionStart");
    writeVerdict(json, before);
    json.key("sessionEnd");
    writeVerdict(json, after);
    json.field("accessLost", static_cast<std::uint64_t>(lost));
    writeWindow();
    json.key("font").beginObject();
    json.field("family", font.font.family().toStdString());
    json.field("pixelSize", kFontPixels).field("advance", font.advance).field("lineHeight", font.lineHeight);
    json.endObject();
    json.field("lines", static_cast<std::uint64_t>(model.lineCount()));
    json.field("flingSeconds", options.seconds);
    json.field("flingVelocityPxPerS", fling.velocityPxPerS).field("flingTauS", fling.tauS);
    json.field("flingFrames", static_cast<std::uint64_t>(flingFrames.size()));
    // Fling ticks = painted frames + vblanks coalesced while a tick was pending.
    json.field("flingCoalesced", static_cast<std::uint64_t>(flingCoalesced));
    json.field("flingTicks", static_cast<std::uint64_t>(flingFrames.size() + flingCoalesced));
    json.field("vblanks", static_cast<std::uint64_t>(vblanks.load()));
    json.field("vblanksCoalesced", static_cast<std::uint64_t>(coalesced.load()));
    json.key("uiCpuPerFrameMs").value(cpu);
    json.key("tickHandlerCpuMs").value(summarize(syncCpuMs));
    json.key("paintMs").value(paint);
    json.key("vblankToFlushMs").value(summarize(tickToReadyMs));
    json.key("vblankToDispatchMs").value(summarize(dispatchMs));
    json.key("content");
    writeContent(json, content);
    json.field("undecodedCaptures", static_cast<std::uint64_t>(undecoded));
    // inputToScreenMs is vblank tick to screen; readyToScreenMs starts after the tick handler.
    json.key("flingLatency");
    writeLatency(json, flingLatency);
    json.key("typing").beginObject();
    json.field("keys", static_cast<std::uint64_t>(typing.size()));
    json.key("inputToPaintEndMs").value(b5);
    json.field("paintedKeys", static_cast<std::uint64_t>(inputToPaintEnd.size()));
    json.key("inputToHandlerReturnMs").value(summarize(inputToFlushEnd));
    json.key("latency");
    writeLatency(json, typingLatency);
    json.endObject();
    if (injection) {
        json.key("cpuInjection").beginObject();
        json.field("injectedMs", injection->injectedMs);
        json.key("spunIntervalsMs").value(injection->spunMs);
        json.key("plainIntervalsMs").value(injection->plainMs);
        json.field("measuredDeltaMs", injection->measuredDeltaMs);
        json.field("toleranceFraction", kInjectTolerance);
        writeVerdictField(json, "pass", injection->pass);
        json.endObject();
    }
    if (views.raster != nullptr) {
        json.field("glyphRunCacheHits", static_cast<std::uint64_t>(views.raster->cache().hits()));
        json.field("glyphRunCacheMisses", static_cast<std::uint64_t>(views.raster->cache().misses()));
    }
    if (views.rhi != nullptr) {
        json.field("lastQuadCount", static_cast<std::uint64_t>(views.rhi->lastQuadCount()));
        json.field("truncatedFrames", static_cast<std::uint64_t>(views.rhi->truncatedFrames()));
        json.field("rhiError", views.rhi->error().toStdString());
    }
    // Verdicts: true, false, or null when the run did not measure enough samples.
    json.key("b6").beginObject();
    writeVerdictField(json, "cpuP99Pass", gates.b6Cpu);
    writeVerdictField(json, "missedPass", gates.b6Missed);
    writeVerdictField(json, "intervalPass", gates.b6Interval);
    json.endObject();
    writeVerdictField(json, "b5Pass", gates.b5);
    json.endObject();
    writeReport(options, json.str());

    std::printf("s1_editor %s/%s: %s%s%s\n", options.rhi ? "rhi" : "raster", options.fillOnly ? "fill" : "text",
                valid ? "valid" : "INVALID", valid ? "" : ": ", reason.c_str());
    std::printf("  fling: %zu frames, %u ticks coalesced; UI CPU/frame p50 %.2f p99 %.2f max %.2f ms; "
                "paint p50 %.2f p99 %.2f ms\n",
                flingFrames.size(), flingCoalesced, cpu.p50, cpu.p99, cpu.max, paint.p50, paint.p99);
    std::printf("  screen: %zu updates over %zu refreshes, missed %.1f %%, max interval %.2f ms; vblank to screen "
                "p50 %.2f p99 %.2f ms; %zu never shown, %zu undecoded\n",
                content.updates, content.refreshes, content.missedFraction * 100.0, content.maxIntervalMs,
                flingLatency.inputToScreenMs.p50, flingLatency.inputToScreenMs.p99, flingLatency.neverShown, undecoded);
    std::printf("  typing: %zu keys (%zu painted); input to paint end p50 %.2f p99 %.2f ms; to screen p50 %.2f "
                "p99 %.2f ms\n",
                typing.size(), inputToPaintEnd.size(), b5.p50, b5.p99, typingLatency.inputToScreenMs.p50,
                typingLatency.inputToScreenMs.p99);
    if (injection) {
        std::printf("  UI-CPU probe check: injected %.2f ms, measured %+.3f ms (%zu with, %zu without): %s\n",
                    injection->injectedMs, injection->measuredDeltaMs, spunCpu.size(), plainCpu.size(),
                    verdictText(injection->pass));
    }
    std::printf("  B6 cpu %s, missed %s, interval %s; B5 %s\n", verdictText(gates.b6Cpu), verdictText(gates.b6Missed),
                verdictText(gates.b6Interval), verdictText(gates.b5));
    if (!valid) {
        return 3;
    }
    return injection && !injection->pass.value_or(false) ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    // B1 context for --first-frame: taken before Qt starts.
    const std::int64_t mainTicks = qpcNow();
    FILETIME mainTime{};
    GetSystemTimePreciseAsFileTime(&mainTime);
    // Plugins (platforms/qwindows) are copied next to the executable at build time.
    QApplication app(argc, argv);
    const Options options = parseOptions(QCoreApplication::arguments());
    if (options.firstFrame) {
        return runFirstFrame(app, options, mainTicks, mainTime);
    }
    return run(app, options);
}
