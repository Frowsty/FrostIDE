// SPDX-License-Identifier: Apache-2.0
//
/*
 * Report-only measurements of the reactor and the file watchers for ADR 0020 (label "perf"): change-to-batch
 * latency (p50/p99; Windows timers tick at 15.6 ms), Reactor::shutdown with ten watches (Budget: B20 path),
 * native setup per 10k directories and polling cost per 10k files. They assert nothing (new thresholds
 * would be new budgets) and measure only in optimised, unsanitised builds; elsewhere they skip with a reason.
 */

#include "frost/io/file_watcher.hpp"
#include "frost/io/reactor.hpp"

#include "support/scratch_dir.hpp"
#include "support/test_env.hpp"
#include "watch_env.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::io::FileChangeBatch;
using frost::io::IDirectoryWatch;
using frost::io::WatchOptions;
using frost::test::nativeJoin;
using frost::test::WatchEnv;
using Clock = std::chrono::steady_clock;

[[nodiscard]] double milliseconds(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

constexpr const char* kSkipReason = "timings mean something only in optimised, unsanitised builds";

TEST(WatcherPerf, ChangeToBatchLatency) {
    if (!frost::test::kAssertBudgets) {
        GTEST_SKIP() << kSkipReason;
    }
    frost::test::ScratchDir scratch("perf-latency");
    WatchEnv env;
    WatchOptions options;
    options.debounce = 0ms; // The raw path: kernel, reactor, queue, scheduler.
    options.maxDelay = 0ms;
    auto watch = env.watch(scratch.path(), options);
    ASSERT_TRUE(watch);
    std::vector<double> samples;
    for (int i = 0; i < 200; ++i) {
        const std::string file = nativeJoin(scratch.path(), std::format("l{}", i));
        const auto start = Clock::now();
        frost::test::writePlain(file, "x");
        ASSERT_TRUE(
            env.until(**watch, nullptr, [&](const FileChangeBatch& b) { return frost::test::mentions(b, file); }));
        samples.push_back(milliseconds(Clock::now() - start));
    }
    std::ranges::sort(samples);
    std::printf("[perf] change-to-batch latency: p50 %.2f ms, p99 %.2f ms (%zu samples)\n", samples[samples.size() / 2],
                samples[samples.size() * 99 / 100], samples.size());
    watch->reset();
}

TEST(WatcherPerf, ReactorShutdownWithTenWatches) {
    if (!frost::test::kAssertBudgets) {
        GTEST_SKIP() << kSkipReason;
    }
    frost::test::ScratchDir scratch("perf-shutdown");
    WatchEnv env;
    std::vector<std::unique_ptr<IDirectoryWatch>> watches;
    for (int i = 0; i < 10; ++i) {
        const std::string dir = nativeJoin(scratch.path(), std::format("w{}", i));
        frost::test::makeDirectory(dir);
        auto watch = env.watch(dir);
        ASSERT_TRUE(watch);
        watches.push_back(std::move(*watch));
    }
    const auto start = Clock::now();
    env.reactor().shutdown(); // Budget: B20 (part of exit).
    std::printf("[perf] Reactor::shutdown with 10 watches: %.3f ms\n", milliseconds(Clock::now() - start));
    watches.clear();
}

TEST(WatcherPerf, NativeSetupPer10kDirectories) {
    if (!frost::test::kAssertBudgets) {
        GTEST_SKIP() << kSkipReason;
    }
    frost::test::ScratchDir scratch("perf-setup");
    for (int a = 0; a < 100; ++a) {
        for (int b = 0; b < 100; ++b) {
            frost::test::makeDirectory(
                nativeJoin(nativeJoin(scratch.path(), std::format("a{}", a)), std::format("b{}", b)));
        }
    }
    WatchEnv env;
    const auto start = Clock::now();
    auto watch = env.watch(scratch.path());
    ASSERT_TRUE(watch);
    // On Linux watch() returns after the root's watch; wait for the walk of the other directories.
    while ((*watch)->status().pendingSetup != 0) {
        std::this_thread::sleep_for(1ms);
    }
    std::printf("[perf] native setup of 10100 directories: %.1f ms, %zu OS watches\n",
                milliseconds(Clock::now() - start), (*watch)->status().osWatches);
    watch->reset();
}

TEST(WatcherPerf, PollingScanPer10kFiles) {
    if (!frost::test::kAssertBudgets) {
        GTEST_SKIP() << kSkipReason;
    }
    frost::test::ScratchDir scratch("perf-poll");
    for (int d = 0; d < 100; ++d) {
        const std::string dir = nativeJoin(scratch.path(), std::format("d{}", d));
        frost::test::makeDirectory(dir);
        for (int f = 0; f < 100; ++f) {
            frost::test::writePlain(nativeJoin(dir, std::format("f{}", f)), "p");
        }
    }
    WatchEnv env;
    WatchOptions options;
    options.forcePolling = true;
    const auto start = Clock::now();
    auto watch = env.watch(scratch.path(), options); // Returns after the first full scan.
    ASSERT_TRUE(watch);
    std::printf("[perf] polling scan of 10000 files in 100 directories: %.1f ms\n", milliseconds(Clock::now() - start));
    watch->reset();
}

} // namespace
