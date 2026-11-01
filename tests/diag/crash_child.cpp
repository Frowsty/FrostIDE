// SPDX-License-Identifier: Apache-2.0
//
// crash_child: the helper program of the crash integration tests (crash_child_test.cpp and
// frost_process_tests). It sets up frost::diag's fatal paths exactly as frost's program does
// (last-chance handlers on Linux, then installFatalHandlers), starts a real crashpad handler
// beside itself writing to <database> (except in inactive-reporter and last-chance), prints
//   "ready handler-pid=<pid>"
// on stdout, and then runs one fatal path:
//
//   av-main                  selfTestCrash() on the main thread (write to 0xF00D)
//   av-worker                selfTestCrash() on a std::thread
//   stack-overflow           unbounded recursion on the main thread
//   contract-enforce         a FROST_PRE violation under enforce (contractEnforceSite)
//   contract-quick           a FROST_PRE violation under quick_enforce (crash_child_quick.cpp)
//   terminate-uncaught-on-worker  an exception leaves a thread that installed the per-thread handlers
//   abort                    std::abort()
//   invalid-parameter        Windows: a CRT call with an invalid descriptor
//   purecall                 Windows: a pure virtual call from a base constructor
//   inactive-reporter        a contract violation with the handlers installed but no handler started
//   last-chance              Linux: only the last-chance handlers, then a SIGSEGV; a probe handler
//                            chained in front prints "dumpable=<PR_GET_DUMPABLE>" after the
//                            last-chance handler ran and before the signal is delivered again
//   exit                     returns 0 after the ready line
//   wait-stdin               prints the ready line, then waits until stdin ends, then returns 0
//   dump-without-crash       ICrashReporter::dumpWithoutCrash("crash_child"), then returns 0
//   pipe-security            Windows: prints "sddl=<the handler pipe's DACL and label>", returns 0
//   settings                 prints "uploads-enabled=<0|1>" from the database's settings, returns 0
//
//   crash_child <mode> <database> [--desktop <name>]
// Exit status 2 on usage errors, 1 when the reporter cannot start.

#include "frost/core/assert.hpp"
#include "frost/core/clock.hpp"
#include "frost/diag/crash_reporter.hpp"
#include "frost/diag/fatal_handlers.hpp"
#include "frost/diag/selftest_faults.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include <client/crash_report_database.h>
#include <client/settings.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>

#include <aclapi.h>
#include <crtdbg.h>
#include <io.h>
#include <sddl.h>
#else
#include <sys/prctl.h>

#include <csignal>

#include <signal.h>
#include <unistd.h>
#endif

namespace frost::test {
// crash_child_quick.cpp: a FROST_PRE under quick_enforce.
void contractQuickSite(int value);
} // namespace frost::test

namespace {

void say(std::string_view line) { std::cout << line << '\n' << std::flush; }

// This executable's directory (UTF-8).
std::string ownDirectory() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    std::string utf8 = frost::io::fromWide(path).value_or(std::string());
    if (utf8.starts_with("\\\\?\\")) {
        utf8.erase(0, 4);
    }
    return std::string(frost::io::parentPath(utf8));
#else
    std::string path(4096, '\0');
    const ssize_t length = ::readlink("/proc/self/exe", path.data(), path.size());
    path.resize(length > 0 ? static_cast<std::size_t>(length) : 0);
    return std::string(frost::io::parentPath(path));
#endif
}

// A precondition the contract-enforce mode violates; frost.frames must name this function.
[[clang::noinline]] void contractEnforceSite(int value) {
    FROST_PRE(value >= 0); // FROST-SELFTEST-CONTRACT-SITE
    std::printf("not reached %d\n", value);
}

volatile int g_depth = 0;

// Recurses until the stack is gone; the volatile buffer keeps every frame large and alive.
[[clang::noinline]] int recurse(int depth) {
    volatile char buffer[4096];
    buffer[0] = static_cast<char>(depth);
    g_depth = depth;
    if (depth >= 0) {
        return recurse(depth + 1) + buffer[0];
    }
    return buffer[0];
}

#ifdef _WIN32
// A pure virtual call: the base constructor reaches pure() through a non-virtual member, while
// the object is still a Base.
struct PureBase {
    PureBase() { callPure(); }
    virtual ~PureBase() = default;
    [[clang::noinline]] void callPure() { pure(); }
    virtual void pure() = 0;
};
struct PureDerived final : PureBase {
    void pure() override {}
};

