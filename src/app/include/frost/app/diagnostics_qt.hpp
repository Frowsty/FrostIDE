// SPDX-License-Identifier: Apache-2.0
//
// frost's diagnostics wiring (ADR 0022, D-E and D-F): the program's crash reporter, log,
// stall watchdog and retention thread, which frost::diag provides Qt-free, and the Qt hooks
// they need: the event-loop hooks of the stall watchdog and the GUI's Qt message handler.
//
/*
 * The GUI's order (main.cpp), chosen for B1 (first frame):
 *   1. installProgramFatalHandlers(): at once, before QApplication; the reporter is inactive,
 *      so a fatal error before step 4 ends the process with a message and no report.
 *   2. startProgramLogging(): the log backend starts; its file opens with the first record.
 *   3. afterFirstFrame(window, ...): once the main window was first exposed and a 0 ms timer
 *      ran (L0), on the GUI thread: startProgramWatchdog() (the sampler must be created on the
 *      thread it samples) and startProgramDiagnosticsThread().
 *   4. The "frost-diag" thread starts the crash handler, annotates the reporter, then prunes
 *      the diagnostics directories, then sleeps for 24 h between prunes.
 *   5. At exit, shutdownProgramDiagnostics(): the watchdog stops, the log is flushed for at most
 *      50 ms (Budget: B20). The reporter, its handler and the log backend are kept until the
 *      process ends.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/diag/crash_reporter.hpp"
#include "frost/diag/paths.hpp"
#include "frost/diag/stall_watchdog.hpp"

#include <QAbstractNativeEventFilter>
#include <QObject>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

class QCoreApplication;
class QWidget;

namespace frost::app {

/*
 * The Qt hooks that drive a StallWatchdog (frost/diag/stall_watchdog.hpp), on the GUI thread:
 * the thread's event dispatcher's awake() and aboutToBlock() signals (direct connections), an
 * application-wide event filter (the start of every event is progress), and on Windows a native
 * event filter that treats WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE and
 * WM_ENTERMENULOOP..WM_EXITMENULOOP as idle (Windows runs those modal loops itself, outside
 * Qt's dispatcher). Each hook costs one clock read and one store (Budget: B5, B21).
 * Construct after QApplication on the GUI thread; destroy before the watchdog and QApplication
 * (the destructor removes the filters and disconnects).
 */
class StallHooks final : public QObject, public QAbstractNativeEventFilter {
public:
    StallHooks(QCoreApplication& app, diag::StallWatchdog& watchdog);
    ~StallHooks() override;
    StallHooks(const StallHooks&) = delete ("installed once on one application");
    StallHooks& operator=(const StallHooks&) = delete ("installed once on one application");

    bool eventFilter(QObject* watched, QEvent* event) override;
    // Windows: MSG messages of type "windows_generic_MSG"; never consumes a message.
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    QCoreApplication& m_app;
    diag::StallWatchdog& m_watchdog;
    bool m_inSizeMove = false;
    bool m_inMenuLoop = false;
};

/*
 * The GUI's Qt message handler: every Qt message goes to frost::log under category "qt" (and
 * to the handler installed before it, unchanged); a QtFatalMsg also sets the frost.message
 * annotation and ends the process through ICrashReporter::crashNow(FatalReason::QtFatal) of the
 * reporter installFatalHandlers() registered. Command-line modes keep
 * installCommandLineFatalHandler() (status 4) instead. Main thread, after
 * installProgramFatalHandlers().
 */
void installGuiQtMessageHandler();

// Calls `callback` once on the GUI thread, from a 0 ms timer started when `window` (shown) is
// first exposed (L0). `window` must outlive the call.
void afterFirstFrame(QWidget& window, std::function<void()> callback);

// --- The program's diagnostics (process-lifetime state in diagnostics_qt.cpp). ---

// The diagnostics directories under the cache root; resolved on first use. Errors: those of
// diag::resolveCacheRoot() and diag::diagnosticsDirectories().
[[nodiscard]] Result<diag::DiagnosticsDirectories> programDirectories();

// The program's crash reporter (a crashpad reporter over the real launcher, inactive until
// startProgramCrashReporter()). Never destroyed.
[[nodiscard]] diag::ICrashReporter& programReporter();

// Linux: the last-chance signal handlers; then diag::installFatalHandlers(programReporter()).
void installProgramFatalHandlers();

/*
 * Creates the diagnostics directories and starts the log backend (level FROST_LOG_LEVEL,
 * default info) as frost::log's sink. Failures are reported on stderr and leave frost without a
 * log file; frost still runs. Idempotent.
 */
void startProgramLogging();

/*
 * Starts the crash handler next to this module (crashpad_handler(.exe) in the directory of
 * frost_app.dll, or of the frost executable on Linux) with `readyTimeout`, and sets the
 * frost.image_path and (when not empty) frost.restarted_from annotations. Errors: those of
 * ICrashReporter::start, and of programDirectories().
 */
[[nodiscard]] Result<diag::ReporterStatus> startProgramCrashReporter(const std::string& restartedFrom,
                                                                     std::chrono::milliseconds readyTimeout);

// Starts the "frost-diag" thread: startProgramCrashReporter(restartedFrom, 5 s), then
// retention (diag::PeriodicPruner's loop). Once per process.
void startProgramDiagnosticsThread(std::string restartedFrom);

/*
 * Starts the stall watchdog for the calling (GUI) thread with freeze reports in
 * <cache>/crashes/freezes and installs its StallHooks on `app`. Errors: those of
 * programDirectories(), diag::createSamplerForCurrentThread() and diag::StallWatchdog::start().
 */
[[nodiscard]] Result<diag::StallWatchdog*> startProgramWatchdog(QCoreApplication& app);

// The watchdog startProgramWatchdog() started, or nullptr.
[[nodiscard]] diag::StallWatchdog* programWatchdog() noexcept;

// Stops the watchdog and removes its hooks (GUI thread, before QApplication is destroyed).
void stopProgramWatchdog() noexcept;

// After QApplication is gone: flushes the log for at most 50 ms (Budget: B20).
void flushProgramLog() noexcept;

} // namespace frost::app
