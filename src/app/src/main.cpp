// SPDX-License-Identifier: Apache-2.0
//
// The FrostIDE application's entry point: on Windows frost_main() in frost_app.dll, which the
// frost.exe launcher calls after hardening the process (src/app/src/launcher.cpp, ADR 0012),
// possibly in a process it restarted through a short link to the install directory because
// the GPU driver cannot start from frost.exe's own path; on Linux main() of the frost
// executable, which hardens the process itself. Both run the same body (run() below).
//
// Start-up order matters for the budgets in PLAN.md section 4 (B1 first frame):
// arguments are parsed before Qt's GUI exists, so --version and --help never initialise
// the GUI stack: no QApplication, no platform plugin. They still need every static import of
// frost_app.dll (Qt6Core, Qt6Gui and Qt6Widgets), and they call qVersion(), install the fatal
// message handler (qInstallMessageHandler) and the dialog hook (SetWindowsHookExW).
// --selftest initialises the GUI stack (offscreen by default) only after its process checks.
// QApplication is constructed only after preparePlatformPlugin() has pinned the platform
// plugin or reported why it cannot, in every mode; only where QT_PLUGIN_PATH,
// QT_QPA_PLATFORM_PLUGIN_PATH or qt.conf may supply a plugin frost did not find is the load
// left to Qt, which may then fail (frost/app/startup.hpp). Qt gets only the program name, so
// no argument (not even one after "--") becomes a Qt option such as -platform. Command-line
// modes (usage errors, --version, --help, --selftest) never show a dialog:
// suppressCrashDialogs(), guardCommandLineDialogs() and installCommandLineFatalHandler() run
// before anything else there, and the launcher has already set an error mode without the
// loader's boxes.
//
// Exit status: 0 success, 1 self-test failed, 2 usage error, 4 frost cannot start in this
// environment: Qt's platform plugin is missing or cannot be loaded, or the launcher cannot
// load frost_app.dll or its DLLs (the GUI also shows the reason in a message box), or Qt
// reported a fatal error in a command-line mode (the message is on stderr). 3 is avoided
// because the Windows CRT's abort() exits with it, so a refusal is never mistaken for an
// abort. --selftest=crash ends with its deliberate crash (Windows 0xC0000005, Linux SIGSEGV)
// once the crash handler is ready, and with 1 if the handler cannot start. Qt's fatal errors
// in the GUI end the process through the crash reporter (frost::diag, ADR 0022): a crash report,
// then exit code 0xE0464154 on Windows or SIGABRT on Linux.
//
// Diagnostics (frost/app/diagnostics_qt.hpp): the GUI installs the fatal handlers and starts the
// log at once, and the stall watchdog and the crash handler only after the first frame, on the
// GUI thread and the "frost-diag" thread, so neither delays B1.

#include "frost/app/command_line.hpp"
#include "frost/app/diagnostics_qt.hpp"
#include "frost/app/main_window.hpp"
#include "frost/app/selftest.hpp"
#include "frost/app/startup.hpp"
#include "frost/app/workbench.hpp"
#include "frost/build/build_service.hpp"
#include "frost/build/run_service.hpp"
#include "frost/core/assert.hpp"
#include "frost/core/build_info.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/log.hpp"
#include "frost/diag/selftest_faults.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/engine_automation.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/settings/project_layout.hpp"
#include "frost/settings/registry.hpp"
#include "frost/settings/store.hpp"
#include "frost/ui/appearance_manager.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/tr.hpp"

#include "engine_request_helper.hpp"
#include "project_open_broker.hpp"
#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTimer>

#include <array>
#include <chrono>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#endif

namespace {

#ifdef _WIN32

// UTF-8 of a Win32 string (unpaired surrogates become U+FFFD).
std::string toUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), bytes, nullptr,
                            nullptr);
    }
    return out;
}

#endif

// Returns the process arguments (without the program name) as UTF-8.
// On Windows they come from the wide command line, so they are correct even if
// the UTF-8 code page from the manifest is not in effect.
std::vector<std::string> utf8Arguments([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) {
        return args;
    }
    for (int i = 1; i < count; ++i) {
        args.push_back(toUtf8(wide[i]));
    }
    LocalFree(wide);
#else
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
#endif
    return args;
}

