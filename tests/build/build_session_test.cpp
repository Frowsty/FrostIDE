// SPDX-License-Identifier: Apache-2.0
//
// Tests for BuildSession (frost/build/build_session.hpp) against the scripted launcher, on every
// platform: streamed lines and diagnostics, duplicates, the result, trust, cancellation of a build
// that never ends, a cancel that races the exit, the destructor's cancel, and what happens to the
// processes left in the container (all ended, or only the infrastructure, with held output abandoned).

#include "frost/build/build_session.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/scripted_process_launcher.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::build::BuildCallbacks;
using frost::build::BuildResult;
using frost::build::BuildSession;
using frost::build::Diagnostic;
using frost::build::OutputStream;

#ifdef _WIN32
constexpr std::string_view kTool = "C:\\tools\\fake-build.exe";
#else
constexpr std::string_view kTool = "/opt/tools/fake-build";
#endif

frost::io::ProcessSpec toolSpec() {
    frost::io::ProcessSpec spec;
    spec.executable = std::string(kTool);
    return spec;
}

// Everything the callbacks saw, guarded because they run on the session's threads.
struct Recorder {
    frost::Mutex mutex;
    std::vector<std::pair<OutputStream, std::string>> lines FROST_GUARDED_BY(mutex);
    std::vector<Diagnostic> diagnostics FROST_GUARDED_BY(mutex);
    int finishedCalls FROST_GUARDED_BY(mutex) = 0;
    bool concurrent FROST_GUARDED_BY(mutex) = false;
    bool inCallback FROST_GUARDED_BY(mutex) = false; // Set while a line or diagnostic callback runs.

    BuildCallbacks callbacks() {
        return {
            .onLine =
                [this](OutputStream stream, std::string_view line) {
                    enter();
                    frost::LockGuard lock(mutex);
                    lines.emplace_back(stream, std::string(line));
                    leave();
                },
            .onDiagnostic =
                [this](const Diagnostic& diagnostic) {
                    enter();
                    frost::LockGuard lock(mutex);
                    diagnostics.push_back(diagnostic);
                    leave();
                },
            .onFinished =
                [this](const BuildResult&) {
                    frost::LockGuard lock(mutex);
                    ++finishedCalls;
                },
        };
    }

    void enter() {
        frost::LockGuard lock(mutex);
        concurrent = concurrent || inCallback;
        inCallback = true;
    }
    void leave() FROST_REQUIRES(mutex) { inCallback = false; }
};

TEST(BuildSession, StreamsLinesAndDiagnosticsAndReportsTheResult) {
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.expectExecutable = std::string(kTool),
                     .stdoutBytes =
                         "\r\n  main.cpp\r\n"
                         "C:\\s\\main.cpp(7,27): error C2065: 'x': undeclared identifier [C:\\s\\App.vcxproj]\r\n"
                         "C:\\s\\lib.h(3): warning C4244: conversion [C:\\s\\App.vcxproj]\r\n"
                         "C:\\s\\lib.h(3): warning C4244: conversion [C:\\s\\App.vcxproj]\r\n"
                         "no newline at the end",
                     .stderrBytes = "MSBUILD : error MSB1009: Project file does not exist.\n"});
    Recorder recorder;
    frost::ManualClock clock;
    auto session = BuildSession::start(launcher, toolSpec(), /*trusted=*/true, recorder.callbacks(), clock);
    ASSERT_TRUE(session.has_value()) << session.error().describe();
    const BuildResult& result = (*session)->wait();
    EXPECT_TRUE((*session)->finished());

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.errorCount, 2U);
    EXPECT_EQ(result.warningCount, 1U); // The repeated warning is dropped.
    ASSERT_EQ(result.diagnostics.size(), 3U);

    frost::LockGuard lock(recorder.mutex);
    EXPECT_EQ(recorder.finishedCalls, 1);
    EXPECT_FALSE(recorder.concurrent);
    EXPECT_EQ(recorder.diagnostics.size(), 3U);
    std::vector<std::string> out;
    std::vector<std::string> err;
    for (const auto& [stream, line] : recorder.lines) {
        (stream == OutputStream::Stdout ? out : err).push_back(line);
    }
    EXPECT_EQ(out, (std::vector<std::string>{"", "  main.cpp",
                                             "C:\\s\\main.cpp(7,27): error C2065: 'x': undeclared identifier "
                                             "[C:\\s\\App.vcxproj]",
                                             "C:\\s\\lib.h(3): warning C4244: conversion [C:\\s\\App.vcxproj]",
                                             "C:\\s\\lib.h(3): warning C4244: conversion [C:\\s\\App.vcxproj]",
                                             "no newline at the end"}));
    EXPECT_EQ(err, (std::vector<std::string>{"MSBUILD : error MSB1009: Project file does not exist."}));
    // Stdout's diagnostics arrive in order (stderr's may come before, after or between them).
    std::vector<std::string> stdoutCodes;
    for (const Diagnostic& diagnostic : result.diagnostics) {
        if (diagnostic.code != "MSB1009") {
            stdoutCodes.push_back(diagnostic.code);
        }
    }
    EXPECT_EQ(stdoutCodes, (std::vector<std::string>{"C2065", "C4244"}));
}

