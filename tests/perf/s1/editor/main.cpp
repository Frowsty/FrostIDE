// SPDX-License-Identifier: Apache-2.0
//
// s1_editor: one S1 run of one editor backend at the full desktop width (ADR 0007).
//
//   s1_editor --backend raster|rhi [--content text|fill] [--scroll-blit] [--seconds 10]
//             [--keys 50] [--size WxH] [--out report.json]
//
// The run opens a frameless, topmost window over the work area of the primary
// screen (5120 x 1392 on REF-W1, not exclusive fullscreen, shown without
// activation so it never takes focus), then:
//   fling   for --seconds, a vblank thread (IDXGIOutput::WaitForVBlank) posts one
//           tick per refresh; the GUI thread advances the scroll along the fling
//           curve and repaints. UI CPU per frame is the GUI thread's cycle time
//           (QueryThreadCycleTime) from one tick to the next, so it includes work
//           Qt defers to the event loop (QRhiWidget renders and presents on the
//           window's next update, not inside repaint());
//   typing  --keys synthetic key presses, 23 ms apart (not vblank-aligned), each
//           inserting a character and repainting the dirty line (raster) or the
//           frame (RHI); B5 is input to paint end, with flush end and screen too;
//   probes  the Desktop Duplication probe watches the window and decodes the
//           frame marker at its top-left corner, giving content intervals and
//           missed refreshes (B6) and frame-to-screen latency.
// --content fill paints only the background and the marker, which measures the
// backend's own floor on this desktop. --scroll-blit (raster) moves the backing
// store and paints only the exposed strip, as a shipping raster editor would.
// --size runs in a smaller window at the work area's top-left, to see which costs
// scale with the window area. On screen for about 12 s; the window is
// closed before analysis. Exit codes: 0 the run is valid (pass or fail is in the
// report), 2 setup error, 3 invalid session (locked, monitor off, access lost).

#include "frost/perf/frame_analysis.hpp"
#include "frost/perf/frame_marker.hpp"
#include "frost/perf/json_writer.hpp"
#include "frost/perf/report.hpp"
#include "frost/perf/win/screen_probes.hpp"
#include "frost/perf/win/system_session_state.hpp"
#include "frost/perf/win/timing.hpp"

#include "corpus.hpp"
#include "views.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QScreen>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
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
constexpr std::uint64_t kCorpusSeed = 0x51'2026;
constexpr int kFontPixels = 15;
constexpr int kKeyIntervalMs = 23;