// Restores the attached console's output code page when run() returns. The
// console belongs to the parent shell, so leaving it on UTF-8 would change the
// user's terminal after frost exits. Lives in run()'s frame (no globals).
struct ConsoleCodePageRestorer {
    unsigned previous = 0; // 0: nothing to restore.
    ConsoleCodePageRestorer() = default;
    ConsoleCodePageRestorer(const ConsoleCodePageRestorer&) = delete ("restores exactly once");
    ConsoleCodePageRestorer& operator=(const ConsoleCodePageRestorer&) = delete ("restores exactly once");
    ~ConsoleCodePageRestorer() {
#ifdef _WIN32
        if (previous != 0) {
            std::fflush(stdout);
            std::fflush(stderr);
            SetConsoleOutputCP(previous);
        }
#endif
    }
};

// Drops the fatal handler's pointer to a ConsoleCodePageRestorer before that object dies
// (declared after it in run(), so destroyed first). Qt keeps the handler installed.
struct FatalHandlerCodePage {
    bool installed = false;
    FatalHandlerCodePage() = default;
    FatalHandlerCodePage(const FatalHandlerCodePage&) = delete ("clears exactly once");
    FatalHandlerCodePage& operator=(const FatalHandlerCodePage&) = delete ("clears exactly once");
    ~FatalHandlerCodePage() {
        if (installed) {
            frost::app::installCommandLineFatalHandler(nullptr);
        }
    }
};

// frost is a GUI-subsystem program on Windows, so it has no console of its own.
// When started from a terminal without redirection, attach to the parent's
// console so --version and --selftest output is visible. Redirected handles
// (pipes, files) already work and are left alone. The console's previous code
// page is recorded in `restorer`.
void attachParentConsoleIfNeeded([[maybe_unused]] ConsoleCodePageRestorer& restorer) {
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) {
        return;
    }
    if (restorer.previous == 0 && AttachConsole(ATTACH_PARENT_PROCESS) != FALSE) {
        FILE* stream = nullptr;
        _wfreopen_s(&stream, L"CONOUT$", L"w", stdout);
        _wfreopen_s(&stream, L"CONOUT$", L"w", stderr);
        restorer.previous = GetConsoleOutputCP();
        SetConsoleOutputCP(CP_UTF8);
    }
#endif
}

void print(FILE* stream, std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stream);
    std::fflush(stream);
}

// A detached engine launch may have no console. Report argument/forwarding errors to stderr
// when inherited, or to the user when there is no standard stream on Windows.
void reportExternalOpenFailure(ConsoleCodePageRestorer& restorer, std::string_view message) {
    attachParentConsoleIfNeeded(restorer);
#ifdef _WIN32
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    const bool hasStream = error && error != INVALID_HANDLE_VALUE && GetFileType(error) != FILE_TYPE_UNKNOWN;
#endif
    print(stderr, std::format("frost: {}\n", message));
#ifdef _WIN32
    if (!hasStream) {
        auto wide = frost::io::toWide(message);
        if (wide)
            MessageBoxW(nullptr, wide->c_str(), L"FrostIDE cannot open the script", MB_OK | MB_ICONERROR);
    }
#endif
}

// Reports why the GUI cannot start: on stderr when a console or redirection exists,
// and in a message box, because a GUI launch from Explorer has neither.
void reportStartupFailure([[maybe_unused]] ConsoleCodePageRestorer& restorer, const std::string& message) {
    attachParentConsoleIfNeeded(restorer);
    print(stderr, std::format("frost: {}\n", message));
#ifdef _WIN32
    const int units = MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(units > 0 ? units : 0), L'\0');
    if (units > 0) {
        MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), wide.data(), units);
    }
    MessageBoxW(nullptr, wide.c_str(), L"FrostIDE cannot start", MB_OK | MB_ICONERROR);
#endif
}

/*
 * What a GUI failure message adds when the launcher restarted frost through its short link
 * (`restartedFrom`, the first launcher's image path in UTF-8): the real install directory,
 * because every path frost names itself (the plugin directory among them) is the link's. ""
 * otherwise, and always on Linux.
 */
