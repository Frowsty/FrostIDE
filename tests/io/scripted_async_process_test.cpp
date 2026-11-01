// SPDX-License-Identifier: Apache-2.0
//
/*
 * Tests for ScriptedProcessLauncher::launchAsync, the IAsyncProcess double (scripted_process_launcher.hpp):
 * chunked output on a SimExecutor's virtual clock, communicate() with recorded stdin, merged stderr,
 * hung children that wait for terminate() (and time out on virtual time, without real sleeping),
 * cancelled waits, launch errors and validation, and the same double on a real ThreadPool.
 */

#include "frost/io/async_process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/sched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <stop_token>
#include <string>

#ifndef _WIN32
#include <csignal>
#endif

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::io::AsyncContext;
using frost::io::ProcessSpec;
using frost::io::Reactor;
using frost::io::ScriptedProcess;
using frost::io::ScriptedProcessLauncher;
using frost::io::Stdio;
using frost::sched::Lane;
using frost::sched::SimExecutor;

#ifdef _WIN32
const std::string kTool = "C:\\tools\\clangd.exe";
#else
const std::string kTool = "/usr/bin/clangd";
#endif

ProcessSpec clangd() {
    ProcessSpec spec;
    spec.executable = kTool;
    spec.arguments = {"--log=error"};
    spec.stdinMode = Stdio::Pipe;
    return spec;
}

template<class T>
ErrorCode codeOf(const frost::Result<T>& result) {
    return result ? ErrorCode::Unknown : result.error().code();
}

// The reactor is part of every AsyncContext; the double never uses it.
class ScriptedAsyncProcess : public ::testing::Test {
protected:
    void SetUp() override {
        auto created = Reactor::create();
        ASSERT_TRUE(created.has_value()) << created.error().describe();
        reactor = std::move(*created);
    }
    std::unique_ptr<Reactor> reactor;
    ScriptedProcessLauncher launcher;
};

TEST_F(ScriptedAsyncProcess, ChunksArriveOnTheVirtualClock) {
    SimExecutor sim;
    launcher.expect(ScriptedProcess{
        .exitCode = 3, .stdoutBytes = "abcdefgh", .stderrBytes = "E", .chunkBytes = 3, .chunkInterval = 10ms});
    auto process = launcher.launchAsync(clangd(), AsyncContext{*reactor, sim});
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    EXPECT_EQ((*process)->pid(), 4000U);
    const auto start = sim.steadyNow();
    std::array<char, 16> buffer{};
    const auto read = [&] { return sim.spawn(Lane::UserVisible, (*process)->readStdout(buffer)).join(); };
    EXPECT_EQ(read().value(), 3U);
    EXPECT_EQ(std::string(buffer.data(), 3), "abc");
    EXPECT_EQ(sim.steadyNow() - start, 10ms);
    EXPECT_EQ(read().value(), 3U);
    EXPECT_EQ(std::string(buffer.data(), 3), "def");
    EXPECT_EQ(read().value(), 2U);
    EXPECT_EQ(std::string(buffer.data(), 2), "gh");
    EXPECT_EQ(sim.steadyNow() - start, 30ms);
    EXPECT_EQ(read().value(), 0U); // EOF at once.
    EXPECT_EQ(sim.steadyNow() - start, 30ms);
    EXPECT_EQ(sim.spawn(Lane::UserVisible, (*process)->readStderr(buffer)).join().value(), 1U);
    const auto status = sim.spawn(Lane::UserVisible, (*process)->exited()).join();
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->exitCode, 3);
    // An empty buffer is refused as by the real process.
    EXPECT_EQ(codeOf(sim.spawn(Lane::UserVisible, (*process)->readStdout({})).join()), ErrorCode::InvalidArgument);
}

TEST_F(ScriptedAsyncProcess, CommunicateRecordsStdinAndCollectsMergedOutput) {
    SimExecutor sim;
    launcher.expect(
        ScriptedProcess{.stdoutBytes = "out;", .stderrBytes = "err;", .chunkBytes = 2, .chunkInterval = 1ms});
    ProcessSpec spec = clangd();
    spec.stderrMode = Stdio::MergeIntoStdout;
    auto process = launcher.launchAsync(spec, AsyncContext{*reactor, sim});
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    const auto result = sim.spawn(Lane::UserVisible, (*process)->communicate("{\"jsonrpc\":\"2.0\"}", 1min)).join();
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->stdoutBytes, "out;err;");
    EXPECT_TRUE(result->stderrBytes.empty());
    const auto record = launcher.launches().at(0);
    EXPECT_TRUE(record.launched);
    EXPECT_EQ(record.stdinBytes, "{\"jsonrpc\":\"2.0\"}");
    EXPECT_TRUE(record.stdinClosed);
    // Writes after the close are refused.
    EXPECT_EQ(codeOf(sim.spawn(Lane::UserVisible, (*process)->writeStdin("x")).join()), ErrorCode::InvalidArgument);
}