TEST(BuildSession, NonZeroExitIsAFailedBuild) {
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.exitCode = 1});
    auto session = BuildSession::start(launcher, toolSpec(), true, {});
    ASSERT_TRUE(session.has_value());
    const std::optional<BuildResult> result = (*session)->waitFor(std::chrono::seconds(30));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->success);
    EXPECT_EQ(result->exitCode, 1);
}

TEST(BuildSession, RefusesUntrustedFoldersAndBadSpecs) {
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({});
    auto untrusted = BuildSession::start(launcher, toolSpec(), /*trusted=*/false, {});
    ASSERT_FALSE(untrusted.has_value());
    EXPECT_EQ(untrusted.error().code(), ErrorCode::Untrusted);

    frost::io::ProcessSpec merged = toolSpec();
    merged.stderrMode = frost::io::Stdio::MergeIntoStdout;
    EXPECT_EQ(BuildSession::start(launcher, merged, true, {}).error().code(), ErrorCode::InvalidArgument);
    frost::io::ProcessSpec uncontained = toolSpec();
    uncontained.killOnClose = false;
    EXPECT_EQ(BuildSession::start(launcher, uncontained, true, {}).error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_EQ(launcher.pendingScripts(), 1U);
}

TEST(BuildSession, LaunchErrorsComeBackAtOnce) {
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.launchError = frost::Error(ErrorCode::NotFound, "no MSBuild.exe")});
    auto session = BuildSession::start(launcher, toolSpec(), true, {});
    ASSERT_FALSE(session.has_value());
    EXPECT_EQ(session.error().code(), ErrorCode::NotFound);
}

TEST(BuildSession, CancelEndsABuildThatNeverFinishes) {
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.stdoutBytes = "working\n", .neverExits = true});
    Recorder recorder;
    auto session = BuildSession::start(launcher, toolSpec(), true, recorder.callbacks());
    ASSERT_TRUE(session.has_value());
    EXPECT_FALSE((*session)->waitFor(std::chrono::milliseconds(50)).has_value());
    EXPECT_FALSE((*session)->finished());
    (*session)->cancel();
    (*session)->cancel(); // Idempotent.
    const BuildResult& result = (*session)->wait();
    EXPECT_TRUE(result.cancelled);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.exitCode, frost::io::kTerminatedExitCode);
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    EXPECT_TRUE(launches[0].terminated);
    frost::LockGuard lock(recorder.mutex);
    EXPECT_EQ(recorder.finishedCalls, 1);
    ASSERT_EQ(recorder.lines.size(), 1U);
    EXPECT_EQ(recorder.lines[0].second, "working");
}

TEST(BuildSession, DestroyingARunningSessionCancelsIt) {
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.neverExits = true});
    Recorder recorder;
    {
        auto session = BuildSession::start(launcher, toolSpec(), true, recorder.callbacks());
        ASSERT_TRUE(session.has_value());
    }
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    EXPECT_TRUE(launches[0].terminated);
    frost::LockGuard lock(recorder.mutex);
    EXPECT_EQ(recorder.finishedCalls, 1);
}

TEST(BuildSession, CancelAfterTheEndChangesNothing) {
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({});
    auto session = BuildSession::start(launcher, toolSpec(), true, {});
    ASSERT_TRUE(session.has_value());
    EXPECT_TRUE((*session)->wait().success);
    (*session)->cancel();
    EXPECT_FALSE((*session)->wait().cancelled);
}

