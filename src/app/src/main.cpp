// SPDX-License-Identifier: Apache-2.0
//
// Entry point of frost, the FrostIDE executable.
//
// Start-up order matters for the budgets in PLAN.md section 4 (B1 first frame):
// arguments are parsed before Qt exists, so --version, --help and --selftest
// never initialise the GUI stack they do not need. The process is hardened
// (frost/app/startup.hpp) before anything else can load a DLL or start a process.
//
// Exit status: 0 success, 1 self-test failed, 2 usage error, 4 frost cannot start in this
// environment (e.g. Qt cannot load its platform plugin from the install directory; the
// GUI also shows the reason in a message box). 3 is avoided because the Windows CRT's
// abort() exits with it, so a refusal is never mistaken for an abort; Qt's own fatal
// errors end the process with 0xC0000409 (__fastfail) instead.

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
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#endif

namespace {

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
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(static_cast<std::size_t>(bytes > 0 ? bytes - 1 : 0), '\0');
        if (bytes > 1) {
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, arg.data(), bytes, nullptr, nullptr);
        }
        args.push_back(std::move(arg));
    }
    LocalFree(wide);
#else
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
#endif
    return args;
}

// Restores the attached console's output code page when main returns. The
// console belongs to the parent shell, so leaving it on UTF-8 would change the
// user's terminal after frost exits. Lives in main's frame (no globals).
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

} // namespace

int main(int argc, char** argv) {
    const frost::app::ProcessHardening hardening = frost::app::hardenProcess();
    ConsoleCodePageRestorer consoleCodePage;
    const std::vector<std::string> args = utf8Arguments(argc, argv);
    const std::vector<std::string_view> views(args.begin(), args.end());
    auto parsed = frost::app::parseCommandLine(views);
    // Command-line modes run unattended (tests, scripts, packaging smoke tests).
    if (!parsed || parsed->mode != frost::app::Mode::Gui) {
        frost::app::suppressCrashDialogs();
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
        report.checks = frost::app::runProcessChecks(hardening);
        // Without a loadable platform plugin QApplication would abort, so stop with a report
        // instead. Runs after QT_QPA_PLATFORM is settled, because it checks that plugin.
        auto plugins = frost::app::checkPluginPaths();
        if (!plugins) {
            report.checks.push_back({"plugin-paths", false, true, plugins.error().message()});
            print(stdout, report.format());
            return 1;
        }
        report.checks.push_back({"plugin-paths", true, true, std::move(plugins->detail)});
        if (!plugins->warning.empty()) {
            report.checks.push_back({"other-plugin-paths", false, false, std::move(plugins->warning)});
        }
        QApplication app(argc, argv);
        for (auto& check : frost::app::runGuiChecks()) {
            report.checks.push_back(std::move(check));
        }
        print(stdout, report.format());
        return report.passed() ? 0 : 1;
    }
    case frost::app::Mode::Gui: break;
    }

    // TODO(M1): log PluginPathReport::warning once frost::core has logging.
    if (auto plugins = frost::app::checkPluginPaths(); !plugins) {
        reportStartupFailure(consoleCodePage, plugins.error().message());
        return 4;
    }
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("FrostIDE"));
    QApplication::setApplicationVersion(QString::fromUtf8(frost::build_info::kVersion));
    frost::app::MainWindow window;
    window.show();
    return QApplication::exec();
}
