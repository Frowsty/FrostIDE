// SPDX-License-Identifier: Apache-2.0
//
/*
 * The pipe fault suite for IAsyncProcess (ADR 0020; label "stress", run in Release and the sanitizer
 * trees): real OS-level faults injected into live pipes. A child killed in the middle of a large write
 * (IoError, never a SIGPIPE death), writes from pool workers to a dead child, huge output streamed with
 * bounded memory (1 GiB optimised, 256 MiB otherwise), a stalled reader that must stall the child, stdout
 * closed early, reads cancelled at random points while data flows (no byte lost; the cancel latency is
 * measured), destruction with every kind of operation pending, a reactor shutdown with many live
 * processes, and launch/destroy churn from several threads. Each test prints its counts.
 */

#include "frost/io/async_process.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/thread_pool.hpp"

#include "process_env.hpp"
#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <random>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>

#include <csignal>
#endif

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::Result;
using frost::io::IAsyncProcess;
using frost::io::ProcessSpec;
using frost::io::Stdio;
using frost::sched::Lane;
using frost::sched::Task;
using frost::test::AsyncEnv;
using frost::test::childSpec;
using frost::test::pattern;
using frost::test::ProcessWatch;

ProcessSpec withStdin(ProcessSpec spec) {
    spec.stdinMode = Stdio::Pipe;
    return spec;
}

template<class T>
ErrorCode codeOf(const Result<T>& result) {
    return result ? ErrorCode::Unknown : result.error().code();
}

// The default SIGPIPE disposition: a SIGPIPE raised on any thread would end the test process.
void restoreDefaultSigpipe() {
#ifndef _WIN32
    ASSERT_NE(std::signal(SIGPIPE, SIG_DFL), SIG_ERR);
#endif
}

using Clock = std::chrono::steady_clock;