std::string restartNote([[maybe_unused]] const std::string& restartedFrom) {
#ifdef _WIN32
    if (restartedFrom.empty()) {
        return {};
    }
    const std::wstring self = frost::app::withoutExtendedPrefix(frost::app::modulePath());
    const std::string link = toUtf8(self.substr(0, self.find_last_of(L'\\')));
    const std::size_t slash = restartedFrom.find_last_of('\\');
    std::string installed = restartedFrom.substr(0, slash == std::string::npos ? 0 : slash);
    if (installed.starts_with("\\\\?\\UNC\\")) {
        installed = "\\\\" + installed.substr(8);
    } else if (installed.starts_with("\\\\?\\")) {
        installed = installed.substr(4);
    }
    return std::format("\n\nFrostIDE is installed in {}. It runs through the short link {}, because graphics drivers "
                       "end a program started from a path of {} UTF-16 units or more.",
                       installed, link, frost::app::kGpuDriverImagePathLimitUnits);
#else
    return {};
#endif
}

// --selftest=crash (ADR 0022): starts the crash handler synchronously (at most 10 s), reports
// it, then crashes in frost::diag::selfTestCrash(). Returns only when the handler cannot start.
int runCrashSelfTest(const std::string& restartedFrom) {
    frost::app::installProgramFatalHandlers();
    frost::app::startProgramLogging();
    auto status = frost::app::startProgramCrashReporter(restartedFrom, std::chrono::seconds(10));
    if (!status) {
        print(stdout, std::format("selftest: FAIL crash-reporter: {}\n", status.error().describe()));
        return 1;
    }
    print(stdout, std::format("selftest: crash-reporter ok pid={} handler={} database={}\n", status->handlerPid,
                              status->handlerPath, status->databasePath));
    frost::app::programReporter().annotate(frost::diag::AnnotationKey::SelfTest, "crash");
    print(stdout, "selftest: crashing now (write to 0xf00d)\n");
    frost::log::info("app", "selftest: crashing on purpose");
    frost::app::flushProgramLog();
    frost::diag::selfTestCrash();
}

/*
 * --selftest=freeze (ADR 0022): an offscreen QApplication (whatever QT_QPA_PLATFORM says, so
 * the GPU driver is never reached) with the stall watchdog and its hooks, no crash reporter.
 * At the first aboutToBlock() (the watchdog is armed then) a 0 ms timer blocks the GUI thread
 * in frost::diag::selfTestFreeze() for 300 ms; the self-test waits at most 10 s for the final
 * freeze report and prints its path.
 */
int runFreezeSelfTest(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    frost::app::startProgramLogging();
    if (auto plugin = frost::app::preparePlatformPlugin(); !plugin) {
        print(stdout, std::format("selftest: FAIL plugin-paths: {}\n", plugin.error().message()));
        return 1;
    }
    int qtArgc = argc > 0 ? 1 : 0;
    QApplication app(qtArgc, argv);
    auto watchdog = frost::app::startProgramWatchdog(app);
    if (!watchdog) {
        print(stdout, std::format("selftest: FAIL watchdog: {}\n", watchdog.error().describe()));
        return 1;
    }
    bool queued = false;
    QObject::connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock, &app, [&queued] {
        if (!queued) {
            queued = true;
            QTimer::singleShot(0, [] { frost::diag::selfTestFreeze(std::chrono::milliseconds(300)); });
        }
    });
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        const auto report = (*watchdog)->lastReport();
        if ((report.has_value() && !report->ongoing) || elapsed.elapsed() > 10'000) {
            QCoreApplication::quit();
        }
    });
    poll.start(10);
    QApplication::exec();
    const auto report = (*watchdog)->lastReport();
    frost::app::stopProgramWatchdog();
    frost::app::flushProgramLog();
    if (!report.has_value() || report->ongoing) {
        print(stdout, "selftest: FAIL freeze-report: no final freeze report within 10 s\n");
        return 1;
    }
    print(stdout, std::format("selftest: freeze-report {} samples={} duration-ms={}\n", report->path, report->samples,
                              report->durationMs));
    return 0;
}

