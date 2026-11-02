// SPDX-License-Identifier: Apache-2.0
//
// RunSession (frost/build/run_session.hpp) with real processes (io_test_child, label
// "integration"): stdout and stderr captured, the exit code, input reaching the program, Stop
// ending the program and its grandchild, a natural exit leaving the grandchild running, and on
// Windows the external console's cmd wrapper keeping the exit code (run with pipes and without a
// new console, so no window opens).

#include "frost/build/run_session.hpp"
#include "frost/build/run_target.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include "io/process_env.hpp"
#include <gtest/gtest.h>

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <string>
#include <vector>

namespace frost::build {
namespace {

using namespace std::chrono_literals;

constexpr auto kWait = 30s;

// What a session streamed.
struct Collected {
    Mutex mutex;
    std::condition_variable changed;
    std::string out FROST_GUARDED_BY(mutex);
    std::string err FROST_GUARDED_BY(mutex);

    RunSessionCallbacks callbacks() {
        return {.onOutput =
                    [this](RunStream stream, std::string_view text) {
                        {
                            LockGuard lock(mutex);
                            (stream == RunStream::Stdout ? out : err) += text;
                        }
                        changed.notify_all();
                    },
                .onExited = {}};
    }

    // Waits until stdout contains `needle`; returns stdout.
    std::string waitForStdout(std::string_view needle) {
        UniqueLock lock(mutex);
        changed.wait_for(lock.native(), kWait,
                         [&]() FROST_NO_THREAD_SAFETY_ANALYSIS { return out.find(needle) != std::string::npos; });
        return out;
    }

    std::string stdoutText() {
        LockGuard lock(mutex);
        return out;
    }