[[nodiscard]] double millis(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

TEST(AsyncPipeFaults, AChildKilledMidWriteFailsTheWriteWithIoError) {
    restoreDefaultSigpipe();
    AsyncEnv env;
    constexpr int kRounds = 20;
    std::mt19937 random(20261101);
    for (int round = 0; round < kRounds; ++round) {
        auto process = env.launch(withStdin(childSpec({"cat"})));
        ASSERT_TRUE(process.has_value()) << process.error().describe();
        // cat echoes until its stdout is full (nobody reads beyond the internal buffer), then stalls.
        auto write = env.pool().spawn(Lane::UserVisible, (*process)->writeStdin(std::string(64 * 1024 * 1024, 'k')));
        std::this_thread::sleep_for(std::chrono::milliseconds(5 + random() % 40));
        ASSERT_TRUE((*process)->terminate().has_value());
        const auto written = write.join();
        ASSERT_FALSE(written.has_value()) << "round " << round;
        EXPECT_EQ(written.error().code(), ErrorCode::IoError) << written.error().describe();
        const auto status = env.run((*process)->exited());
        ASSERT_TRUE(status.has_value()) << status.error().describe();
        EXPECT_EQ(status->exitCode, frost::io::kTerminatedExitCode);
    }
    std::printf("[ faults   ] %d writes failed with IoError after the kill\n", kRounds);
}

// Writes `rounds` times from a pool worker to a child that is gone; counts the IoErrors.
Task<int> writeToTheDead(IAsyncProcess& process, int rounds) {
    int failures = 0;
    for (int i = 0; i < rounds; ++i) {
        const auto written = co_await process.writeStdin(std::string(256 * 1024, 'd'));
        if (!written && written.error().code() == ErrorCode::IoError) {
            ++failures;
        }
    }
    co_return failures;
}

TEST(AsyncPipeFaults, WritesFromPoolWorkersToADeadChildFailWithoutASignal) {
    restoreDefaultSigpipe();
    AsyncEnv env;
    auto process = env.launch(withStdin(childSpec({"read-then-exit", "0"})));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    ASSERT_TRUE(env.run((*process)->exited()).has_value());
    constexpr int kRounds = 200;
    const int failures = env.run(writeToTheDead(**process, kRounds));
    EXPECT_EQ(failures, kRounds);
}

TEST(AsyncPipeFaults, HugeOutputStreamsWithBoundedMemory) {
    AsyncEnv env;
    const std::size_t size =
        frost::test::kOptimized && !frost::test::kSanitized ? std::size_t{1} << 30 : std::size_t{256} << 20;
    auto process = env.launch(childSpec({"stream", std::to_string(size)}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    // Checked while streaming: only the 64 KiB read buffer is ever held.
    struct Tally {
        std::size_t total = 0;
        std::size_t reads = 0;
        std::size_t largest = 0;
        bool intact = true;
    };
    const auto start = Clock::now();
    const auto tally = env.run([](IAsyncProcess& p) -> Task<Result<Tally>> {
        Tally t;
        std::vector<char> buffer(64 * 1024);
        for (;;) {
            auto got = co_await p.readStdout(buffer);
            if (!got) {
                co_return std::unexpected(std::move(got.error()));
            }
            if (*got == 0) {
                co_return t;
            }
            for (std::size_t i = 0; i < *got && t.intact; ++i) {
                t.intact = static_cast<std::size_t>(static_cast<unsigned char>(buffer[i])) == (t.total + i) % 251;
            }
            t.total += *got;
            ++t.reads;
            t.largest = std::max(t.largest, *got);
        }
    }(**process));
    const auto elapsed = Clock::now() - start;
    ASSERT_TRUE(tally.has_value()) << tally.error().describe();
    EXPECT_EQ(tally->total, size);
    EXPECT_TRUE(tally->intact);
    EXPECT_LE(tally->largest, std::size_t{64 * 1024});
    const auto status = env.run((*process)->exited());
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->exitCode, 0);
    std::printf("[ stream   ] %zu MiB in %.0f ms (%.0f MiB/s), %zu reads, largest %zu bytes\n", size >> 20,
                millis(elapsed), static_cast<double>(size >> 20) / (millis(elapsed) / 1000.0), tally->reads,
                tally->largest);
}

TEST(AsyncPipeFaults, AStalledReaderStallsTheChildAndMemoryStaysBounded) {
    AsyncEnv env;
    constexpr std::size_t kSize = 32 * 1024 * 1024;
    auto process = env.launch(childSpec({"flood", std::to_string(kSize)}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    // Only stdout is read: the child blocks on its full stderr pipe, and stdout stops too.
    std::atomic<std::size_t> outBytes{0};
    std::stop_source stopOut;
    auto out = env.pool().spawn(
        Lane::UserVisible,
        [](IAsyncProcess& p, std::atomic<std::size_t>& count, std::stop_token token) -> Task<Result<std::string>> {
            std::string all;
            std::vector<char> buffer(16 * 1024);
            for (;;) {
                auto got = co_await p.readStdout(buffer, token);
                if (!got) {
                    if (got.error().code() == ErrorCode::Cancelled) {
                        co_return all;
                    }
                    co_return std::unexpected(std::move(got.error()));
                }
                if (*got == 0) {
                    co_return all;
                }
                all.append(buffer.data(), *got);
                count.store(all.size(), std::memory_order_relaxed);
            }
        }(**process, outBytes, stopOut.get_token()));
    std::size_t before = 0;
    std::size_t after = 0;
    for (int attempt = 0; attempt < 50; ++attempt) {
        std::this_thread::sleep_for(100ms);
        before = outBytes.load(std::memory_order_relaxed);
        std::this_thread::sleep_for(200ms);
        after = outBytes.load(std::memory_order_relaxed);
        if (before == after) {
            break;
        }
    }
    EXPECT_EQ(before, after) << "stdout kept flowing although stderr was never read";
    EXPECT_LT(after, kSize / 2);
    // Reading stderr now releases the child; stdout resumes where it stopped, nothing lost.
    const auto err = env.drain(**process, true);
    ASSERT_TRUE(err.has_value()) << err.error().describe();
    const auto outResult = out.join();
    ASSERT_TRUE(outResult.has_value()) << outResult.error().describe();
    const std::string expected = pattern(kSize);
    EXPECT_TRUE(*err == expected) << err->size();
    EXPECT_TRUE(*outResult == expected) << outResult->size();
    std::printf("[ stall    ] stdout stalled at %zu bytes until stderr was read\n", after);
}

TEST(AsyncPipeFaults, StdoutClosedEarlyIsEofEveryTime) {
    AsyncEnv env;
    constexpr int kRounds = 40;
    for (int round = 0; round < kRounds; ++round) {
        auto process = env.launch(childSpec({"close-stdout-then-sleep", "20"}));
        ASSERT_TRUE(process.has_value()) << process.error().describe();
        const auto out = env.drain(**process);
        ASSERT_TRUE(out.has_value()) << out.error().describe();
        EXPECT_TRUE(out->empty());
        const auto status = env.run((*process)->exited());
        ASSERT_TRUE(status.has_value()) << status.error().describe();
        EXPECT_EQ(status->exitCode, 0);
    }
}

// Writes `chunks` pieces of growing size (byte i of the stream is i % 251), then closes stdin.
Task<Result<std::size_t>> feed(IAsyncProcess& process, int chunks) {
    std::size_t offset = 0;
    for (int i = 0; i < chunks; ++i) {
        const std::size_t size = 1 + static_cast<std::size_t>(i) * 37 % 9000;
        std::string piece(size, '\0');
        for (std::size_t k = 0; k < size; ++k) {
            piece[k] = static_cast<char>((offset + k) % 251);
        }
        auto written = co_await process.writeStdin(std::move(piece));
        if (!written) {
            co_return std::unexpected(std::move(written.error()));
        }
        offset += size;
    }
    process.closeStdin();
    co_return offset;
}

TEST(AsyncPipeFaults, ReadsCancelledAtRandomPointsLoseNoByte) {
    AsyncEnv env(2);
    auto process = env.launch(withStdin(childSpec({"cat"})));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    auto feeder = env.pool().spawn(Lane::Background, feed(**process, 3000));
    std::mt19937 random(7);
    std::string received;
    std::array<char, 4096> buffer{};
    int cancelled = 0;
    int served = 0;
    for (;;) {
        std::stop_source stop;
        auto read = env.pool().spawn(Lane::UserVisible, (*process)->readStdout(buffer, stop.get_token()));
        const auto pause = std::chrono::microseconds(random() % 300);
        if (pause.count() > 0) {
            std::this_thread::sleep_for(pause);
        }
        static_cast<void>(stop.request_stop());
        const auto got = read.join();
        if (!got) {
            ASSERT_EQ(got.error().code(), ErrorCode::Cancelled) << got.error().describe();
            ++cancelled;
            continue;
        }
        if (*got == 0) {
            break;
        }
        ++served;
        received.append(buffer.data(), *got);
    }
    const auto fed = feeder.join();
    ASSERT_TRUE(fed.has_value()) << fed.error().describe();
    ASSERT_EQ(received.size(), *fed);
    EXPECT_TRUE(received == pattern(*fed));
    EXPECT_GT(cancelled, 0);
    std::printf("[ cancel   ] %zu bytes intact; %d reads served, %d cancelled\n", received.size(), served, cancelled);
}

TEST(AsyncPipeFaults, ACancelledReadEndsWithinTenMilliseconds) {
    AsyncEnv env;
    auto process = env.launch(withStdin(childSpec({"cat"})));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    constexpr int kRounds = 200;
    std::vector<double> latencies;
    std::array<char, 64> buffer{};
    for (int round = 0; round < kRounds; ++round) {
        std::stop_source stop;
        auto read = env.pool().spawn(Lane::UserVisible, (*process)->readStdout(buffer, stop.get_token()));
        std::this_thread::sleep_for(1ms);
        const auto at = Clock::now();
        static_cast<void>(stop.request_stop());
        const auto got = read.join();
        latencies.push_back(millis(Clock::now() - at));
        ASSERT_EQ(codeOf(got), ErrorCode::Cancelled) << "round " << round;
    }
    std::ranges::sort(latencies);
    const double p50 = latencies[latencies.size() / 2];
    const double p99 = latencies[latencies.size() * 99 / 100];
    EXPECT_LT(p50, 10.0);
    std::printf("[ cancel   ] latency p50 %.3f ms, p99 %.3f ms, max %.3f ms\n", p50, p99, latencies.back());
    // Nothing was lost to all those cancels.
    ASSERT_TRUE(env.run((*process)->writeStdin("tail")).has_value());
    (*process)->closeStdin();
    EXPECT_EQ(env.drain(**process).value(), "tail");
}

TEST(AsyncPipeFaults, DestructionWithEveryOperationPending) {
    AsyncEnv env(2);
    constexpr int kRounds = 60;
    std::mt19937 random(99);
    for (int round = 0; round < kRounds; ++round) {
        auto process = env.launch(withStdin(childSpec({"sleep", "60000"})));
        ASSERT_TRUE(process.has_value()) << process.error().describe();
        const ProcessWatch watch((*process)->pid());
        std::array<char, 64> outBuffer{};
        std::array<char, 64> errBuffer{};
        auto out = env.pool().spawn(Lane::UserVisible, (*process)->readStdout(outBuffer));
        auto err = env.pool().spawn(Lane::Background, (*process)->readStderr(errBuffer));
        auto exit = env.pool().spawn(Lane::UserVisible, (*process)->exited());
        auto write = env.pool().spawn(Lane::UserVisible, (*process)->writeStdin(std::string(4 * 1024 * 1024, 'p')));
        const auto pause = std::chrono::microseconds(random() % 3000);
        if (pause.count() > 0) {
            std::this_thread::sleep_for(pause);
        }
        process->reset();
        EXPECT_EQ(codeOf(out.join()), ErrorCode::Cancelled) << "round " << round;
        EXPECT_EQ(codeOf(err.join()), ErrorCode::Cancelled) << "round " << round;
        EXPECT_EQ(codeOf(exit.join()), ErrorCode::Cancelled) << "round " << round;
        EXPECT_EQ(codeOf(write.join()), ErrorCode::Cancelled) << "round " << round;
        EXPECT_TRUE(watch.exitsWithin(30s)) << "round " << round;
    }
    EXPECT_TRUE(env.settlesWithin(30s)) << env.boundObjects() << " states still bound";
}

TEST(AsyncPipeFaults, AReactorShutdownWithManyLiveProcesses) {
    std::vector<std::unique_ptr<ProcessWatch>> watches;
    std::vector<frost::io::ProcessId> pids;
    {
        AsyncEnv env(2);
        constexpr int kCount = 16;
        std::vector<std::unique_ptr<IAsyncProcess>> processes;
        std::vector<std::vector<char>> buffers(kCount, std::vector<char>(64));
        std::vector<frost::sched::JoinHandle<Result<std::size_t>>> reads;
        std::vector<frost::sched::JoinHandle<Result<frost::io::ExitStatus>>> exits;
        for (int i = 0; i < kCount; ++i) {
            auto process = env.launch(childSpec({"sleep", "60000"}));
            ASSERT_TRUE(process.has_value()) << process.error().describe();
            pids.push_back((*process)->pid());
            watches.push_back(std::make_unique<ProcessWatch>((*process)->pid()));
            reads.push_back(
                env.pool().spawn(Lane::UserVisible, (*process)->readStdout(buffers[static_cast<std::size_t>(i)])));
            exits.push_back(env.pool().spawn(Lane::UserVisible, (*process)->exited()));
            processes.push_back(std::move(*process));
        }
        std::this_thread::sleep_for(50ms);
        env.reactor().shutdown();
        for (auto& read : reads) {
            EXPECT_EQ(codeOf(read.join()), ErrorCode::Cancelled);
        }
        for (auto& exit : exits) {
            EXPECT_EQ(codeOf(exit.join()), ErrorCode::Cancelled);
        }
        processes.clear(); // The last references: killOnClose ends each child.
        EXPECT_TRUE(env.settlesWithin(5s)) << env.boundObjects() << " states still bound";
    }
    for (const auto& watch : watches) {
        EXPECT_TRUE(watch->exitsWithin(30s));
    }
#ifndef _WIN32
    // After a shutdown nothing waits for the children: reap the ones the destructors could not.
    for (const frost::io::ProcessId pid : pids) {
        int status = 0;
        static_cast<void>(waitpid(static_cast<pid_t>(pid), &status, 0));
    }
#endif
}

TEST(AsyncPipeFaults, LaunchAndDestroyChurnFromSeveralThreads) {
    AsyncEnv env(2);
    constexpr int kThreads = 4;
    constexpr int kRounds = 25;
    std::atomic<int> completed{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&env, &completed, &failures, t] {
            for (int round = 0; round < kRounds; ++round) {
                auto process = env.launch(withStdin(childSpec({"tee"})));
                if (!process) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if ((round + t) % 3 == 0) {
                    continue; // Destroyed at once, before any operation.
                }
                const std::string input = pattern(static_cast<std::size_t>(1000 + round * 97 + t));
                const auto result = env.run((*process)->communicate(input, 120s));
                if (!result || result->exitCode != 0 || result->stdoutBytes != input || result->stderrBytes != input) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_GT(completed.load(), 0);
    EXPECT_TRUE(env.settlesWithin(30s)) << env.boundObjects() << " states still bound";
    std::printf("[ churn    ] %d communicate calls over %d threads\n", completed.load(), kThreads);
}

} // namespace