/*
 * --selftest=render (ADR 0043): the real platform (QT_QPA_PLATFORM as the environment sets it, so
 * on Windows the "windows" plugin and the GPU driver), one EditorView drawn with editor.renderer's
 * default, then the checks' report (frost/app/selftest.hpp runRenderChecks).
 */
int runRenderSelfTest(int argc, char** argv) {
    frost::app::SelfTestReport report;
    if (auto plugin = frost::app::preparePlatformPlugin(); !plugin) {
        report.checks.push_back({"plugin-paths", false, true, plugin.error().message()});
        print(stdout, report.format());
        return 1;
    }
    int qtArgc = argc > 0 ? 1 : 0;
    QApplication app(qtArgc, argv);
    report.checks = frost::app::runRenderChecks();
    print(stdout, report.format());
    return report.passed() ? 0 : 1;
}

// The GUI's settings and trust stores. A store that cannot open on its file falls back to an
// in-memory one (settings) or a file in the temporary directory (trust), with a warning.
struct GuiStores {
    std::unique_ptr<frost::settings::SettingsStore> settings;
    std::unique_ptr<frost::project::TrustStore> trust;
    std::string dataDirectory; // FrostIDE's per-user data directory (the trust store's); "" if unknown.
    std::vector<std::string> warnings;
};

GuiStores openGuiStores() {
    GuiStores stores;
    frost::io::IFileSystem& fs = frost::io::realFileSystem();
    const frost::settings::Registry& registry = frost::settings::builtinRegistry();
    if (auto file = frost::settings::applicationSettingsFile(); file) {
        if (auto opened = frost::settings::SettingsStore::open(registry, fs, {*file, std::nullopt}); opened) {
            stores.settings = std::move(*opened);
        } else {
            stores.warnings.push_back(std::format("The settings file cannot be used: {}", opened.error().message()));
        }
    } else {
        stores.warnings.push_back(std::format("No settings file: {}", file.error().message()));
    }
    if (!stores.settings) {
        auto memory = frost::settings::SettingsStore::open(registry, fs, {});
        FROST_ASSERT(memory.has_value()); // No file to read: open cannot fail.
        stores.settings = std::move(*memory);
    }
    frost::Result<std::string> trustPath = frost::project::TrustStore::defaultStorePath();
    if (trustPath) {
        stores.dataDirectory = std::string(frost::io::parentPath(*trustPath));
        if (auto opened = frost::project::TrustStore::open(fs, *trustPath); opened) {
            stores.trust = std::move(*opened);
            return stores;
        }
    }
    stores.warnings.emplace_back("The trusted-projects list cannot be read; trust decisions last for this session.");
    const QByteArray fallback =
        QDir::toNativeSeparators(QDir::tempPath() + QStringLiteral("/FrostIDE-trusted-roots.txt")).toUtf8();
    auto temporary = frost::project::TrustStore::open(fs, fallback.toStdString());
    FROST_ASSERT(temporary.has_value()); // A missing file opens as an empty list.
    stores.trust = std::move(*temporary);
    return stores;
}

// Where the workbench keeps the hot-exit journal, local history, the sessions, the build's and the
// runs' state and the synthesized compile commands: FrostIDE's data directory. The file watcher
// runs (external changes, ADR 0042).
frost::app::WorkbenchOptions workbenchOptions(const GuiStores& stores) {
    frost::app::WorkbenchOptions options;
    if (!stores.dataDirectory.empty()) {
        const auto under = [&](std::string_view name) {
            auto path = frost::io::joinPath(stores.dataDirectory, name);
            return path ? std::move(*path) : std::string();
        };
        options.journalDirectory = under("hot-exit");
        options.historyPath = under("local-history.db");
        options.compileCommandsCache = under("compile-commands");
        options.sessionDirectory = under("sessions");
        options.projectEditStateDirectory = under("project-edits");
        options.cmakeStatePath = under("cmake-profiles.txt");
    }
    if (auto state = frost::build::BuildService::defaultStatePath(); state) {
        options.buildStatePath = std::move(*state);
    }
    if (auto state = frost::build::RunService::defaultStatePath(); state) {
        options.runStatePath = std::move(*state);
    }
    if (auto file = frost::settings::applicationSettingsFile(); file) {
        options.applicationSettingsFile = std::move(*file);
    }
    options.watchFiles = true;
    return options;
}

