// SPDX-License-Identifier: Apache-2.0
//
// The FrostIDE application's entry point: on Windows frost_main() in frost_app.dll, which the
// frost.exe launcher calls after hardening the process (src/app/src/launcher.cpp, ADR 0012);
// on Linux main() of the frost executable, which hardens the process itself. Both run the
// same body (run() below).
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
// abort. Qt's own fatal errors in the GUI still end the process with 0xC0000409 (__fastfail)
// until M1's crash handler.

#include "frost/app/command_line.hpp"
#include "frost/app/main_window.hpp"
#include "frost/app/selftest.hpp"
#include "frost/app/startup.hpp"
#include "frost/core/build_info.hpp"

#include <QApplication>

#include <cstdio>
#include <format>
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

// The application, after the process is hardened (`hardening` says how far that got).
// `argv` must outlive the call: QApplication keeps it.
int run(const frost::app::ProcessHardening& hardening, int argc, char** argv) {
    ConsoleCodePageRestorer consoleCodePage;
    FatalHandlerCodePage fatalHandler;
    const std::vector<std::string> args = utf8Arguments(argc, argv);
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
        // Offscreen by default so the self-test runs headless (packaging smoke tests, WSL).
        if (!parsed->selfTestOnScreen && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
        frost::app::SelfTestReport report;
        report.checks = frost::app::runProcessChecks(hardening, dialogGuard);
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

    if (auto plugin = frost::app::preparePlatformPlugin(); !plugin) {
        reportStartupFailure(consoleCodePage, plugin.error().message());
        return 4;
    }
    // TODO(M1): log a failed preload (PluginPreloadReport) once frost::core has logging.
    static_cast<void>(frost::app::preloadOtherPlugins());
    // As in the self-test: Qt gets only the program name, never the user's arguments.
    int qtArgc = argc > 0 ? 1 : 0;
    QApplication app(qtArgc, argv);
    QApplication::setApplicationName(QStringLiteral("FrostIDE"));
    QApplication::setApplicationVersion(QString::fromUtf8(frost::build_info::kVersion));
    frost::app::MainWindow window;
    window.show();
    return QApplication::exec();
}

} // namespace

#ifdef _WIN32

/*
 * frost_app.dll's entry point (frost::app::kAppEntryPoint), called once by the frost.exe
 * launcher on its main thread after hardenProcess() and searchInstallDirectoryExtended();
 * `hardeningBits` is encodeHardening() of the launcher's state. Returns the exit status, which
 * the launcher returns from wmain. Qt's one-element argv is frost.exe's path as
 * GetModuleFileNameW reports it, without a \\?\ prefix. The old executable's CRT passed the
 * command line's first token instead, as the parent spelled it (possibly relative); Qt takes
 * applicationFilePath() and applicationDirPath() from GetModuleFileNameW in both cases.
 */
extern "C" __declspec(dllexport) int frost_main(unsigned hardeningBits) {
    const frost::app::ProcessHardening hardening = frost::app::decodeHardening(hardeningBits);
    std::string program = toUtf8(frost::app::withoutExtendedPrefix(frost::app::modulePath()));
    char* argv[] = {program.data(), nullptr};
    return run(hardening, 1, argv);
}
static_assert(std::is_same_v<decltype(&frost_main), frost::app::AppEntryPoint>,
              "frost_main must match the launcher's frost::app::AppEntryPoint");

#else

int main(int argc, char** argv) {
    // Before any other code can load a library or start a process.
    const frost::app::ProcessHardening hardening = frost::app::hardenProcess();
    return run(hardening, argc, argv);
}

#endif
