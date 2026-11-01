// SPDX-License-Identifier: Apache-2.0
//
// The fatal paths end to end (ADR 0022, D-G): crash_child starts a real crashpad handler and
// dies on one path; each case checks the exit status, that exactly one finished report is in
// the database and none is left unfinished, and, through tests/support/minidump_reader.hpp
// (an independent reader of Microsoft's minidump layout and crashpad's documented extension
// stream), the exception code, the fault address and frost's annotations. Also: the handler
// never outlives its client, the Windows handler pipe admits only this user, the Linux
// database is private, uploads stay disabled, and a fatal path without a running handler ends
// the process without any report (nothing reaches Windows Error Reporting or a core file).

#include "frost/diag/crash_reporter.hpp"
#include "frost/diag/selftest_faults.hpp"

#include "diag_test_support.hpp"
#include "support/minidump_reader.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>

#include <sddl.h>
#else
#include <sys/stat.h>

#include <csignal>
#endif

namespace {

using namespace std::chrono_literals;
using frost::test::ChildRun;
using frost::test::ScratchDir;

std::string crashChild() { return frost::io::normalizePath(FROST_CRASH_CHILD).value(); }

// What one fatal path must leave behind.
struct Expected {
    std::uint32_t code; // Windows exception code / Linux signal number in the dump.
    std::optional<std::uint64_t> faultAddress;
    std::uint32_t exitStatus;     // Windows: the process exit code.
    int signal;                   // Linux: the signal that ends the process.
    std::string_view fatalReason; // frost.fatal_reason, "" when crashpad saw the fault itself.
};

#ifdef _WIN32
constexpr std::uint32_t kAccessViolation = 0xC0000005u;
constexpr std::uint32_t kIllegalInstruction = 0xC000001Du;
constexpr std::uint32_t kStackOverflow = 0xC00000FDu;
constexpr std::uint32_t kFatal = frost::diag::kFatalExceptionCode;
constexpr std::uint32_t kFatalAppExit = 0x40000015u; // STATUS_FATAL_APP_EXIT
#endif

struct Case {
    std::string_view mode;
    Expected expected;
};

std::string describe(const ChildRun& run) {
    return std::format("exit {} (0x{:08x}) signal {} error '{}'\n{}", run.exitCode,
                       static_cast<std::uint32_t>(run.exitCode), run.signal, run.error, run.output);
}

// Runs `mode` and checks everything Expected says, returning the dump for further checks.
std::optional<frost::test::MinidumpReader> runFatal(const ScratchDir& scratch, const Case& test) {
    const std::string database = scratch.join("crashpad");
    const ChildRun run =
        frost::test::runChild(crashChild(), {std::string(test.mode), database}, frost::test::crashEnvironment());
    EXPECT_TRUE(run.output.contains("ready handler-pid=")) << describe(run);
#ifdef _WIN32
    EXPECT_EQ(static_cast<std::uint32_t>(run.exitCode), test.expected.exitStatus) << describe(run);
#else
    EXPECT_EQ(run.signal, test.expected.signal) << describe(run);
#endif
    const frost::test::DatabaseReports reports = frost::test::waitForReports(database, 1);
    EXPECT_TRUE(reports.unfinished.empty()) << reports.unfinished.size() << " unfinished reports";
    if (reports.finished.size() != 1) {
        ADD_FAILURE() << reports.finished.size() << " finished reports, expected 1; " << describe(run);
        return std::nullopt;
    }
    frost::test::MinidumpReader dump(frost::test::readBytes(reports.finished.front()));
    if (!dump.error().empty()) {
        ADD_FAILURE() << "unreadable minidump: " << dump.error();
        return std::nullopt;
    }
    if (!dump.exception().has_value()) {
        ADD_FAILURE() << "the minidump has no exception stream";
        return std::nullopt;
    }
    const frost::test::MinidumpException& exception = *dump.exception();
    EXPECT_EQ(exception.code, test.expected.code) << std::format("code 0x{:08x}", exception.code);
    if (test.expected.faultAddress.has_value()) {
#ifdef _WIN32
        // EXCEPTION_ACCESS_VIOLATION: information[0] is 1 for a write, [1] the address.
        if (exception.information.size() < 2) {
            ADD_FAILURE() << "the access violation has " << exception.information.size() << " parameters";
        } else {
            EXPECT_EQ(exception.information[0], 1u);
            EXPECT_EQ(exception.information[1], *test.expected.faultAddress);
        }
#else
        // crashpad's Linux snapshot records si_addr as the exception address.
        EXPECT_EQ(exception.address, *test.expected.faultAddress) << std::format("0x{:x}", exception.address);
#endif
    }
    const auto& annotations = dump.annotations();
    EXPECT_EQ(dump.simpleAnnotations().contains("prod") ? dump.simpleAnnotations().at("prod") : "", "FrostIDE");
    EXPECT_EQ(annotations.contains("frost.image_path") ? annotations.at("frost.image_path") : "", "crash_child");
    if (!test.expected.fatalReason.empty()) {
        EXPECT_EQ(annotations.contains("frost.fatal_reason") ? annotations.at("frost.fatal_reason") : "",
                  test.expected.fatalReason);
        EXPECT_TRUE(annotations.contains("frost.frames") && annotations.at("frost.frames").starts_with("0x"));
    }
    return dump;
}

class FatalPath : public testing::TestWithParam<Case> {};

TEST_P(FatalPath, WritesExactlyOneReport) {
    const ScratchDir scratch("diag-fatal");
    (void)runFatal(scratch, GetParam());
}

// The status crashpad ends the client with, and the code in the dump, per path: crashpad's own
// handlers report the fault as the OS raised it; crashNow() reports kFatalExceptionCode on
// Windows and SIGABRT on Linux, with frost.fatal_reason naming the path.
const Case kCases[] = {
#ifdef _WIN32
    {"av-main", {kAccessViolation, 0xF00D, kAccessViolation, 0, ""}},
    {"av-worker", {kAccessViolation, 0xF00D, kAccessViolation, 0, ""}},
    {"stack-overflow", {kStackOverflow, std::nullopt, kStackOverflow, 0, ""}},
    {"contract-quick", {kIllegalInstruction, std::nullopt, kIllegalInstruction, 0, ""}},
    {"terminate-uncaught-on-worker", {kFatal, std::nullopt, kFatal, 0, "terminate"}},
    // abort() itself: crashpad's SIGABRT handler (installed when the handler registers) reports
    // STATUS_FATAL_APP_EXIT, no frost reason.
    {"abort", {kFatalAppExit, std::nullopt, kFatalAppExit, 0, ""}},
    {"invalid-parameter", {kFatal, std::nullopt, kFatal, 0, "invalid-parameter"}},
    {"purecall", {kFatal, std::nullopt, kFatal, 0, "purecall"}},
#else
    {"av-main", {SIGSEGV, 0xF00D, 0, SIGSEGV, ""}},
    {"av-worker", {SIGSEGV, 0xF00D, 0, SIGSEGV, ""}},
    {"stack-overflow", {SIGSEGV, std::nullopt, 0, SIGSEGV, ""}},
    {"contract-quick", {SIGILL, std::nullopt, 0, SIGILL, ""}},
    {"terminate-uncaught-on-worker", {SIGABRT, std::nullopt, 0, SIGABRT, "terminate"}},
    // abort() itself: crashpad's SIGABRT handler, no frost reason.
    {"abort", {SIGABRT, std::nullopt, 0, SIGABRT, ""}},
#endif
};

INSTANTIATE_TEST_SUITE_P(CrashChild, FatalPath, testing::ValuesIn(kCases),
                         [](const testing::TestParamInfo<Case>& info) {
                             std::string name;
                             for (const char c : info.param.mode) {
                                 name.push_back(c == '-' ? '_' : c);
                             }
                             return name;
                         });

TEST(CrashChild, ContractViolationNamesTheExpressionAndLocation) {
    const ScratchDir scratch("diag-contract");
#ifdef _WIN32
    const Case test{"contract-enforce", {kFatal, std::nullopt, kFatal, 0, "contract-violation"}};
#else
    const Case test{"contract-enforce", {SIGABRT, std::nullopt, 0, SIGABRT, "contract-violation"}};
#endif
    const auto dump = runFatal(scratch, test);
    ASSERT_TRUE(dump.has_value());
    const auto& annotations = dump->annotations();
    ASSERT_TRUE(annotations.contains("frost.message"));
    const std::string& message = annotations.at("frost.message");
    EXPECT_TRUE(message.contains("precondition violated: value >= 0")) << message;
    EXPECT_TRUE(message.contains("crash_child.cpp:")) << message;
}

TEST(CrashChild, InactiveReporterEndsWithoutAnyReport) {
    // The handlers are installed but no handler runs: crashNow() writes the reason to stderr and
    // ends the process itself, Windows with kFatalExceptionCode through TerminateProcess (no
    // exception, so Windows Error Reporting never sees it), Linux non-dumpable with SIGABRT.
    const ScratchDir scratch("diag-inactive");
    const std::string database = scratch.join("crashpad");
    const ChildRun run =
        frost::test::runChild(crashChild(), {"inactive-reporter", database}, frost::test::crashEnvironment());
#ifdef _WIN32
    EXPECT_EQ(static_cast<std::uint32_t>(run.exitCode), kFatal) << describe(run);
#else
    EXPECT_EQ(run.signal, SIGABRT) << describe(run);
#endif
    EXPECT_TRUE(run.output.contains("frost: fatal contract-violation: precondition violated: value >= 0"))
        << describe(run);
    EXPECT_TRUE(frost::test::listReports(database).finished.empty());
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(database)));
}

