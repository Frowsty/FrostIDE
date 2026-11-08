// SPDX-License-Identifier: Apache-2.0
//
/*
 * Integration tests for IAsyncProcess (async_process.hpp, ADR 0020) through the real launcher's
 * launchAsync, driving io_test_child: communicate() on both pipes, streamed output in bounded reads,
 * stdin writes, closing stdin, EOF on a stdout the child closed early, exit statuses, terminate(),
 * timeouts, cancelled reads and writes (a cancelled read loses no byte), destruction with operations
 * pending, the process tree with killOnClose, reaping on POSIX, a reactor shutdown with live processes,
 * the refused caller-driven scheduler and the non-pipe streams. The heavier pipe faults are in
 * async_process_fault_test.cpp (label stress).
 */

#include "frost/io/async_process.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/sched/thread_pool.hpp"

#include "process_env.hpp"
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>

#include <cerrno>
#include <csignal>

#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::io::IAsyncProcess;
using frost::io::ProcessSpec;
using frost::io::Stdio;
using frost::sched::Lane;
using frost::test::AsyncEnv;
using frost::test::childSpec;
using frost::test::pattern;
using frost::test::ProcessWatch;

ProcessSpec withStdin(ProcessSpec spec) {
    spec.stdinMode = Stdio::Pipe;
    return spec;
}

// The error code of a failed result; Unknown for a success (never expected where this is used).
template<class T>
ErrorCode codeOf(const frost::Result<T>& result) {
    return result ? ErrorCode::Unknown : result.error().code();
}

#ifndef _WIN32
// Whether `pid` is no longer a child of this process that waits to be reaped (ECHILD), within `timeout`.
bool reapedWithin(pid_t pid, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        siginfo_t info{};
        if (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0 && errno == ECHILD) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
}
#endif

TEST(AsyncProcess, CommunicateRoundTripsBothPipes) {
    AsyncEnv env;
    auto process = env.launch(withStdin(childSpec({"tee"})));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    // More than any pipe buffer, so the writer, both readers and the exit overlap.
    const std::string input = pattern(3 * 1024 * 1024 + 5);
    auto result = env.run((*process)->communicate(input, 120s));
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 0);
    EXPECT_EQ(result->signal, 0);
    EXPECT_TRUE(result->stdoutBytes == input) << result->stdoutBytes.size();
    EXPECT_TRUE(result->stderrBytes == input) << result->stderrBytes.size();
}