struct Options {
    bool rhi = false;
    bool fillOnly = false;
    bool scrollBlit = false;
    double seconds = 10.0;
    int keys = 50;
    QSize size; // Empty: the whole work area.
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

struct TypingSample {
    std::uint32_t seq = 0;
    std::int64_t inputTicks = 0;
    std::int64_t paintEndTicks = 0;
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

int run(QApplication& app, const Options& options) {
    const DisplayRequiredGuard awake;
    const SystemSessionState session;
    const TickRate rate = qpcRate();
    JsonWriter json;
    json.beginObject();
    json.field("tool", "s1_editor");
    json.field("backend", options.rhi ? "rhi" : "raster");
    json.field("content", options.fillOnly ? "fill" : "text");
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
    std::unique_ptr<FrameView> view;
    frost::s1::RasterView* raster = nullptr;
    frost::s1::RhiView* rhiView = nullptr;
    if (options.rhi) {
        auto created = std::make_unique<frost::s1::RhiView>(model, font, theme);
        rhiView = created.get();
        view = std::move(created);
    } else {
        auto created = std::make_unique<frost::s1::RasterView>(model, font, theme);
        created->setScrollBlit(options.scrollBlit);
        raster = created.get();
        view = std::move(created);
    }
    QWidget* const window = view->widget();
    window->setWindowTitle(QStringLiteral("FrostIDE S1 spike"));
    window->setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    window->setAttribute(Qt::WA_ShowWithoutActivating);
    QRect geometry = screen->availableGeometry();
    if (!options.size.isEmpty()) {
        geometry.setSize(options.size.boundedTo(geometry.size()));
    }
    window->setGeometry(geometry);
    ViewState state;
    state.fillOnly = options.fillOnly;
    view->setState(state);
    window->show();
    pump(app, 600); // Show animation and first paint.
    const auto closeWindow = [&] {
        window->close();
        pump(app, 50);
    };
    if (rhiView != nullptr && !rhiView->error().isEmpty()) {
        closeWindow();
        return fail("setup-error", rhiView->error().toStdString(), 2);
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

    // The run state shared by the tick handler and the typing timer (GUI thread only).
    enum class Phase { Fling, Typing, Done };
    Phase phase = Phase::Fling;
    std::uint32_t seq = 0;
    std::int64_t flingStart = 0;
    const frost::s1::Fling fling;
    const ThreadCycleMeter meter;
    std::vector<AppFrame> flingFrames;
    std::vector<double> cpuMs;     // Tick to tick, all GUI-thread work.
    std::vector<double> syncCpuMs; // Inside the tick handler only.
    std::uint64_t previousCycles = 0;
    std::vector<double> tickToReadyMs;
    std::vector<double> dispatchMs;
    std::vector<TypingSample> typing;
    std::uint32_t firstFlingSeq = 0;
    std::uint32_t lastFlingSeq = 0;
    std::uint32_t firstTypingSeq = 0;

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
        }
        previousCycles = tickCycles;
        if (flingStart == 0) {
            flingStart = tick;
        }
        const double t = rate.toMs(tick - flingStart) / 1000.0;
        if (t > options.seconds) {
            phase = options.keys > 0 ? Phase::Typing : Phase::Done;
            firstTypingSeq = seq + 1;
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
    };

    // One tick per vblank; while the GUI thread is still busy with the previous
    // tick, further vblanks are counted as coalesced instead of queued.
    std::atomic<bool> stopTicker{false};
    std::atomic<bool> tickPending{false};
    std::atomic<std::int64_t> latestTick{0};
    std::atomic<std::uint32_t> vblanks{0};
    std::atomic<std::uint32_t> coalesced{0};
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
    pump(app, 150); // Let the last frames reach the screen.
    stopTicker.store(true);
    ticker.join();
    (*probe)->stop();
    window->removeEventFilter(&filter);
    const std::uint32_t lost = (*probe)->accessLost();
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
    const bool valid = after.valid && lost == 0 && !flingFrames.empty();
    const bool b6Cpu = cpu.p99 <= 5.0;
    const bool b6Missed = content.missedFraction * 100.0 <= 1.0;
    const bool b6Interval = content.maxIntervalMs <= 16.7;
    const bool b5Pass = b5.p50 <= 3.0 && b5.p99 <= 8.0;

    json.field("status", valid ? "valid" : "invalid");
    json.key("sessionStart");
    writeVerdict(json, before);
    json.key("sessionEnd");
    writeVerdict(json, after);
    json.field("accessLost", static_cast<std::uint64_t>(lost));
    json.key("window").beginObject();
    json.field("left", client.left).field("top", client.top).field("width", client.width());
    json.field("height", client.height());
    json.endObject();
    json.key("font").beginObject();
    json.field("family", font.font.family().toStdString());
    json.field("pixelSize", kFontPixels).field("advance", font.advance).field("lineHeight", font.lineHeight);
    json.endObject();
    json.field("lines", static_cast<std::uint64_t>(model.lineCount()));
    json.field("refreshPeriodMs", periodMs);
    json.field("flingSeconds", options.seconds);
    json.field("flingVelocityPxPerS", fling.velocityPxPerS).field("flingTauS", fling.tauS);
    json.field("flingFrames", static_cast<std::uint64_t>(flingFrames.size()));
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
    if (raster != nullptr) {
        json.field("glyphRunCacheHits", static_cast<std::uint64_t>(raster->cache().hits()));
        json.field("glyphRunCacheMisses", static_cast<std::uint64_t>(raster->cache().misses()));
    }
    if (rhiView != nullptr) {
        json.field("lastQuadCount", static_cast<std::uint64_t>(rhiView->lastQuadCount()));
        json.field("rhiError", rhiView->error().toStdString());
    }
    json.key("b6").beginObject();
    json.field("cpuP99Pass", b6Cpu).field("missedPass", b6Missed).field("intervalPass", b6Interval);
    json.endObject();
    json.field("b5Pass", b5Pass);
    json.endObject();
    writeReport(options, json.str());

    std::printf("s1_editor %s/%s: %s\n", options.rhi ? "rhi" : "raster", options.fillOnly ? "fill" : "text",
                valid ? "valid" : "INVALID");
    std::printf("  fling: %zu frames, %u vblanks (%u coalesced); UI CPU/frame p50 %.2f p99 %.2f max %.2f ms; "
                "paint p50 %.2f p99 %.2f ms\n",
                flingFrames.size(), vblanks.load(), coalesced.load(), cpu.p50, cpu.p99, cpu.max, paint.p50, paint.p99);
    std::printf("  screen: %zu updates over %zu refreshes, missed %.1f %%, max interval %.2f ms; frame to screen "
                "p50 %.2f p99 %.2f ms, %zu never shown\n",
                content.updates, content.refreshes, content.missedFraction * 100.0, content.maxIntervalMs,
                flingLatency.readyToScreenMs.p50, flingLatency.readyToScreenMs.p99, flingLatency.neverShown);
    std::printf("  typing: %zu keys; input to paint end p50 %.2f p99 %.2f ms; to screen p50 %.2f p99 %.2f ms\n",
                typing.size(), b5.p50, b5.p99, typingLatency.inputToScreenMs.p50, typingLatency.inputToScreenMs.p99);
    std::printf("  B6 cpu %s, missed %s, interval %s; B5 %s\n", b6Cpu ? "pass" : "FAIL", b6Missed ? "pass" : "FAIL",
                b6Interval ? "pass" : "FAIL", b5Pass ? "pass" : "FAIL");
    return valid ? 0 : 3;
}

} // namespace

int main(int argc, char** argv) {
    // Plugins (platforms/qwindows) are copied next to the executable at build time.
    QApplication app(argc, argv);
    const Options options = parseOptions(QCoreApplication::arguments());
    return run(app, options);
}