#ifndef _WIN32
TEST(Linux, LastChanceHandlerMakesTheProcessNonDumpable) {
    // The last-chance handler (installed before crashpad, so crashpad chains to it) clears the
    // dumpable flag before the signal is delivered again, so no core file reaches the system's
    // crash collector (wsl-capture-crash, apport, systemd-coredump); the process still dies by
    // the original signal.
    const ScratchDir scratch("diag-lastchance");
    const ChildRun run =
        frost::test::runChild(crashChild(), {"last-chance", scratch.join("unused")}, frost::test::crashEnvironment());
    EXPECT_EQ(run.signal, SIGSEGV) << describe(run);
    EXPECT_TRUE(run.output.contains("dumpable=0")) << describe(run);
}

TEST(CrashHandler, DatabaseDirectoriesArePrivate) {
    // diag::prepareDiagnosticsDirectories() creates crashes/ and its subdirectories 0700; crashpad
    // writes its files 0600 (it creates them with that mode).
    const ScratchDir scratch("diag-private");
    const std::string database = scratch.join("crashpad");
    ASSERT_EQ(::mkdir(database.c_str(), 0700), 0);
    const ChildRun run = frost::test::runChild(crashChild(), {"dump-without-crash", database});
    ASSERT_EQ(run.exitCode, 0) << describe(run);
    const auto reports = frost::test::waitForReports(database, 1);
    ASSERT_EQ(reports.finished.size(), 1u);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(database)) {
        struct stat info{};
        ASSERT_EQ(::lstat(entry.path().c_str(), &info), 0);
        const unsigned mode = info.st_mode & 0777u;
        if (S_ISDIR(info.st_mode)) {
            EXPECT_EQ(mode, 0700u) << entry.path();
        } else {
            EXPECT_EQ(mode & 0077u, 0u) << entry.path() << std::format(" has mode {:o}", mode);
        }
    }
}
#endif