TEST_F(ScriptedAsyncProcess, HungProcessesWaitForTerminate) {
    SimExecutor sim;
    launcher.expect(ScriptedProcess{.stdoutBytes = "partial", .neverExits = true});
    auto process = launcher.launchAsync(clangd(), AsyncContext{*reactor, sim});
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    std::array<char, 16> buffer{};
    EXPECT_EQ(sim.spawn(Lane::UserVisible, (*process)->readStdout(buffer)).join().value(), 7U);
    // The pipes stay open and the exit does not come until terminate().
    auto read = sim.spawn(Lane::UserVisible, (*process)->readStdout(buffer));
    auto exit = sim.spawn(Lane::Background, (*process)->exited());
    sim.runUntilIdle();
    EXPECT_FALSE(read.ready());
    EXPECT_FALSE(exit.ready());
    ASSERT_TRUE((*process)->terminate().has_value());
    EXPECT_EQ(read.join().value(), 0U);
    const auto status = exit.join();
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->exitCode, frost::io::kTerminatedExitCode);
#ifndef _WIN32
    EXPECT_EQ(status->signal, SIGKILL);
#endif
    EXPECT_TRUE(launcher.launches().at(0).terminated);
}

TEST_F(ScriptedAsyncProcess, HungCommunicateTimesOutOnVirtualTime) {
    SimExecutor sim;
    launcher.expect(ScriptedProcess{.neverExits = true});
    auto process = launcher.launchAsync(clangd(), AsyncContext{*reactor, sim});
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    const auto virtualStart = sim.steadyNow();
    const auto realStart = std::chrono::steady_clock::now();
    const auto result = sim.spawn(Lane::UserVisible, (*process)->communicate({}, 1h)).join();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout) << result.error().describe();
    EXPECT_EQ(sim.steadyNow() - virtualStart, 1h);
    EXPECT_LT(std::chrono::steady_clock::now() - realStart, 10s);
    EXPECT_TRUE(launcher.launches().at(0).terminated);
}

TEST_F(ScriptedAsyncProcess, StoppedWaitsAreCancelled) {
    SimExecutor sim;
    launcher.expect(ScriptedProcess{.neverExits = true});
    auto process = launcher.launchAsync(clangd(), AsyncContext{*reactor, sim});
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    std::stop_source stop;
    std::array<char, 16> buffer{};
    auto exit = sim.spawn(Lane::UserVisible, (*process)->exited(stop.get_token()));
    auto read = sim.spawn(Lane::UserVisible, (*process)->readStdout(buffer, stop.get_token()));
    auto talk = sim.spawn(Lane::UserVisible, (*process)->communicate({}, 1h, stop.get_token()));
    sim.runFor(1min);
    EXPECT_FALSE(exit.ready());
    EXPECT_FALSE(read.ready());
    EXPECT_FALSE(talk.ready());
    static_cast<void>(stop.request_stop());
    EXPECT_EQ(codeOf(exit.join()), ErrorCode::Cancelled);
    EXPECT_EQ(codeOf(read.join()), ErrorCode::Cancelled);
    EXPECT_EQ(codeOf(talk.join()), ErrorCode::Cancelled);
    EXPECT_FALSE(launcher.launches().at(0).terminated);
    // A token stopped before the call.
    EXPECT_EQ(codeOf(sim.spawn(Lane::UserVisible, (*process)->exited(stop.get_token())).join()), ErrorCode::Cancelled);
    EXPECT_EQ(codeOf(sim.spawn(Lane::UserVisible, (*process)->writeStdin("x", stop.get_token())).join()),
              ErrorCode::Cancelled);
}

TEST_F(ScriptedAsyncProcess, LaunchErrorsAndValidation) {
    SimExecutor sim;
    const AsyncContext context{*reactor, sim};
    // Unscripted.
    EXPECT_EQ(codeOf(launcher.launchAsync(clangd(), context)), ErrorCode::NotFound);
    // Scripted errors are returned and consume the script.
    launcher.expect(ScriptedProcess{.launchError = frost::Error(ErrorCode::PermissionDenied, "scripted")});
    EXPECT_EQ(codeOf(launcher.launchAsync(clangd(), context)), ErrorCode::PermissionDenied);
    EXPECT_EQ(launcher.pendingScripts(), 0U);
    // The same validation as the real launcher.
    launcher.expect(ScriptedProcess{});
    ProcessSpec relative = clangd();
    relative.executable = "clangd";
    EXPECT_EQ(codeOf(launcher.launchAsync(relative, context)), ErrorCode::InvalidArgument);
    launcher.expect(ScriptedProcess{.expectExecutable = "/elsewhere"});
    EXPECT_EQ(launcher.pendingScripts(), 2U);
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 3U);
    for (const auto& record : launches) {
        EXPECT_FALSE(record.launched);
    }
}

TEST_F(ScriptedAsyncProcess, WorksOnAThreadPool) {
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    launcher.expect(ScriptedProcess{.exitCode = 1, .stdoutBytes = std::string(100'000, 'o'), .chunkBytes = 4096});
    auto process = launcher.launchAsync(clangd(), AsyncContext{*reactor, pool});
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    const auto result = pool.spawn(Lane::UserVisible, (*process)->communicate("in", 10s)).join();
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 1);
    EXPECT_EQ(result->stdoutBytes, std::string(100'000, 'o'));
    process->reset();
    pool.shutdown();
}

} // namespace
