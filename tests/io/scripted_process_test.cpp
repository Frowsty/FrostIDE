// SPDX-License-Identifier: Apache-2.0
//
// Tests for ScriptedProcessLauncher, the IProcessLauncher double: scripted
// output and exit codes, recorded launches and stdin, the same validation as
// the real launcher, and deterministic hung-process behaviour.

#include "frost/io/scripted_process_launcher.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <string>

#ifndef _WIN32
#include <csignal>
#endif

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::io::ProcessSpec;
using frost::io::ScriptedProcess;
using frost::io::ScriptedProcessLauncher;
using frost::io::Stdio;

// An absolute path in this platform's syntax.
#ifdef _WIN32
const std::string kTool = "C:\\tools\\git.exe";
#else
const std::string kTool = "/usr/bin/git";
#endif

ProcessSpec gitStatus() {
    ProcessSpec spec;
    spec.executable = kTool;
    spec.arguments = {"status", "--porcelain=v2"};
    return spec;
}

TEST(ScriptedProcessLauncher, ServesScriptedOutputAndRecordsTheLaunch) {
    ScriptedProcessLauncher launcher;
    launcher.expect(
        ScriptedProcess{.expectExecutable = kTool, .exitCode = 1, .stdoutBytes = "out", .stderrBytes = "err"});
    frost::io::IProcessLauncher& iface = launcher;
    const auto result = frost::io::runToCompletion(iface, gitStatus(), "", 1s);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 1);
    EXPECT_EQ(result->stdoutBytes, "out");
    EXPECT_EQ(result->stderrBytes, "err");
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1u);
    EXPECT_TRUE(launches[0].launched);
    EXPECT_EQ(launches[0].spec.arguments, gitStatus().arguments);
    EXPECT_EQ(launcher.pendingScripts(), 0u);
}

TEST(ScriptedProcessLauncher, RecordsStdin) {
    ScriptedProcessLauncher launcher;
    launcher.expect(ScriptedProcess{});
    ProcessSpec spec = gitStatus();
    spec.stdinMode = Stdio::Pipe;
    ASSERT_TRUE(frost::io::runToCompletion(launcher, spec, "commit message\n", 1s).has_value());
    const auto record = launcher.launches().at(0);
    EXPECT_EQ(record.stdinBytes, "commit message\n");
    EXPECT_TRUE(record.stdinClosed);

    // Writing without a stdin pipe is the caller's bug, as with a real process.
    launcher.expect(ScriptedProcess{});
    const auto noPipe = frost::io::runToCompletion(launcher, gitStatus(), "data", 1s);
    ASSERT_FALSE(noPipe.has_value());
    EXPECT_EQ(noPipe.error().code(), ErrorCode::InvalidArgument);
}

TEST(ScriptedProcessLauncher, StreamingReadsAndMergedStderr) {
    ScriptedProcessLauncher launcher;
    launcher.expect(ScriptedProcess{.stdoutBytes = "abcdef", .stderrBytes = "!"});
    ProcessSpec spec = gitStatus();
    spec.stderrMode = Stdio::MergeIntoStdout;
    auto process = launcher.launch(spec);
    ASSERT_TRUE(process.has_value());
    std::array<char, 4> buffer{};
    EXPECT_EQ((*process)->readStdout(buffer).value(), 4u);
    EXPECT_EQ(std::string(buffer.data(), 4), "abcd");
    EXPECT_EQ((*process)->readStdout(buffer).value(), 3u);
    EXPECT_EQ(std::string(buffer.data(), 3), "ef!");
    EXPECT_EQ((*process)->readStdout(buffer).value(), 0u);                                // EOF.
    EXPECT_EQ((*process)->readStderr(buffer).error().code(), ErrorCode::InvalidArgument); // Not a pipe.
    EXPECT_EQ((*process)->wait().value(), (frost::io::ExitStatus{0, 0}));
    EXPECT_EQ((*process)->pid(), 4000u);
}

TEST(ScriptedProcessLauncher, ValidatesLikeTheRealLauncher) {
    ScriptedProcessLauncher launcher;
    launcher.expect(ScriptedProcess{});
    ProcessSpec spec = gitStatus();
    spec.executable = "git"; // Relative: must never be launched, scripted or not.
    const auto result = launcher.launch(spec);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(launcher.pendingScripts(), 1u); // The script is still waiting for a valid launch.
    ASSERT_EQ(launcher.launches().size(), 1u);
    EXPECT_FALSE(launcher.launches()[0].launched);
}

