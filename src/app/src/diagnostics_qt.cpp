// SPDX-License-Identifier: Apache-2.0
//
// frost's diagnostics wiring (frost/app/diagnostics_qt.hpp).

#include "frost/app/diagnostics_qt.hpp"

#include "frost/app/startup_core.hpp"
#include "frost/core/build_info.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/log.hpp"
#include "frost/diag/fatal_handlers.hpp"
#include "frost/diag/log_backend.hpp"
#include "frost/diag/retention.hpp"
#include "frost/diag/thread_sampler.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QEvent>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <atomic>
#include <cstdio>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>

extern "C" IMAGE_DOS_HEADER __ImageBase; // This module: frost_app.dll in the program.
#else
#include <unistd.h>
#endif

namespace frost::app {

// --- StallHooks ---

StallHooks::StallHooks(QCoreApplication& app, diag::StallWatchdog& watchdog) : m_app(app), m_watchdog(watchdog) {
    QAbstractEventDispatcher* dispatcher = QAbstractEventDispatcher::instance(app.thread());
    if (dispatcher != nullptr) {
        diag::StallWatchdog* target = &m_watchdog;
        connect(
            dispatcher, &QAbstractEventDispatcher::awake, this, [target] { target->eventLoopWoke(); },
            Qt::DirectConnection);
        connect(
            dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, [target] { target->eventLoopWillBlock(); },
            Qt::DirectConnection);
    }
    app.installEventFilter(this);
    app.installNativeEventFilter(this);
}

StallHooks::~StallHooks() {
    m_app.removeNativeEventFilter(this);
    m_app.removeEventFilter(this);
    if (m_inSizeMove) {
        m_watchdog.idleEnd();
    }
    if (m_inMenuLoop) {
        m_watchdog.idleEnd();
    }
}

bool StallHooks::eventFilter(QObject*, QEvent*) {
    m_watchdog.eventStarted(); // Budget: B5, B21.
    return false;
}

bool StallHooks::nativeEventFilter([[maybe_unused]] const QByteArray& eventType, [[maybe_unused]] void* message,
                                   qintptr*) {
#ifdef _WIN32
    if (eventType != "windows_generic_MSG" || message == nullptr) {
        return false;
    }
    const auto toggle = [this](bool& flag, bool enter) {
        if (flag == enter) {
            return; // Unbalanced (a nested or repeated notification): keep one idle section.
        }
        flag = enter;
        if (enter) {
            m_watchdog.idleBegin();
        } else {
            m_watchdog.idleEnd();
        }
    };
    switch (static_cast<const MSG*>(message)->message) {
    case WM_ENTERSIZEMOVE: toggle(m_inSizeMove, true); break;
    case WM_EXITSIZEMOVE: toggle(m_inSizeMove, false); break;
    case WM_ENTERMENULOOP: toggle(m_inMenuLoop, true); break;
    case WM_EXITMENULOOP: toggle(m_inMenuLoop, false); break;
    default: break;
    }
#endif
    return false;
}

// --- The GUI's Qt message handler ---

namespace {

constinit std::atomic<QtMessageHandler> g_previousQtHandler{nullptr};

log::Level levelOf(QtMsgType type) noexcept {
    switch (type) {
    case QtDebugMsg: return log::Level::Debug;
    case QtInfoMsg: return log::Level::Info;
    case QtWarningMsg: return log::Level::Warn;
    case QtCriticalMsg: return log::Level::Error;
    case QtFatalMsg: return log::Level::Critical;
    }
    return log::Level::Warn;
}

void guiQtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const QByteArray utf8 = message.toUtf8();
    const std::string_view text(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    if (log::enabled(levelOf(type))) {
        log::writeRecord(levelOf(type), "qt", text);
    }
    if (type == QtFatalMsg) {
        flushProgramLog();
        if (diag::ICrashReporter* reporter = diag::fatalReporter(); reporter != nullptr) {
            reporter->annotate(diag::AnnotationKey::Message, text);
            reporter->crashNow(diag::FatalReason::QtFatal, text);
        }
    }
    if (const QtMessageHandler previous = g_previousQtHandler.load(); previous != nullptr) {
        previous(type, context, message);
    }
}

} // namespace

void installGuiQtMessageHandler() {
    const QtMessageHandler previous = qInstallMessageHandler(&guiQtMessageHandler);
    // Qt returns its default handler when none was installed; never chain to ourselves.
    if (previous != &guiQtMessageHandler) {
        g_previousQtHandler.store(previous);
    }
}

// --- First frame ---

namespace {

// Watches one window for its first exposure, then runs the callback from a 0 ms timer and
// deletes itself.
class FirstFrameWatcher final : public QObject {
public:
    FirstFrameWatcher(QWindow& window, std::function<void()> callback)
        : QObject(&window), m_callback(std::move(callback)) {
        window.installEventFilter(this);
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Expose && !m_fired) {
            auto* window = static_cast<QWindow*>(watched);
            if (window->isExposed()) {
                m_fired = true;
                window->removeEventFilter(this);
                QTimer::singleShot(0, this, [this] {
                    std::function<void()> callback = std::move(m_callback);
                    deleteLater();
                    callback();
                });
            }
        }
        return false;
    }

private:
    std::function<void()> m_callback;
    bool m_fired = false;
};

} // namespace