// The application, after the process is hardened (`hardening` says how far that got).
// `restartedFrom` is the first launcher's image path (UTF-8) when the launcher restarted
// frost through its short link, else "". `argv` must outlive the call: QApplication keeps it.
int run(const frost::app::ProcessHardening& hardening, const std::string& restartedFrom, int argc, char** argv) {
    ConsoleCodePageRestorer consoleCodePage;
    FatalHandlerCodePage fatalHandler;
    const std::vector<std::string> args = utf8Arguments(argc, argv);
    if (args.size() == 1 && args[0] == "--internal-engine-request") {
        std::string input;
        std::array<char, 4096> bytes{};
        while (const auto n = std::fread(bytes.data(), 1, bytes.size(), stdin)) {
            input.append(bytes.data(), n);
            if (input.size() > 16 * 1024 * 1024)
                return 1;
        }
        auto parsed = frost::lang::parseJson(input);
        if (!parsed || !parsed->isObject())
            return 1;
        auto result = frost::app::executeEngineHelper(*parsed);
        auto output = frost::lang::Json::object();
        if (result)
            output.set("ok", true).set("value", std::move(*result));
        else
            output.set("ok", false)
                .set("code", static_cast<int>(result.error().code()))
                .set("message", result.error().message());
        const std::string payload = output.dump();
        if (payload.size() > 16 * 1024 * 1024)
            return 1;
        attachParentConsoleIfNeeded(consoleCodePage);
        print(stdout, payload);
        return 0;
    }
    const std::vector<std::string_view> views(args.begin(), args.end());
    auto parsed = frost::app::parseCommandLine(views);
    // Command-line modes run unattended (tests, scripts, packaging smoke tests), so they never
    // show a dialog, and a Qt fatal error ends them with status 4 (frost/app/startup.hpp).
    bool dialogGuard = false;
    if (!parsed || parsed->mode != frost::app::Mode::Gui) {
        frost::app::suppressCrashDialogs();
        dialogGuard = frost::app::guardCommandLineDialogs();
        frost::app::installCommandLineFatalHandler(&consoleCodePage.previous);
        fatalHandler.installed = true;
    }
    if (!parsed) {
        attachParentConsoleIfNeeded(consoleCodePage);
        print(stderr, std::format("frost: {}\n\n{}", parsed.error().message(), frost::app::usageText()));
        return 2;
    }

    switch (parsed->mode) {
    case frost::app::Mode::Version:
        attachParentConsoleIfNeeded(consoleCodePage);
        print(stdout, std::format("FrostIDE {} ({}, Qt {}, {})\n", frost::build_info::kVersion,
                                  frost::build_info::kCompiler, qVersion(), frost::build_info::kSystem));
        return 0;
    case frost::app::Mode::Help:
        attachParentConsoleIfNeeded(consoleCodePage);
        print(stdout, frost::app::usageText());
        return 0;
    case frost::app::Mode::SelfTest: {
        attachParentConsoleIfNeeded(consoleCodePage);
        if (parsed->selfTestKind == frost::app::SelfTestKind::Crash) {
            return runCrashSelfTest(restartedFrom);
        }
        if (parsed->selfTestKind == frost::app::SelfTestKind::Freeze) {
            return runFreezeSelfTest(argc, argv);
        }
        if (parsed->selfTestKind == frost::app::SelfTestKind::Render) {
            return runRenderSelfTest(argc, argv);
        }
        // Offscreen by default so the self-test runs headless (packaging smoke tests, WSL).
        if (!parsed->selfTestOnScreen && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
        frost::app::SelfTestReport report;
        report.checks = frost::app::runProcessChecks(hardening, dialogGuard, restartedFrom);
        // Pin the platform plugin before QApplication exists, or report why it cannot be loaded
        // instead of letting Qt fail. Runs after QT_QPA_PLATFORM is settled, because it pins
        // that platform's plugin.
        auto plugin = frost::app::preparePlatformPlugin();
        if (!plugin) {
            report.checks.push_back({"plugin-paths", false, true, plugin.error().message()});
            print(stdout, report.format());
            return 1;
        }
        report.checks.push_back({"plugin-paths", true, true, std::move(plugin->detail)});
        frost::app::PluginPreloadReport others = frost::app::preloadOtherPlugins();
        report.checks.push_back({"other-plugins", others.ok, false, std::move(others.detail)});
        // frost parsed the arguments; paths after "--" must not become Qt options such as
        // -platform, so Qt gets only the program name. Qt keeps a reference to the count,
        // so it lives as long as `app`.
        int qtArgc = argc > 0 ? 1 : 0;
        QApplication app(qtArgc, argv);
        for (auto& check : frost::app::runGuiChecks()) {
            report.checks.push_back(std::move(check));
        }
        for (auto& check : frost::app::runLateLoadChecks()) {
            report.checks.push_back(std::move(check));
        }
        print(stdout, report.format());
        return report.passed() ? 0 : 1;
    }
    case frost::app::Mode::Gui: break;
    }

    // Fatal handlers and the log at once; everything slower waits for the first frame.
    frost::app::installProgramFatalHandlers();
    frost::app::startProgramLogging();
    frost::app::installGuiQtMessageHandler();
    frost::log::info("app", "FrostIDE {} starting", frost::build_info::kVersion);
    if (auto plugin = frost::app::preparePlatformPlugin(); !plugin) {
        frost::log::error("app", "cannot start: {}", plugin.error().message());
        frost::app::flushProgramLog();
        reportStartupFailure(consoleCodePage, plugin.error().message() + restartNote(restartedFrom));
        return 4;
    }
    if (frost::app::PluginPreloadReport others = frost::app::preloadOtherPlugins(); !others.ok) {
        // Qt skips a plugin it cannot load, so this is a warning, not a start-up failure.
        frost::log::warn("app", "plugin preload: {}", others.detail);
    }
    int status = 0;
    {
        // As in the self-test: Qt gets only the program name, never the user's arguments.
        int qtArgc = argc > 0 ? 1 : 0;
        QApplication app(qtArgc, argv);
        QApplication::setApplicationName(QStringLiteral("FrostIDE"));
        QApplication::setApplicationVersion(QString::fromUtf8(frost::build_info::kVersion));
        // Reserve the explicit project's endpoint before opening any windows. A second launch
        // waits for its first frame and forwards there rather than touching its dirty documents.
        std::string externalRoot;
        std::string externalFile;
        std::unique_ptr<frost::io::LocalIpcOwner> reserved;
        if (!parsed->project.empty()) {
            if (parsed->paths.size() > 1) {
                reportExternalOpenFailure(consoleCodePage, "--project accepts at most one file");
                return 2;
            }
            auto root = frost::app::externalProjectRoot(parsed->project);
            if (!root) {
                reportExternalOpenFailure(consoleCodePage, root.error().message());
                return 2;
            }
            externalRoot = std::move(*root);
            if (!parsed->paths.empty()) {
                const QFileInfo file(
                    QString::fromUtf8(parsed->paths[0].data(), static_cast<qsizetype>(parsed->paths[0].size())));
                const QString suffix = file.suffix().toLower();
                if (!file.isFile() || suffix == QStringLiteral("frostproj") || suffix == QStringLiteral("sln") ||
                    suffix == QStringLiteral("slnx")) {
                    reportExternalOpenFailure(consoleCodePage,
                                              "external script does not exist or is not an editor document");
                    return 2;
                }
                externalFile = QDir::toNativeSeparators(file.absoluteFilePath()).toUtf8().toStdString();
            }
            auto owner = frost::io::LocalIpcOwner::claim(frost::app::externalProjectKey(externalRoot));
            if (!owner) {
                if (owner.error().code() == frost::ErrorCode::Busy) {
                    auto sent = frost::app::forwardExternalOpen(externalRoot, externalFile, parsed->line,
                                                                parsed->column, parsed->columnUtf16);
                    if (sent)
                        return 0;
                    reportExternalOpenFailure(consoleCodePage,
                                              std::format("cannot contact project window: {}", sent.error().message()));
                    return 2;
                }
                reportExternalOpenFailure(consoleCodePage,
                                          std::format("cannot reserve project window: {}", owner.error().message()));
                return 2;
            }
            reserved = std::move(*owner);
        }
        // The workbench and what it borrows outlive the window (declared first, destroyed last).
        GuiStores stores = openGuiStores();
        frost::app::DialogWorkbenchPrompts prompts;
        frost::app::Workbench workbench(
            frost::app::WorkbenchServices{frost::io::realFileSystem(), frost::io::realProcessLauncher(),
                                          frost::systemClock(), *stores.settings, *stores.trust, prompts},
            workbenchOptions(stores));
        // The appearance comes from the resources deployed next to frost (ADR 0015). Without them the
        // window still opens on Qt's own style and says why (ADR 0038), instead of not starting.
        const QByteArray resources = (QCoreApplication::applicationDirPath() + QStringLiteral("/resources")).toUtf8();
        const auto appearance = workbench.installAppearance(app, resources.toStdString());
        frost::app::MainWindow window(workbench.environment());
        workbench.attach(window);
        if (!appearance) {
            window.notifications().notify({frost::ui::qtr("frost::app", "The theme could not be loaded"),
                                           QString::fromUtf8(appearance.error().message()),
                                           frost::ui::NotificationKind::Error,
                                           std::nullopt,
                                           {}});
        } else {
            window.reportStartupWarnings(appearance->warnings);
        }
        if (frost::ui::AppearanceManager* manager = workbench.appearance(); manager != nullptr) {
            QObject::connect(manager, &frost::ui::AppearanceManager::appearanceApplyFailed, &window,
                             [&window](const QString& description) {
                                 window.notifications().notify(
                                     {frost::ui::qtr("frost::app", "The appearance could not be changed"),
                                      description,
                                      frost::ui::NotificationKind::Warning,
                                      std::nullopt,
                                      {}});
                             });
        }
        for (const std::string& warning : stores.warnings) {
            window.notifications().notify({frost::ui::qtr("frost::app", "Settings"),
                                           QString::fromStdString(warning),
                                           frost::ui::NotificationKind::Warning,
                                           std::nullopt,
                                           {}});
        }
        /*
         * A folder or solution named on the command line opens as the project, then files open in
         * tabs (ADR 0042). Without a project argument the last project and its tabs come back
         * (general.reopenLastProject), else the Welcome screen shows.
         */
        std::vector<std::string> projects;
        std::vector<std::string> files;
        for (const std::string& path : parsed->paths) {
            const QFileInfo file(QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())));
            std::string absolute = QDir::toNativeSeparators(file.absoluteFilePath()).toUtf8().toStdString();
            const QString suffix = file.suffix().toLower();
            const bool cmakeProject = parsed->project.empty() && parsed->line == 0 && parsed->column == 0 &&
                                      file.isFile() && file.fileName() == QStringLiteral("CMakeLists.txt");
            const bool project =
                cmakeProject || file.isDir() ||
                (file.isFile() && (suffix == QStringLiteral("sln") || suffix == QStringLiteral("slnx") ||
                                   suffix == QStringLiteral("frostproj")));
            (project ? projects : files).push_back(std::move(absolute));
        }
        if (!parsed->project.empty()) {
            const QFileInfo project(
                QString::fromUtf8(parsed->project.data(), static_cast<qsizetype>(parsed->project.size())));
            const std::string absolute = QDir::toNativeSeparators(project.absoluteFilePath()).toUtf8().toStdString();
            if (auto opened = workbench.openExternalProject(absolute); !opened) {
                reportExternalOpenFailure(consoleCodePage,
                                          std::format("cannot open project: {}", opened.error().message()));
                return 2;
            }
        } else {
            for (const std::string& path : projects) {
                const QFileInfo info(QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())));
                if (info.isFile() && info.fileName() == QStringLiteral("CMakeLists.txt"))
                    static_cast<void>(workbench.openCMakeProject(path));
                else
                    static_cast<void>(workbench.openPath(path));
            }
            if (projects.empty())
                static_cast<void>(workbench.restoreLastProject());
        }
        for (const std::string& path : files) {
            auto opened =
                frost::app::openExternalFile(workbench, path, parsed->line, parsed->column, parsed->columnUtf16);
            if (!opened && !parsed->project.empty()) {
                reportExternalOpenFailure(consoleCodePage,
                                          std::format("cannot open file: {}", opened.error().message()));
                return 2;
            }
        }
        auto brokerRoot = frost::app::externalProjectRoot(workbench.projectRoot());
        if (!reserved && brokerRoot) {
            auto owner = frost::io::LocalIpcOwner::claim(frost::app::externalProjectKey(*brokerRoot));
            if (owner)
                reserved = std::move(*owner);
        }
        frost::app::ProjectOpenBroker broker(workbench, window, brokerRoot ? *brokerRoot : std::string{},
                                             std::move(reserved));
        QObject::connect(&workbench, &frost::app::Workbench::projectChanged, &broker,
                         [&broker](const QString&) { broker.projectChanged(); });
        window.show();
        frost::app::afterFirstFrame(window, [&app, &window, &workbench, &broker, restartedFrom] {
            if (auto started = broker.start(); !started)
                frost::log::warn("app", "cannot start project-open IPC: {}", started.error().message());
            // FrostProcess.GuiStartsTheCrashHandlerAfterTheFirstFrame orders this line before the
            // handler's "crash handler registered" line.
            frost::log::info("app", "first frame shown");
            if (auto watchdog = frost::app::startProgramWatchdog(app); !watchdog) {
                frost::log::warn("diag", "no stall watchdog: {}", watchdog.error().describe());
            }
            frost::app::startProgramDiagnosticsThread(restartedFrom);
            // Unsaved files of an earlier session: asked once the window is up (the scan may wait
            // for other instances' journals, so never before the first frame).
            QTimer::singleShot(0, &window, [&workbench] { static_cast<void>(workbench.offerRecovery()); });
        });
        status = QApplication::exec();
        // Autosave and the hot-exit journal while the window's documents are still open.
        workbench.shutdown();
        // The watchdog and its hooks go before the window and QApplication (declared after it).
        frost::app::stopProgramWatchdog();
    }
    frost::log::info("app", "FrostIDE exits with {}", status);
    frost::app::flushProgramLog(); // Budget: B20.
    return status;
}

} // namespace