TEST(CrashHandler, DoesNotOutliveItsClient) {
    // The handler ends with its client: Windows through the client's job (killOnClose), Linux at
    // the socket's EOF; within 2 s after a normal exit, a crash and std::terminate.
    for (const std::string_view mode : {"exit", "av-main", "terminate-uncaught-on-worker"}) {
        SCOPED_TRACE(std::string(mode));
        const ScratchDir scratch("diag-outlive");
        const ChildRun run = frost::test::runChild(crashChild(), {std::string(mode), scratch.join("crashpad")},
                                                   frost::test::crashEnvironment());
        const std::string pid = frost::test::valueAfter(run.output, "ready handler-pid");
        ASSERT_FALSE(pid.empty()) << describe(run);
        EXPECT_TRUE(frost::test::waitForProcessGone(std::stoull(pid), 2s)) << "handler " << pid << " still runs";
        (void)frost::test::waitForReports(scratch.join("crashpad"), mode == "exit" ? 0 : 1);
    }
}

TEST(CrashHandler, UploadsStayDisabled) {
    // No --url (the argument goldens in crash_reporter_test.cpp) and the database's own settings
    // say uploads are off (D26).
    const ScratchDir scratch("diag-settings");
    const std::string database = scratch.join("crashpad");
    const ChildRun run = frost::test::runChild(crashChild(), {"settings", database});
    ASSERT_EQ(run.exitCode, 0) << describe(run);
    EXPECT_TRUE(run.output.contains("uploads-enabled=0")) << describe(run);
}