TEST(AsyncProcess, ExitCodesAreReportedToEveryWaiter) {
    AsyncEnv env;
    auto process = env.launch(childSpec({"exit", "7"}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    auto first = env.pool().spawn(Lane::UserVisible, (*process)->exited());
    auto second = env.pool().spawn(Lane::Background, (*process)->exited());
    const auto a = first.join();
    const auto b = second.join();
    ASSERT_TRUE(a.has_value()) << a.error().describe();
    ASSERT_TRUE(b.has_value()) << b.error().describe();
    EXPECT_EQ(a->exitCode, 7);
    EXPECT_EQ(b->exitCode, 7);
    // Later calls return the same status at once.
    const auto again = env.run((*process)->exited());
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->exitCode, 7);
    // Output pipes end with EOF.
    const auto out = env.drain(**process);
    ASSERT_TRUE(out.has_value()) << out.error().describe();
    EXPECT_TRUE(out->empty());
}

TEST(AsyncProcess, StreamsLargeOutputInBoundedReads) {
    AsyncEnv env;
    constexpr std::size_t kSize = 64 * 1024 * 1024 + 3;
    auto process = env.launch(childSpec({"stream", std::to_string(kSize)}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    // Odd-sized reads: every read returns at least one and at most 4,099 bytes.
    const auto out = env.drain(**process, false, 4099);
    ASSERT_TRUE(out.has_value()) << out.error().describe();
    ASSERT_EQ(out->size(), kSize);
    EXPECT_TRUE(*out == pattern(kSize));
    const auto status = env.run((*process)->exited());
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->exitCode, 0);
}

TEST(AsyncProcess, BothPipesStreamConcurrently) {
    AsyncEnv env;
    constexpr std::size_t kSize = 8 * 1024 * 1024 + 11;
    auto process = env.launch(childSpec({"flood", std::to_string(kSize)}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    // The child interleaves 4 KiB chunks: either reader alone would stall it on the other pipe.
    auto out =
        env.pool().spawn(Lane::UserVisible, [](IAsyncProcess& p) -> frost::sched::Task<frost::Result<std::string>> {
            std::string all;
            std::array<char, 16 * 1024> buffer{};
            for (;;) {
                auto got = co_await p.readStdout(buffer);
                if (!got || *got == 0) {
                    co_return got ? frost::Result<std::string>(all) : std::unexpected(got.error());
                }
                all.append(buffer.data(), *got);
            }
        }(**process));
    const auto err = env.drain(**process, true);
    const auto outResult = out.join();
    ASSERT_TRUE(outResult.has_value()) << outResult.error().describe();
    ASSERT_TRUE(err.has_value()) << err.error().describe();
    const std::string expected = pattern(kSize);
    EXPECT_TRUE(*outResult == expected) << outResult->size();
    EXPECT_TRUE(*err == expected) << err->size();
}

TEST(AsyncProcess, MergedStderrArrivesOnStdout) {
    AsyncEnv env;
    ProcessSpec spec = childSpec({"print", "out;", "err;"});
    spec.stderrMode = Stdio::MergeIntoStdout;
    auto process = env.launch(spec);
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    std::array<char, 16> buffer{};
    const auto stderrRead = env.run((*process)->readStderr(buffer));
    ASSERT_FALSE(stderrRead.has_value());
    EXPECT_EQ(stderrRead.error().code(), ErrorCode::InvalidArgument);
    const auto out = env.drain(**process);
    ASSERT_TRUE(out.has_value()) << out.error().describe();
    EXPECT_EQ(*out, "out;err;");
}

TEST(AsyncProcess, WritesReachTheChildAndClosingStdinEndsIt) {
    AsyncEnv env;
    auto process = env.launch(withStdin(childSpec({"cat"})));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    ASSERT_TRUE(env.run((*process)->writeStdin("hello, ")).has_value());
    ASSERT_TRUE(env.run((*process)->writeStdin("")).has_value()); // Empty writes succeed at once.
    ASSERT_TRUE(env.run((*process)->writeStdin("world")).has_value());
    (*process)->closeStdin();
    (*process)->closeStdin(); // Idempotent.
    const auto out = env.drain(**process);
    ASSERT_TRUE(out.has_value()) << out.error().describe();
    EXPECT_EQ(*out, "hello, world");
    const auto status = env.run((*process)->exited());
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->exitCode, 0);
    // A write after closeStdin is refused.
    const auto late = env.run((*process)->writeStdin("x"));
    ASSERT_FALSE(late.has_value());
    EXPECT_EQ(late.error().code(), ErrorCode::InvalidArgument);
}

TEST(AsyncProcess, WriteToAChildThatExitedFailsWithIoError) {
#ifndef _WIN32
    // The default disposition: a SIGPIPE raised on any thread would end this test process.
    ASSERT_NE(std::signal(SIGPIPE, SIG_DFL), SIG_ERR);
#endif
    AsyncEnv env;
    auto process = env.launch(withStdin(childSpec({"read-then-exit", "0"})));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    const auto status = env.run((*process)->exited());
    ASSERT_TRUE(status.has_value()) << status.error().describe();
    EXPECT_EQ(status->exitCode, 0);
    const auto written = env.run((*process)->writeStdin(std::string(1024 * 1024, 'x')));
    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().code(), ErrorCode::IoError) << written.error().describe();
    // communicate() treats a child that stopped reading as having dropped the rest.
    auto again = env.launch(withStdin(childSpec({"read-then-exit", "10"})));
    ASSERT_TRUE(again.has_value()) << again.error().describe();
    const auto result = env.run((*again)->communicate(std::string(4 * 1024 * 1024, 'y'), 120s));
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 0);
    EXPECT_EQ(result->stdoutBytes, "read 10\n");
}

TEST(AsyncProcess, StdoutClosedEarlyGivesEofWhileTheChildRuns) {
    AsyncEnv env;
    auto process = env.launch(childSpec({"close-stdout-then-sleep", "1500"}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    std::stop_source stop;
    auto exit = env.pool().spawn(Lane::UserVisible, (*process)->exited(stop.get_token()));
    const auto out = env.drain(**process);
    ASSERT_TRUE(out.has_value()) << out.error().describe();
    EXPECT_TRUE(out->empty());
    EXPECT_FALSE(exit.ready()) << "EOF came only with the exit";
    const auto status = exit.join();
    ASSERT_TRUE(status.has_value()) << status.error().describe();
    EXPECT_EQ(status->exitCode, 0);
}

TEST(AsyncProcess, TerminateWakesExitWaitersAndEndsTheReads) {
    AsyncEnv env;
    auto process = env.launch(childSpec({"sleep", "60000"}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    auto exit = env.pool().spawn(Lane::UserVisible, (*process)->exited());
    std::array<char, 64> buffer{};
    auto read = env.pool().spawn(Lane::UserVisible, (*process)->readStdout(buffer));
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(exit.ready());
    EXPECT_FALSE(read.ready());
    ASSERT_TRUE((*process)->terminate().has_value());
    const auto status = exit.join();
    ASSERT_TRUE(status.has_value()) << status.error().describe();
    EXPECT_EQ(status->exitCode, frost::io::kTerminatedExitCode);
#ifndef _WIN32
    EXPECT_EQ(status->signal, SIGKILL);
#endif
    const auto got = read.join();
    ASSERT_TRUE(got.has_value()) << got.error().describe();
    EXPECT_EQ(*got, 0U);                              // EOF.
    ASSERT_TRUE((*process)->terminate().has_value()); // Again, after the exit: still fine.
}

TEST(AsyncProcess, CommunicateTimesOutAndTerminatesTheChild) {
    AsyncEnv env;
    auto process = env.launch(withStdin(childSpec({"sleep", "60000"})));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    const ProcessWatch watch((*process)->pid());
    ASSERT_TRUE(watch.valid());
    const auto start = std::chrono::steady_clock::now();
    const auto result = env.run((*process)->communicate("ignored", 300ms));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout) << result.error().describe();
    EXPECT_GE(elapsed, 250ms);
    EXPECT_LT(elapsed, 30s);
    EXPECT_TRUE(watch.exitsWithin(30s));
}

TEST(AsyncProcess, CommunicateIsCancelledByItsToken) {
    AsyncEnv env;
    auto process = env.launch(childSpec({"sleep", "60000"}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    std::stop_source stop;
    auto call = env.pool().spawn(Lane::UserVisible, (*process)->communicate({}, 1h, stop.get_token()));
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(call.ready());
    static_cast<void>(stop.request_stop());
    const auto result = call.join();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled) << result.error().describe();
}

TEST(AsyncProcess, ACancelledReadLosesNoByte) {
    AsyncEnv env;
    auto process = env.launch(withStdin(childSpec({"cat"})));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    std::array<char, 64> buffer{};
    // A read cancelled while the OS read is in flight...
    std::stop_source stop;
    auto pending = env.pool().spawn(Lane::UserVisible, (*process)->readStdout(buffer, stop.get_token()));
    std::this_thread::sleep_for(20ms);
    ASSERT_FALSE(pending.ready());
    const auto cancelledAt = std::chrono::steady_clock::now();
    static_cast<void>(stop.request_stop());
    const auto cancelled = pending.join();
    const auto latency = std::chrono::steady_clock::now() - cancelledAt;
    ASSERT_FALSE(cancelled.has_value());
    EXPECT_EQ(cancelled.error().code(), ErrorCode::Cancelled);
    EXPECT_LT(latency, 1s); // The 10 ms bound is measured in the fault suite.
    // ...leaves that OS read running: what it brings goes to the next read.
    ASSERT_TRUE(env.run((*process)->writeStdin("abc")).has_value());
    (*process)->closeStdin();
    const auto rest = env.drain(**process);
    ASSERT_TRUE(rest.has_value()) << rest.error().describe();
    EXPECT_EQ(*rest, "abc");
    // A token stopped before the call is Cancelled at once.
    std::stop_source early;
    static_cast<void>(early.request_stop());
    const auto refused = env.run((*process)->readStdout(buffer, early.get_token()));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::Cancelled);
}

TEST(AsyncProcess, AStopMidWriteCancelsAndClosesStdin) {
    AsyncEnv env;
    auto process = env.launch(withStdin(childSpec({"sleep", "60000"})));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    std::stop_source stop;
    // The child never reads: the write stalls once the pipe is full.
    auto write = env.pool().spawn(Lane::UserVisible,
                                  (*process)->writeStdin(std::string(64 * 1024 * 1024, 'z'), stop.get_token()));
    std::this_thread::sleep_for(100ms);
    ASSERT_FALSE(write.ready());
    static_cast<void>(stop.request_stop());
    const auto result = write.join();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled) << result.error().describe();
    // A torn write cannot be resumed: stdin is closed now.
    const auto next = env.run((*process)->writeStdin("x"));
    ASSERT_FALSE(next.has_value());
    EXPECT_EQ(next.error().code(), ErrorCode::InvalidArgument) << next.error().describe();
}

TEST(AsyncProcess, DestroyingTheProcessCancelsPendingOperationsAndEndsTheChild) {
    AsyncEnv env;
    auto process = env.launch(withStdin(childSpec({"sleep", "60000"})));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    const ProcessWatch watch((*process)->pid());
    ASSERT_TRUE(watch.valid());
    std::array<char, 64> outBuffer{};
    std::array<char, 64> errBuffer{};
    auto out = env.pool().spawn(Lane::UserVisible, (*process)->readStdout(outBuffer));
    auto err = env.pool().spawn(Lane::UserVisible, (*process)->readStderr(errBuffer));
    auto exit = env.pool().spawn(Lane::UserVisible, (*process)->exited());
    auto write = env.pool().spawn(Lane::UserVisible, (*process)->writeStdin(std::string(16 * 1024 * 1024, 'w')));
    std::this_thread::sleep_for(50ms);
    const auto destroyStart = std::chrono::steady_clock::now();
    process->reset();
    // Budget: B21: the destructor only posts.
    EXPECT_LT(std::chrono::steady_clock::now() - destroyStart, 1s);
    for (auto* read : {&out, &err}) {
        const auto got = read->join();
        ASSERT_FALSE(got.has_value()) << *got;
        EXPECT_EQ(got.error().code(), ErrorCode::Cancelled) << got.error().describe();
    }
    const auto exited = exit.join();
    ASSERT_FALSE(exited.has_value());
    EXPECT_EQ(exited.error().code(), ErrorCode::Cancelled);
    const auto written = write.join();
    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().code(), ErrorCode::Cancelled) << written.error().describe();
    EXPECT_TRUE(watch.exitsWithin(30s)) << "killOnClose left the child running";
    EXPECT_TRUE(env.settlesWithin(30s)) << env.boundObjects() << " states still bound";
}

TEST(AsyncProcess, DestroyingTheProcessEndsTheWholeTree) {
    AsyncEnv env;
    auto process = env.launch(childSpec({"grandchild", "exit"}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    // "grandchild <pid>\n" arrives before the child exits.
    std::string text;
    std::array<char, 256> buffer{};
    while (text.find('\n') == std::string::npos) {
        const auto got = env.run((*process)->readStdout(buffer));
        ASSERT_TRUE(got.has_value()) << got.error().describe();
        ASSERT_NE(*got, 0U) << text;
        text.append(buffer.data(), *got);
    }
    ASSERT_TRUE(text.starts_with("grandchild ")) << text;
    const auto pid = static_cast<frost::io::ProcessId>(std::stoul(text.substr(11)));
    const ProcessWatch grandchild(pid);
    ASSERT_TRUE(grandchild.valid());
    const auto status = env.run((*process)->exited());
    ASSERT_TRUE(status.has_value()) << status.error().describe();
    EXPECT_EQ(status->exitCode, 0);
    EXPECT_FALSE(grandchild.exitsWithin(200ms));
    process->reset();
    EXPECT_TRUE(grandchild.exitsWithin(30s)) << "the release left the grandchild running";
    grandchild.kill();
}

// A descendant in a process group of its own (ninja's commands) is ended by terminate() and by the
// release: the Windows job, the Linux session sweep.
TEST(AsyncProcess, TerminateAndReleaseEndADescendantInAProcessGroupOfItsOwn) {
    AsyncEnv env;
    for (const bool terminate : {true, false}) {
        SCOPED_TRACE(terminate ? "terminate" : "release");
        auto process = env.launch(childSpec({"grandchild-own-group", terminate ? "wait" : "exit"}));
        ASSERT_TRUE(process.has_value()) << process.error().describe();
        std::string text;
        std::array<char, 256> buffer{};
        while (text.find('\n') == std::string::npos) {
            const auto got = env.run((*process)->readStdout(buffer));
            ASSERT_TRUE(got.has_value()) << got.error().describe();
            ASSERT_NE(*got, 0U) << text;
            text.append(buffer.data(), *got);
        }
        ASSERT_TRUE(text.starts_with("grandchild ")) << text;
        const auto pid = static_cast<frost::io::ProcessId>(std::stoul(text.substr(11)));
        const ProcessWatch grandchild(pid);
        ASSERT_TRUE(grandchild.valid());
#ifndef _WIN32
        ASSERT_EQ(::getpgid(static_cast<pid_t>(pid)), static_cast<pid_t>(pid)) << "not in a group of its own";
#endif
        if (terminate) {
            ASSERT_TRUE((*process)->terminate().has_value());
            const auto status = env.run((*process)->exited());
            ASSERT_TRUE(status.has_value()) << status.error().describe();
            EXPECT_EQ(status->exitCode, frost::io::kTerminatedExitCode);
        } else {
            const auto status = env.run((*process)->exited());
            ASSERT_TRUE(status.has_value()) << status.error().describe();
            EXPECT_EQ(status->exitCode, 0);
            EXPECT_FALSE(grandchild.exitsWithin(200ms));
            process->reset();
        }
        EXPECT_TRUE(grandchild.exitsWithin(30s)) << "the grandchild in its own group is still running";
        grandchild.kill();
    }
}

TEST(AsyncProcess, WithoutKillOnCloseTheChildOutlivesItsObject) {
    AsyncEnv env;
    ProcessSpec spec = childSpec({"sleep", "60000"});
    spec.killOnClose = false;
    auto process = env.launch(spec);
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    const frost::io::ProcessId pid = (*process)->pid();
    const ProcessWatch child(pid);
    ASSERT_TRUE(child.valid());
    process->reset();
    EXPECT_FALSE(child.exitsWithin(500ms)) << "the release ended a child launched without killOnClose";
    child.kill();
    EXPECT_TRUE(child.exitsWithin(30s));
#ifndef _WIN32
    // The exit watch outlived the object and reaps the child once it exits.
    EXPECT_TRUE(reapedWithin(static_cast<pid_t>(pid), 30s)) << "the exited child was left a zombie";
#endif
    EXPECT_TRUE(env.settlesWithin(30s)) << env.boundObjects() << " states still bound";
}

#ifndef _WIN32
TEST(AsyncProcess, ReleasedChildrenAreReapedByPid) {
    AsyncEnv env;
    // One that is still running at release (killed, then reaped on exit) and one that exited before.
    auto running = env.launch(childSpec({"sleep", "60000"}));
    auto finished = env.launch(childSpec({"exit", "3"}));
    ASSERT_TRUE(running.has_value()) << running.error().describe();
    ASSERT_TRUE(finished.has_value()) << finished.error().describe();
    const auto runningPid = static_cast<pid_t>((*running)->pid());
    const auto finishedPid = static_cast<pid_t>((*finished)->pid());
    ASSERT_EQ(env.run((*finished)->exited()).value().exitCode, 3);
    // Exited but never reaped while the object lives: the pid stays reserved.
    EXPECT_FALSE(reapedWithin(finishedPid, 50ms));
    running->reset();
    finished->reset();
    EXPECT_TRUE(reapedWithin(runningPid, 30s));
    EXPECT_TRUE(reapedWithin(finishedPid, 30s));
    EXPECT_TRUE(env.settlesWithin(30s)) << env.boundObjects() << " states still bound";
}
#endif

TEST(AsyncProcess, AReactorShutdownCancelsEverythingAndRefusesLaterLaunches) {
    std::optional<ProcessWatch> watch;
    frost::io::ProcessId pid = 0;
    {
        AsyncEnv env;
        auto process = env.launch(childSpec({"sleep", "60000"}));
        ASSERT_TRUE(process.has_value()) << process.error().describe();
        pid = (*process)->pid();
        watch.emplace(pid);
        ASSERT_TRUE(watch->valid());
        std::array<char, 64> buffer{};
        auto read = env.pool().spawn(Lane::UserVisible, (*process)->readStdout(buffer));
        auto exit = env.pool().spawn(Lane::UserVisible, (*process)->exited());
        std::this_thread::sleep_for(50ms);
        env.reactor().shutdown();
        EXPECT_EQ(codeOf(read.join()), ErrorCode::Cancelled);
        EXPECT_EQ(codeOf(exit.join()), ErrorCode::Cancelled);
        // Operations started after the shutdown end at once.
        const auto late = env.run((*process)->exited());
        ASSERT_FALSE(late.has_value());
        EXPECT_EQ(late.error().code(), ErrorCode::Cancelled);
        const auto refused = env.launch(childSpec({"exit", "0"}));
        ASSERT_FALSE(refused.has_value());
        EXPECT_EQ(refused.error().code(), ErrorCode::Cancelled);
        // Destroying the object now still ends the child (killOnClose) as its last reference drops.
        process->reset();
        EXPECT_TRUE(env.settlesWithin(5s)) << env.boundObjects() << " states still bound";
    }
    EXPECT_TRUE(watch->exitsWithin(30s));
#ifndef _WIN32
    // After a shutdown nothing waits for the child: reap it here if the destructor could not.
    int status = 0;
    static_cast<void>(waitpid(static_cast<pid_t>(pid), &status, 0));
#endif
}

TEST(AsyncProcess, CallerDrivenSchedulersAreRefused) {
    AsyncEnv env;
    frost::sched::SimExecutor sim;
    const auto refused = frost::io::realProcessLauncher().launchAsync(childSpec({"exit", "0"}), {env.reactor(), sim});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument) << refused.error().describe();
}

TEST(AsyncProcess, LaunchErrorsMatchTheBlockingLauncher) {
    AsyncEnv env;
    ProcessSpec relative;
    relative.executable = "io_test_child";
    const auto invalid = env.launch(relative);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
    ProcessSpec missing = childSpec({});
    missing.executable += ".missing";
    const auto notFound = env.launch(missing);
    ASSERT_FALSE(notFound.has_value());
    EXPECT_EQ(notFound.error().code(), ErrorCode::NotFound) << notFound.error().describe();
    EXPECT_EQ(env.boundObjects(), 0);
}

TEST(AsyncProcess, StreamsThatAreNotPipesAreRefused) {
    AsyncEnv env;
    ProcessSpec spec = childSpec({"exit", "0"});
    spec.stdoutMode = Stdio::Null;
    spec.stderrMode = Stdio::Null;
    auto process = env.launch(spec);
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    std::array<char, 8> buffer{};
    EXPECT_EQ(codeOf(env.run((*process)->readStdout(buffer))), ErrorCode::InvalidArgument);
    EXPECT_EQ(codeOf(env.run((*process)->readStderr(buffer))), ErrorCode::InvalidArgument);
    EXPECT_EQ(codeOf(env.run((*process)->writeStdin("x"))), ErrorCode::InvalidArgument);
    const auto stdinData = env.run((*process)->communicate("data", 10s));
    ASSERT_FALSE(stdinData.has_value());
    EXPECT_EQ(stdinData.error().code(), ErrorCode::InvalidArgument);
    const auto result = env.run((*process)->communicate({}, 120s));
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 0);
    // An empty buffer is refused on a pipe too.
    auto piped = env.launch(childSpec({"exit", "0"}));
    ASSERT_TRUE(piped.has_value()) << piped.error().describe();
    EXPECT_EQ(codeOf(env.run((*piped)->readStdout({}))), ErrorCode::InvalidArgument);
}

TEST(AsyncProcess, ManyConcurrentProcesses) {
    AsyncEnv env(2);
    constexpr std::size_t kCount = 24;
    std::vector<std::unique_ptr<IAsyncProcess>> processes;
    std::vector<frost::sched::JoinHandle<frost::Result<frost::io::ProcessResult>>> calls;
    for (std::size_t i = 0; i < kCount; ++i) {
        auto process = env.launch(withStdin(childSpec({"tee"})));
        ASSERT_TRUE(process.has_value()) << process.error().describe();
        calls.push_back(env.pool().spawn(Lane::UserVisible, (*process)->communicate(pattern(70'000 + i), 120s)));
        processes.push_back(std::move(*process));
    }
    for (std::size_t i = 0; i < kCount; ++i) {
        const auto result = calls[i].join();
        ASSERT_TRUE(result.has_value()) << i << ": " << result.error().describe();
        EXPECT_EQ(result->exitCode, 0);
        EXPECT_TRUE(result->stdoutBytes == pattern(70'000 + i)) << i;
        EXPECT_TRUE(result->stderrBytes == pattern(70'000 + i)) << i;
    }
    processes.clear();
    EXPECT_TRUE(env.settlesWithin(30s)) << env.boundObjects() << " states still bound";
}

} // namespace