#ifdef _WIN32

/*
 * frost_app.dll's entry point (frost::app::kAppEntryPoint), called once by the frost.exe
 * launcher on its main thread after hardenProcess() and searchInstallDirectoryExtended();
 * `hardeningBits` is encodeHardening() of the launcher's state, `restartedFrom` the image path
 * of the launcher that restarted this one through its short link, which the launcher checked
 * to name this process's own image file, or nullptr (frost::app::AppEntryPoint; ADR 0012).
 * Returns the exit status, which the launcher returns from wmain. Qt's one-element argv is
 * frost.exe's path as GetModuleFileNameW reports it, without a \\?\ prefix (after a restart,
 * the short link's). The old executable's CRT passed the command line's first token instead, as
 * the parent spelled it (possibly relative); Qt takes applicationFilePath() and
 * applicationDirPath() from GetModuleFileNameW in both cases.
 * Crash reports record `restartedFrom` (the frost.restarted_from annotation); logs and dumps
 * live under the cache root, not the install directory.
 * TODO(M1): code that starts frost again takes the install directory from `restartedFrom` when it
 * is set.
 * TODO(M16): so does the code that registers frost (file associations, I10), and frost sets an
 * explicit AppUserModelID with the Jump List, so a restarted frost groups with a shortcut to its
 * long path (both moved to M16 by ADR 0029).
 */
extern "C" __declspec(dllexport) int frost_main(unsigned hardeningBits, const wchar_t* restartedFrom) {
    const frost::app::ProcessHardening hardening = frost::app::decodeHardening(hardeningBits);
    std::string program = toUtf8(frost::app::withoutExtendedPrefix(frost::app::modulePath()));
    char* argv[] = {program.data(), nullptr};
    return run(hardening, restartedFrom != nullptr ? toUtf8(restartedFrom) : std::string(), 1, argv);
}
static_assert(std::is_same_v<decltype(&frost_main), frost::app::AppEntryPoint>,
              "frost_main must match the launcher's frost::app::AppEntryPoint");

#else

int main(int argc, char** argv) {
    // Before any other code can load a library or start a process.
    const frost::app::ProcessHardening hardening = frost::app::hardenProcess();
    return run(hardening, {}, argc, argv);
}

#endif