void afterFirstFrame(QWidget& window, std::function<void()> callback) {
    QWindow* handle = window.windowHandle();
    if (handle == nullptr || handle->isExposed()) {
        QTimer::singleShot(0, &window, std::move(callback));
        return;
    }
    new FirstFrameWatcher(*handle, std::move(callback)); // Parented to the window.
}

// --- The program's diagnostics ---

namespace {

// Process-lifetime state; never destroyed (the handler must serve until the very end, and
// threads that outlive main must find these objects).
struct ProgramState {
    std::mutex mutex;
    std::optional<Result<diag::DiagnosticsDirectories>> directories;
    std::unique_ptr<diag::ICrashReporter> reporter;
    std::unique_ptr<diag::LogBackend> log;
    std::unique_ptr<diag::ICrashReportStore, void (*)(diag::ICrashReportStore*)> store{nullptr, nullptr};
    std::unique_ptr<diag::PeriodicPruner> pruner;
    std::unique_ptr<diag::StallWatchdog> watchdog;
    std::unique_ptr<StallHooks> hooks;
    std::atomic<diag::StallWatchdog*> watchdogPointer{nullptr};
    std::atomic<diag::LogBackend*> logPointer{nullptr}; // For the fatal paths, which take no lock.
    bool loggingStarted = false;
};

ProgramState& state() {
    [[clang::no_destroy]] static ProgramState instance;
    return instance;
}

void printError(std::string_view what, const Error& error) {
    const std::string line = std::format("frost: {}: {}\n", what, error.describe());
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

// This module's path (frost_app.dll; the frost executable on Linux), UTF-8, without \\?\.
std::string thisModulePath() {
#ifdef _WIN32
    return io::fromWide(withoutExtendedPrefix(modulePath(&__ImageBase))).value_or(std::string());
#else
    std::string path(4096, '\0');
    const ssize_t length = ::readlink("/proc/self/exe", path.data(), path.size());
    path.resize(length > 0 ? static_cast<std::size_t>(length) : 0);
    return path;
#endif
}

} // namespace

Result<diag::DiagnosticsDirectories> programDirectories() {
    ProgramState& program = state();
    const std::scoped_lock lock(program.mutex);
    if (!program.directories.has_value()) {
        auto root = diag::resolveCacheRoot();
        if (!root) {
            program.directories.emplace(std::unexpected(std::move(root.error())));
        } else {
            program.directories.emplace(diag::diagnosticsDirectories(*root));
        }
    }
    return *program.directories;
}

diag::ICrashReporter& programReporter() {
    ProgramState& program = state();
    const std::scoped_lock lock(program.mutex);
    if (program.reporter == nullptr) {
        program.reporter = diag::createCrashpadReporter(io::realProcessLauncher(), systemClock());
    }
    return *program.reporter;
}

void installProgramFatalHandlers() {
    diag::installLastChanceHandlers();
    diag::installFatalHandlers(programReporter());
}

void startProgramLogging() {
    ProgramState& program = state();
    {
        const std::scoped_lock lock(program.mutex);
        if (program.loggingStarted) {
            return;
        }
        program.loggingStarted = true;
    }
    auto directories = programDirectories();
    if (!directories) {
        printError("no log file", directories.error());
        return;
    }
    if (auto prepared = diag::prepareDiagnosticsDirectories(*directories); !prepared) {
        printError("no log file", prepared.error());
        return;
    }
    diag::LogBackendOptions options;
    options.directory = directories->logs;
    options.pid = static_cast<std::uint32_t>(
#ifdef _WIN32
        GetCurrentProcessId()
#else
        ::getpid()
#endif
    );
    options.level = diag::logLevelFromEnvironment(log::Level::Info);
    auto backend = diag::LogBackend::start(options, diag::realLogFileOpener(), systemClock());
    if (!backend) {
        printError("no log file", backend.error());
        return;
    }
    log::setLevel(options.level);
    log::setSink(backend->get());
    program.logPointer.store(backend->get(), std::memory_order_release);
    const std::scoped_lock lock(program.mutex);
    program.log = std::move(*backend);
}

Result<diag::ReporterStatus> startProgramCrashReporter(const std::string& restartedFrom,
                                                       std::chrono::milliseconds readyTimeout) {
    auto directories = programDirectories();
    if (!directories) {
        return std::unexpected(directories.error());
    }
    if (auto prepared = diag::prepareDiagnosticsDirectories(*directories); !prepared) {
        return std::unexpected(std::move(prepared.error()));
    }
    const std::string self = thisModulePath();
    auto handler = io::joinPath(io::parentPath(self), std::string("crashpad_handler") +
#ifdef _WIN32
                                                          ".exe"
#else
                                                          ""
#endif
    );
    if (!handler) {
        return std::unexpected(std::move(handler.error()));
    }
    diag::CrashReporterConfig config;
    config.handlerPath = std::move(*handler);
    config.databasePath = directories->crashpadDatabase;
    config.version = std::string(build_info::kVersion);
    config.readyTimeout = readyTimeout;
    diag::ICrashReporter& reporter = programReporter();
    if (auto started = reporter.start(config); !started) {
        return std::unexpected(std::move(started.error()));
    }
    reporter.annotate(diag::AnnotationKey::ImagePath, self);
    if (!restartedFrom.empty()) {
        reporter.annotate(diag::AnnotationKey::RestartedFrom, restartedFrom);
    }
    diag::ReporterStatus status = reporter.status();
    // The process tests read this line (the handler's pid and path, the restart annotation);
    // the paths are quoted because they may hold spaces.
    log::info("diag", "crash handler registered pid={} path=\"{}\" restarted-from=\"{}\"", status.handlerPid,
              status.handlerPath, restartedFrom);
    return status;
}

void startProgramDiagnosticsThread(std::string restartedFrom) {
    ProgramState& program = state();
    auto directories = programDirectories();
    if (!directories) {
        log::warn("diag", "no diagnostics directories: {}", directories.error().describe());
        return;
    }
    const std::scoped_lock lock(program.mutex);
    if (program.pruner != nullptr) {
        return;
    }
    auto setup = [restartedFrom = std::move(restartedFrom), directories = *directories]() -> diag::ICrashReportStore* {
        if (auto started = startProgramCrashReporter(restartedFrom, std::chrono::seconds(5)); !started) {
            log::warn("diag", "no crash reports this session: {}", started.error().describe());
        }
        // Retention also runs without a handler: the database may hold earlier reports.
        auto store = diag::openCrashpadReportStore(directories.crashpadDatabase);
        if (!store) {
            log::warn("diag", "retention disabled: {}", store.error().describe());
            return nullptr;
        }
        ProgramState& inner = state();
        const std::scoped_lock innerLock(inner.mutex);
        inner.store = std::move(*store);
        return inner.store.get();
    };
    program.pruner =
        std::make_unique<diag::PeriodicPruner>(std::move(setup), *directories, io::realFileSystem(),
                                               diag::realActiveFileProbe(), systemClock(), diag::RetentionPolicy{});
}

Result<diag::StallWatchdog*> startProgramWatchdog(QCoreApplication& app) {
    auto directories = programDirectories();
    if (!directories) {
        return std::unexpected(directories.error());
    }
    if (auto prepared = diag::prepareDiagnosticsDirectories(*directories); !prepared) {
        return std::unexpected(std::move(prepared.error()));
    }
    auto sampler = diag::createSamplerForCurrentThread();
    if (!sampler) {
        return std::unexpected(std::move(sampler.error()));
    }
    diag::StallWatchdog::Options options;
    options.freezesDirectory = directories->freezes;
    options.version = std::string(build_info::kVersion);
    auto watchdog =
        diag::StallWatchdog::start(std::move(options), std::move(*sampler), io::realFileSystem(), systemClock());
    if (!watchdog) {
        return std::unexpected(std::move(watchdog.error()));
    }
    ProgramState& program = state();
    const std::scoped_lock lock(program.mutex);
    program.watchdog = std::move(*watchdog);
    program.hooks = std::make_unique<StallHooks>(app, *program.watchdog);
    program.watchdogPointer.store(program.watchdog.get(), std::memory_order_release);
    diag::StallWatchdog::install(program.watchdog.get()); // For ScopedUiIdle.
    return program.watchdog.get();
}

diag::StallWatchdog* programWatchdog() noexcept { return state().watchdogPointer.load(std::memory_order_acquire); }

void stopProgramWatchdog() noexcept {
    ProgramState& program = state();
    const std::scoped_lock lock(program.mutex);
    program.hooks.reset();
    diag::StallWatchdog::install(nullptr);
    if (program.watchdog != nullptr) {
        program.watchdog->stop();
        const diag::StallStats stats = program.watchdog->stats();
        log::info("diag", "UI stalls this session: {} (longest {} ms), {} reports, {} dropped", stats.stalls,
                  std::chrono::duration_cast<std::chrono::milliseconds>(stats.longest).count(), stats.reportsWritten,
                  stats.reportsDropped);
    }
}

void flushProgramLog() noexcept {
    if (diag::LogBackend* backend = state().logPointer.load(std::memory_order_acquire); backend != nullptr) {
        (void)backend->flushFor(std::chrono::milliseconds(50)); // Budget: B20.
    }
}

} // namespace frost::app