#ifdef _WIN32
TEST(ScriptedProcessLauncher, AppliesTheWindowsCommandLineChecks) {
    // What CreateProcessW cannot start must not "succeed" in tests either.
    ScriptedProcessLauncher launcher;
    launcher.expect(ScriptedProcess{});
    ProcessSpec quote = gitStatus();
    quote.executable = "C:\\to\"ols\\git.exe";
    ProcessSpec tooLong = gitStatus();
    tooLong.arguments = {std::string(40000, 'a')};
    ProcessSpec batch = gitStatus();
    batch.executable = "C:\\tools\\build.bat\\.";
    ProcessSpec device = gitStatus();
    device.executable = "\\\\.\\C:\\tools\\git.exe";
    for (const ProcessSpec& spec : {quote, tooLong, batch, device}) {
        const auto result = launcher.launch(spec);
        ASSERT_FALSE(result.has_value()) << spec.executable;
        EXPECT_TRUE(result.error().code() == ErrorCode::InvalidArgument ||
                    result.error().code() == ErrorCode::Unsupported)
            << result.error().describe();
    }
    EXPECT_EQ(launcher.pendingScripts(), 1u);
    EXPECT_EQ(launcher.launches().size(), 4u);
}
#endif

TEST(ScriptedProcessLauncher, UnexpectedAndUnscriptedLaunchesFail) {
    ScriptedProcessLauncher launcher;
    EXPECT_EQ(launcher.launch(gitStatus()).error().code(), ErrorCode::NotFound);
#ifdef _WIN32
    launcher.expect(ScriptedProcess{.expectExecutable = "C:\\tools\\cmake.exe"});
#else
    launcher.expect(ScriptedProcess{.expectExecutable = "/usr/bin/cmake"});
#endif
    const auto wrong = launcher.launch(gitStatus());
    ASSERT_FALSE(wrong.has_value());
    EXPECT_EQ(wrong.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(wrong.error().message().find("unexpected launch"), std::string::npos);
}

TEST(ScriptedProcessLauncher, LaunchErrorsAreReturned) {
    ScriptedProcessLauncher launcher;
    launcher.expect(ScriptedProcess{.launchError = frost::Error(ErrorCode::NotFound, "git is not installed")});
    const auto result = frost::io::runToCompletion(launcher, gitStatus(), "", 1s);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    ASSERT_EQ(result.error().context().size(), 1u); // runToCompletion names the executable.
    EXPECT_NE(result.error().context()[0].find(kTool), std::string::npos);
    EXPECT_FALSE(launcher.launches().at(0).launched);
}

TEST(ScriptedProcessLauncher, HungProcessesTimeOutWithoutRealTime) {
    ScriptedProcessLauncher launcher;
    launcher.expect(ScriptedProcess{.neverExits = true});
    auto process = launcher.launch(gitStatus());
    ASSERT_TRUE(process.has_value());
    EXPECT_EQ((*process)->wait(10min).error().code(), ErrorCode::Timeout);
    EXPECT_EQ((*process)->wait().error().code(), ErrorCode::Internal); // Would hang for real.
    ASSERT_TRUE((*process)->terminate().has_value());
    const auto status = (*process)->wait();
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->exitCode, frost::io::kTerminatedExitCode);
#ifndef _WIN32
    EXPECT_EQ(status->signal, SIGKILL);
#endif
    EXPECT_TRUE(launcher.launches().at(0).terminated);

    launcher.expect(ScriptedProcess{.neverExits = true});
    const auto start = std::chrono::steady_clock::now();
    const auto timedOut = frost::io::runToCompletion(launcher, gitStatus(), "", 1h);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 10s);
    ASSERT_FALSE(timedOut.has_value());
    EXPECT_EQ(timedOut.error().code(), ErrorCode::Timeout);
    EXPECT_TRUE(launcher.launches().at(1).terminated);
}

TEST(ScriptedProcessLauncher, ProcessesMayOutliveTheLauncher) {
    std::unique_ptr<frost::io::IProcess> process;
    {
        ScriptedProcessLauncher launcher;
        launcher.expect(ScriptedProcess{.stdoutBytes = "late"});
        process = std::move(launcher.launch(gitStatus()).value());
    }
    const auto result = process->communicate("", 1s);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->stdoutBytes, "late");
}

} // namespace