#ifdef _WIN32
// This process's user SID as SDDL text ("S-1-5-21-...").
std::string currentUserSid() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return {};
    }
    alignas(TOKEN_USER) unsigned char buffer[256];
    DWORD size = 0;
    std::string text;
    if (GetTokenInformation(token, TokenUser, buffer, sizeof buffer, &size) != FALSE) {
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &sid) != FALSE) {
            text = frost::io::fromWide(sid).value_or("");
            LocalFree(sid);
        }
    }
    CloseHandle(token);
    return text;
}

TEST(CrashHandler, PipeAdmitsOnlyTheCurrentUser) {
    // frost-pipe-dacl.patch (ADR 0022): the handler pipe's DACL grants SYSTEM and this user and
    // nobody else (no AppContainer SID, no Administrators, no Everyone), and its mandatory label
    // is Medium with no-write-up (no untrusted or low-integrity client can connect).
    const ScratchDir scratch("diag-pipe");
    const ChildRun run = frost::test::runChild(crashChild(), {"pipe-security", scratch.join("crashpad")});
    ASSERT_EQ(run.exitCode, 0) << describe(run);
    const std::string sddl = frost::test::valueAfter(run.output, "sddl");
    ASSERT_FALSE(sddl.empty()) << describe(run);
    const std::string user = currentUserSid();
    ASSERT_FALSE(user.empty());
    const std::size_t dacl = sddl.find("D:");
    const std::size_t sacl = sddl.find("S:");
    ASSERT_NE(dacl, std::string::npos) << sddl;
    ASSERT_NE(sacl, std::string::npos) << sddl;
    const std::string aces = sddl.substr(dacl + 2, sacl > dacl ? sacl - dacl - 2 : std::string::npos);
    // Every ACE is an allow ACE for SYSTEM or the user.
    std::size_t count = 0;
    for (std::size_t at = aces.find('('); at != std::string::npos; at = aces.find('(', at + 1)) {
        const std::size_t end = aces.find(')', at);
        ASSERT_NE(end, std::string::npos) << sddl;
        const std::string ace = aces.substr(at + 1, end - at - 1);
        const std::string trustee = ace.substr(ace.rfind(';') + 1);
        EXPECT_TRUE(ace.starts_with("A;")) << ace;
        EXPECT_TRUE(trustee == "SY" || trustee == user) << "unexpected trustee in " << ace << " (" << sddl << ")";
        ++count;
    }
    EXPECT_GE(count, 2u) << sddl;
    EXPECT_TRUE(aces.contains(";;;SY)")) << sddl;
    EXPECT_TRUE(aces.contains(user + ")")) << sddl;
    // The SACL holds exactly one ACE, the label (its flags, such as AI, may vary).
    const std::string saclAces = sddl.substr(std::min(sddl.find('(', sacl), sddl.size()));
    EXPECT_EQ(saclAces, "(ML;;NW;;;ME)") << sddl;
}
#endif

} // namespace