// Prints the handler pipe's DACL and label (SDDL), read through a client handle opened with
// READ_CONTROL only.
int printPipeSecurity(const std::string& pipeName) {
    const std::wstring wide = frost::io::toWide(pipeName).value();
    const HANDLE pipe = CreateFileW(wide.c_str(), READ_CONTROL, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        say("pipe-open-failed " + std::to_string(GetLastError()));
        return 1;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD error = GetSecurityInfo(pipe, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
                                        nullptr, nullptr, nullptr, nullptr, &descriptor);
    CloseHandle(pipe);
    if (error != ERROR_SUCCESS) {
        say("get-security-failed " + std::to_string(error));
        return 1;
    }
    LPWSTR text = nullptr;
    const BOOL converted = ConvertSecurityDescriptorToStringSecurityDescriptorW(
        descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION, &text, nullptr);
    LocalFree(descriptor);
    if (converted == FALSE) {
        say("convert-failed");
        return 1;
    }
    say("sddl=" + frost::io::fromWide(text).value_or("?"));
    LocalFree(text);
    return 0;
}
#else
struct sigaction g_lastChance{};

// Chained in front of the last-chance handler: runs it, then reports the dumpable flag it left;
// the signal it raised again is delivered when this handler returns.
void probeHandler(int signal, siginfo_t* info, void* context) {
    g_lastChance.sa_sigaction(signal, info, context);
    char line[32] = "dumpable=?\n";
    line[9] = static_cast<char>('0' + ::prctl(PR_GET_DUMPABLE, 0, 0, 0, 0));
    (void)!::write(STDOUT_FILENO, line, std::strlen(line));
}
#endif

int usage() {
    std::fputs("usage: crash_child <mode> <database> [--desktop <name>]\n", stderr);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 5) {
        return usage();
    }
#ifdef _WIN32
    // The debug CRT reports an invalid parameter with an assertion box before it calls the
    // handler; the report goes to stderr instead (release CRTs have no such box).
    (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    const std::string_view mode = argv[1];
    const std::string database = argv[2];
    std::string desktop;
    if (argc == 5) {
        if (std::string_view(argv[3]) != "--desktop") {
            return usage();
        }
        desktop = argv[4];
    }

#ifndef _WIN32
    if (mode == "last-chance") {
        frost::diag::installLastChanceHandlers();
        struct sigaction probe{};
        probe.sa_sigaction = &probeHandler;
        probe.sa_flags = SA_SIGINFO;
        sigemptyset(&probe.sa_mask);
        (void)::sigaction(SIGSEGV, &probe, &g_lastChance);
        say("ready handler-pid=0");
        frost::diag::selfTestCrash();
    }
#endif

    // As frost's program: last-chance handlers first, so crashpad chains to them.
    frost::diag::installLastChanceHandlers();
    std::unique_ptr<frost::diag::ICrashReporter> reporter =
        frost::diag::createCrashpadReporter(frost::io::realProcessLauncher(), frost::systemClock());
    frost::diag::installFatalHandlers(*reporter);

    if (mode == "inactive-reporter") {
        say("ready handler-pid=0");
        contractEnforceSite(-1);
        return 0;
    }

    frost::diag::CrashReporterConfig config;
    config.handlerPath = frost::io::joinPath(ownDirectory(), "crashpad_handler"
#ifdef _WIN32
                                                             ".exe"
#endif
                                             )
                             .value();
    config.databasePath = database;
    config.version = "0.0.0-crash-child";
    config.desktop = desktop;
    if (auto started = reporter->start(config); !started) {
        say("start-failed " + started.error().describe());
        return 1;
    }
    reporter->annotate(frost::diag::AnnotationKey::ImagePath, "crash_child");
    const frost::diag::ReporterStatus status = reporter->status();
    say("ready handler-pid=" + std::to_string(status.handlerPid));

    if (mode == "av-main") {
        frost::diag::selfTestCrash();
    } else if (mode == "av-worker") {
        std::thread([] { frost::diag::selfTestCrash(); }).join();
    } else if (mode == "stack-overflow") {
        return recurse(0);
    } else if (mode == "contract-enforce") {
        contractEnforceSite(-1);
    } else if (mode == "contract-quick") {
        frost::test::contractQuickSite(-1);
    } else if (mode == "terminate-uncaught-on-worker") {
        std::thread([] {
            frost::diag::installThreadFatalHandlers(); // As every thread frost::diag starts.
            throw std::runtime_error("crash_child: uncaught on a worker");
        }).join();
    } else if (mode == "abort") {
        std::abort();
#ifdef _WIN32
    } else if (mode == "invalid-parameter") {
        (void)_get_osfhandle(-1);
    } else if (mode == "purecall") {
        const PureDerived derived;
        (void)derived;
    } else if (mode == "pipe-security") {
        return printPipeSecurity(status.pipeName);
#endif
    } else if (mode == "exit") {
        return 0;
    } else if (mode == "wait-stdin") {
        std::string ignored;
        while (std::getline(std::cin, ignored)) {
        }
        return 0;
    } else if (mode == "dump-without-crash") {
        if (auto dumped = reporter->dumpWithoutCrash("crash_child"); !dumped) {
            say("dump-failed " + dumped.error().describe());
            return 1;
        }
        say("dumped");
        return 0;
    } else if (mode == "settings") {
#ifdef _WIN32
        const base::FilePath path(frost::io::toWin32Path(database).value());
#else
        const base::FilePath path(database);
#endif
        // On Linux the client registers its socket without waiting for the handler, which
        // creates the database only once it runs: wait for it (10 s at most).
        auto opened = crashpad::CrashReportDatabase::InitializeWithoutCreating(path);
        for (int attempt = 0; !opened && attempt < 1000; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            opened = crashpad::CrashReportDatabase::InitializeWithoutCreating(path);
        }
        bool enabled = true;
        if (!opened || !opened->GetSettings()->GetUploadsEnabled(&enabled)) {
            say("settings-unreadable");
            return 1;
        }
        say(std::string("uploads-enabled=") + (enabled ? "1" : "0"));
        return 0;
    } else {
        return usage();
    }
    say("fatal path returned");
    return 3;
}