    std::string stderrText() {
        LockGuard lock(mutex);
        return err;
    }
};

// io_test_child with every stream piped and the tree contained, as the Run console starts programs.
io::ProcessSpec childRun(std::vector<std::string> arguments) {
    io::ProcessSpec spec = test::childSpec(std::move(arguments));
    spec.stdinMode = io::Stdio::Pipe;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    return spec;
}

std::unique_ptr<RunSession> start(const io::ProcessSpec& spec, Collected& collected, RunSessionOptions options = {}) {
    auto session = RunSession::start(io::realProcessLauncher(), spec, collected.callbacks(), systemClock(), options);
    EXPECT_TRUE(session.has_value()) << session.error().describe();
    return session ? std::move(*session) : nullptr;
}

// The pid in io_test_child's "grandchild <pid>" line; 0 if there is none.
io::ProcessId grandchildPid(const std::string& out) {
    constexpr std::string_view prefix = "grandchild ";
    const std::size_t at = out.find(prefix);
    if (at == std::string::npos) {
        return 0;
    }
    io::ProcessId pid = 0;
    const char* begin = out.data() + at + prefix.size();
    std::from_chars(begin, out.data() + out.size(), pid);
    return pid;
}

TEST(RunSession, CapturesStdoutStderrAndTheExitCode) {
    Collected collected;
    auto session = start(childRun({"print", "out text", "err text"}), collected);
    ASSERT_NE(session, nullptr);
    const auto exit = session->waitFor(kWait);
    ASSERT_TRUE(exit.has_value());
    EXPECT_EQ(exit->exitCode, 0);
    EXPECT_FALSE(exit->stopped);
    EXPECT_FALSE(exit->waitFailed);
    EXPECT_GT(exit->duration.count(), 0);
    EXPECT_EQ(collected.stdoutText(), "out text");
    EXPECT_EQ(collected.stderrText(), "err text");
    EXPECT_TRUE(session->finished());

    Collected second;
    auto failing = start(childRun({"exit", "7"}), second);
    ASSERT_NE(failing, nullptr);
    const auto failed = failing->waitFor(kWait);
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(failed->exitCode, 7);
}

TEST(RunSession, InputReachesTheProgram) {
    Collected collected;
    auto session = start(childRun({"cat"}), collected);
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(session->acceptsInput());
    // cat copies with fread, which returns once its buffer is full or at end of file, so the
    // output comes after closeInput (the tiny fixture's program shows line-by-line input).
    ASSERT_TRUE(session->writeInput("first line\n").has_value());
    ASSERT_TRUE(session->writeInput("second\n").has_value());
    session->closeInput();
    EXPECT_FALSE(session->acceptsInput());
    EXPECT_EQ(session->writeInput("late").error().code(), ErrorCode::InvalidArgument);
    const auto exit = session->waitFor(kWait);
    ASSERT_TRUE(exit.has_value());
    EXPECT_EQ(exit->exitCode, 0) << "cat ends at end of file";
    EXPECT_EQ(collected.stdoutText(), "first line\nsecond\n");
}

TEST(RunSession, StopEndsTheProgramAndItsGrandchild) {
    Collected collected;
    auto session = start(childRun({"grandchild", "wait"}), collected);
    ASSERT_NE(session, nullptr);
    const io::ProcessId pid = grandchildPid(collected.waitForStdout("\n"));
    ASSERT_NE(pid, 0U) << collected.stdoutText();
    const test::ProcessWatch grandchild(pid);
    ASSERT_TRUE(grandchild.valid());
    EXPECT_FALSE(grandchild.exitsWithin(100ms));

    session->stop();
    session->stop(); // Idempotent.
    const auto exit = session->waitFor(kWait);
    ASSERT_TRUE(exit.has_value());
    EXPECT_TRUE(exit->stopped);
    EXPECT_EQ(exit->exitCode, io::kTerminatedExitCode);
    const bool ended = grandchild.exitsWithin(10s);
    EXPECT_TRUE(ended) << "Stop left the grandchild running";
    if (!ended) {
        grandchild.kill();
    }
}

TEST(RunSession, ANaturalExitLeavesWhatTheProgramStartedRunning) {
    Collected collected;
    auto session = start(childRun({"grandchild", "exit"}), collected, {.outputGrace = 200ms});
    ASSERT_NE(session, nullptr);
    const auto exit = session->waitFor(kWait);
    ASSERT_TRUE(exit.has_value()) << "the grandchild holding stdout must not keep the session open";
    EXPECT_EQ(exit->exitCode, 0);
    EXPECT_FALSE(exit->stopped);
    const io::ProcessId pid = grandchildPid(collected.stdoutText());
    ASSERT_NE(pid, 0U) << collected.stdoutText();
    const test::ProcessWatch grandchild(pid);
    ASSERT_TRUE(grandchild.valid()) << "the grandchild should still run";
    EXPECT_FALSE(grandchild.exitsWithin(200ms));
    session.reset();
    EXPECT_FALSE(grandchild.exitsWithin(100ms)) << "destroying a finished session ends nothing";
    grandchild.kill(); // The test's own cleanup.
    EXPECT_TRUE(grandchild.exitsWithin(10s));
}

#ifdef _WIN32
// The external console's command line, run by the real cmd.exe but with pipes and no new console:
// the program's exit code comes back through "pause" and the closing line is printed.
TEST(RunSession, TheExternalConsoleWrapperKeepsTheExitCode) {
    auto cmd = systemCommandProcessor();
    ASSERT_TRUE(cmd.has_value()) << cmd.error().describe();
    RunCommand command;
    command.executable = test::childPath();
    command.arguments = "exit 5";
    command.workingDirectory = std::string(io::parentPath(command.executable));
    auto spec = runProcessSpec(command, ConsoleMode::ExternalConsole, *cmd);
    ASSERT_TRUE(spec.has_value()) << spec.error().describe();
    ASSERT_TRUE(spec->newConsole);
    spec->newConsole = false;          // No window during tests.
    spec->stdinMode = io::Stdio::Null; // "pause" reads end of file and returns at once.
    spec->stdoutMode = io::Stdio::Pipe;
    spec->stderrMode = io::Stdio::Pipe;
    Collected collected;
    auto session = start(*spec, collected);
    ASSERT_NE(session, nullptr);
    const auto exit = session->waitFor(kWait);
    ASSERT_TRUE(exit.has_value());
    EXPECT_EQ(exit->exitCode, 5) << collected.stdoutText();
    EXPECT_NE(collected.stdoutText().find("Process finished with exit code 5."), std::string::npos)
        << collected.stdoutText();
}
#endif

} // namespace
} // namespace frost::build