TEST(BuildSession, ACancelThatRacesTheExitReportsTheFinishedBuild) {
    // The tool exits with 0 just as cancel() arrives: terminate() then finds it gone.
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.exitCode = 0, .stdoutBytes = "done\n", .neverExits = true, .racesTerminate = true});
    auto session = BuildSession::start(launcher, toolSpec(), true, {});
    ASSERT_TRUE(session.has_value());
    EXPECT_FALSE((*session)->waitFor(std::chrono::milliseconds(20)).has_value());
    (*session)->cancel();
    const BuildResult& result = (*session)->wait();
    EXPECT_FALSE(result.cancelled);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.exitCode, 0);

    // The same race with a failed build: failed, not cancelled.
    launcher.expect({.exitCode = 3, .neverExits = true, .racesTerminate = true});
    auto failed = BuildSession::start(launcher, toolSpec(), true, {});
    ASSERT_TRUE(failed.has_value());
    (*failed)->cancel();
    EXPECT_FALSE((*failed)->wait().cancelled);
    EXPECT_FALSE((*failed)->wait().success);
    EXPECT_EQ((*failed)->wait().exitCode, 3);
}

TEST(BuildSession, ByDefaultTheWholeContainerEndsWithTheTool) {
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.leftovers = {{9001, "mspdbsrv.exe"}, {9002, "game.exe"}}});
    auto session = BuildSession::start(launcher, toolSpec(), true, {});
    ASSERT_TRUE(session.has_value());
    EXPECT_TRUE((*session)->wait().success);
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    EXPECT_TRUE(launches[0].terminated);
    EXPECT_FALSE(launches[0].containerReleased);
}

TEST(BuildSession, OnlyInfrastructureEndsAndTheRestKeepsRunning) {
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.leftovers = {{9001, "mspdbsrv.exe"}, {9002, "game.exe"}, {9003, "MSBUILD.EXE"}}});
    const frost::build::BuildSessionOptions options{.infrastructureImages = {"MSBuild.exe", "mspdbsrv.exe"},
                                                    .outputGrace = std::chrono::milliseconds(1000)};
    auto session = BuildSession::start(launcher, toolSpec(), true, {}, frost::systemClock(), options);
    ASSERT_TRUE(session.has_value());
    EXPECT_TRUE((*session)->wait().success);
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    EXPECT_EQ(launches[0].terminatedContained, (std::vector<frost::io::ProcessId>{9001, 9003}));
    EXPECT_TRUE(launches[0].containerReleased); // game.exe outlives the session.
    EXPECT_FALSE(launches[0].terminated);
    EXPECT_FALSE(launches[0].outputAbandoned); // The output ended on its own.

    // Only infrastructure left: it is ended and nothing needs releasing.
    launcher.expect({.leftovers = {{9101, "VBCSCompiler.exe"}}});
    auto quiet = BuildSession::start(launcher, toolSpec(), true, {}, frost::systemClock(),
                                     {.infrastructureImages = {"VBCSCompiler.exe"}});
    ASSERT_TRUE(quiet.has_value());
    EXPECT_TRUE((*quiet)->wait().success);
    const auto second = launcher.launches().at(1);
    EXPECT_EQ(second.terminatedContained, (std::vector<frost::io::ProcessId>{9101}));
    EXPECT_FALSE(second.containerReleased);
}

TEST(BuildSession, OutputASurvivorHoldsOpenIsAbandonedAfterTheGrace) {
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.stdoutBytes = "  App.vcxproj -> C:\\out\\App.exe\n",
                     .outputHeld = true,
                     .leftovers = {{9002, "server.exe"}}});
    Recorder recorder;
    const auto started = std::chrono::steady_clock::now();
    auto session =
        BuildSession::start(launcher, toolSpec(), true, recorder.callbacks(), frost::systemClock(),
                            {.infrastructureImages = {"mspdbsrv.exe"}, .outputGrace = std::chrono::milliseconds(50)});
    ASSERT_TRUE(session.has_value());
    const std::optional<BuildResult> result = (*session)->waitFor(std::chrono::seconds(30));
    ASSERT_TRUE(result.has_value()) << "held output kept the build from finishing";
    EXPECT_TRUE(result->success);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(10));
    const auto launches = launcher.launches();
    EXPECT_TRUE(launches[0].outputAbandoned);
    EXPECT_TRUE(launches[0].containerReleased);
    EXPECT_FALSE(launches[0].terminated);
    frost::LockGuard lock(recorder.mutex);
    ASSERT_EQ(recorder.lines.size(), 1U);
    EXPECT_EQ(recorder.lines[0].second, "  App.vcxproj -> C:\\out\\App.exe");
}

} // namespace
