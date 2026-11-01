// SPDX-License-Identifier: Apache-2.0
//
// The stall detector and watchdog (frost/diag/stall_watchdog.hpp) on scripted timelines: the
// UI thread's B21 count (the 50 ms threshold exactly, short-event floods, nested loops and
// idle sections, nothing before arming), the watchdog's own view (agreement with the count,
// late wakes marking a stall unreliable, the debugger rule), its freeze reports (contents,
// the interim report after 2 s and its final rewrite, the session cap and rate limit), and a
// bounded stop of the real thread. ManualClock, MemFs and FakeThreadSampler throughout; the
// watchdog runs unthreaded (Options::threaded = false) and the test calls poll().

#include "frost/core/clock.hpp"
#include "frost/diag/stall_watchdog.hpp"
#include "frost/diag/thread_sampler.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ManualClock;
using frost::diag::StallDetector;
using frost::diag::StallWatchdog;
using SteadyTime = frost::IClock::SteadyTime;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;
const std::string kFreezes = kWindows ? "C:\\cache\\crashes\\freezes" : "/cache/crashes/freezes";

SteadyTime at(std::chrono::nanoseconds offset) { return SteadyTime(std::chrono::seconds(1000) + offset); }

TEST(StallDetector, ExactlyTheThresholdIsNotAStall) {
    StallDetector detector;
    detector.eventLoopWillBlock(at(0ms)); // Arms.
    detector.eventLoopWoke(at(1ms));
    detector.eventLoopWillBlock(at(51ms)); // 50 ms busy: not a stall.
    EXPECT_EQ(detector.stalls(), 0u);
    detector.eventLoopWoke(at(100ms));
    detector.eventLoopWillBlock(at(150ms + 1ns)); // 50 ms + 1 ns: a stall.
    EXPECT_EQ(detector.stalls(), 1u);
    EXPECT_EQ(detector.longest(), 50ms + 1ns);
}

TEST(StallDetector, AFloodOfShortEventsIsNotAStall) {
    StallDetector detector;
    detector.eventLoopWillBlock(at(0ms));
    detector.eventLoopWoke(at(0ms));
    for (int i = 1; i <= 100; ++i) {
        detector.eventStarted(at(std::chrono::milliseconds(10 * i))); // 1 s busy, progress every 10 ms.
    }
    detector.eventLoopWillBlock(at(1005ms));
    EXPECT_EQ(detector.stalls(), 0u);
}

TEST(StallDetector, OneLongEventIsAStallEvenAmongShortOnes) {
    StallDetector detector;
    detector.eventLoopWillBlock(at(0ms));
    detector.eventLoopWoke(at(0ms));
    detector.eventStarted(at(10ms));
    detector.eventStarted(at(130ms)); // The event that began at 10 ms ran 120 ms.
    detector.eventStarted(at(140ms));
    detector.eventLoopWillBlock(at(150ms));
    EXPECT_EQ(detector.stalls(), 1u);
    EXPECT_EQ(detector.longest(), 120ms);
}

TEST(StallDetector, NestedLoopsAndIdleSectionsCountAsIdle) {
    StallDetector detector;
    detector.eventLoopWillBlock(at(0ms));
    // A modal dialog's nested loop: it blocks (its own aboutToBlock) for 300 ms inside an event.
    detector.eventLoopWoke(at(10ms));
    detector.eventStarted(at(11ms));
    detector.eventLoopWillBlock(at(20ms));
    detector.eventLoopWoke(at(320ms));
    detector.eventLoopWillBlock(at(325ms));
    // A native modal loop marked idle (ScopedUiIdle, or WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE).
    detector.eventLoopWoke(at(400ms));
    detector.idleBegin(at(410ms));
    detector.eventStarted(at(500ms)); // Events inside the idle section are not progress or stalls.
    detector.idleEnd(at(900ms));
    detector.eventLoopWillBlock(at(910ms));
    EXPECT_EQ(detector.stalls(), 0u);
    // Unbalanced idleEnd() is ignored.
    detector.idleEnd(at(950ms));
    detector.eventLoopWoke(at(1000ms));
    detector.eventLoopWillBlock(at(1100ms));
    EXPECT_EQ(detector.stalls(), 1u);
}

TEST(StallDetector, NothingCountsBeforeArming) {
    StallDetector detector;
    detector.eventLoopWoke(at(0ms));
    detector.eventStarted(at(500ms));
    EXPECT_FALSE(detector.armed());
    EXPECT_EQ(detector.progressNs(), 0);
    detector.eventLoopWillBlock(at(900ms)); // Start-up (B1) is over: armed, nothing counted.
    EXPECT_TRUE(detector.armed());
    EXPECT_EQ(detector.stalls(), 0u);
    EXPECT_LT(detector.progressNs(), 0);
}

TEST(StallDetector, AnEventAfterAboutToBlockBeginsABusyStretch) {
    // The glib dispatcher dispatches after aboutToBlock() without awake().
    StallDetector detector;
    detector.eventLoopWillBlock(at(0ms));
    EXPECT_TRUE(detector.eventStarted(at(10ms)));
    EXPECT_GT(detector.progressNs(), 0);
    detector.eventLoopWillBlock(at(100ms));
    EXPECT_EQ(detector.stalls(), 1u);
}

struct Rig {
    ManualClock clock{at(0ms), frost::IClock::SystemTime(std::chrono::sys_days(std::chrono::year(2026) / 11 / 1))};
    frost::io::MemFs fs{clock};
    frost::diag::FakeThreadSampler* sampler = nullptr;
    std::unique_ptr<StallWatchdog> watchdog;
    bool debugger = false;

    explicit Rig(StallWatchdog::Options options = {}) {
        fs.addRoot(kWindows ? "C:\\" : "/");
        EXPECT_TRUE(fs.createDirectories(kFreezes).has_value());
        options.freezesDirectory = kFreezes;
        options.version = "1.2.3";
        options.threaded = false;
        options.debuggerPresent = [this] { return debugger; };
        // Frame 0 inside this test binary (a module), frame 1 nowhere.
        auto fake = std::make_unique<frost::diag::FakeThreadSampler>(
            std::initializer_list<std::uintptr_t>{reinterpret_cast<std::uintptr_t>(&at), 0x10});
        sampler = fake.get();
        auto started = StallWatchdog::start(std::move(options), std::move(fake), fs, clock);
        EXPECT_TRUE(started.has_value());
        watchdog = std::move(*started);
    }

    // Advances the clock in 10 ms polls for `duration`.
    void run(std::chrono::milliseconds duration) {
        for (auto left = duration; left > 0ms; left -= 10ms) {
            clock.advance(std::min<std::chrono::milliseconds>(10ms, left));
            watchdog->poll();
        }
    }

    // One busy stretch of `duration` (awake, no progress, aboutToBlock), then 20 ms idle.
    void stall(std::chrono::milliseconds duration) {
        watchdog->eventLoopWoke();
        watchdog->poll();
        run(duration);
        watchdog->eventLoopWillBlock();
        run(20ms);
    }

    std::vector<std::string> reports() const {
        std::vector<std::string> found;
        for (const std::string& path : fs.allPaths()) {
            if (path.ends_with(".json")) {
                found.push_back(path);
            }
        }
        return found;
    }
};

TEST(StallWatchdog, WritesAFreezeReportWithSamples) {
    Rig rig;
    rig.watchdog->eventLoopWillBlock();
    rig.stall(300ms);
    const frost::diag::StallStats stats = rig.watchdog->stats();
    EXPECT_EQ(stats.stalls, 1u);
    EXPECT_EQ(stats.watchdogStalls, 1u);
    EXPECT_EQ(stats.reportsWritten, 1u);
    const auto reports = rig.reports();
    ASSERT_EQ(reports.size(), 1u);
    const std::string name(frost::io::fileName(reports.front()));
    EXPECT_TRUE(name.starts_with("freeze-20261101T000000Z-")) << name; // The wall clock at its start.
    EXPECT_TRUE(name.ends_with("-1.json")) << name;
    const std::string json = rig.fs.readFile(reports.front()).value();
    for (const char* field :
         {"\"schema\": \"frost-freeze-report/1\"", "\"version\": \"1.2.3\"", "\"id\": 7", "\"duration_ms\": 300",
          "\"ongoing\": false", "\"unreliable\": false", "\"threshold_ms\": 50", "\"sample_period_ms\": 10",
          "\"kind\": \"", "[-1, \"0x10\"]", "[0, \"0x"}) {
        EXPECT_TRUE(json.contains(field)) << field << " in\n" << json;
    }
    // Sampling from 25 ms of no progress every 10 ms: 28 samples in 300 ms.
    const auto info = rig.watchdog->lastReport();
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->samples, 28u);
    EXPECT_EQ(info->durationMs, 300);
    EXPECT_FALSE(info->ongoing);
}

TEST(StallWatchdog, ShortStretchesNeverSampleOrReport) {
    Rig rig;
    rig.watchdog->eventLoopWillBlock();
    for (int i = 0; i < 20; ++i) {
        rig.stall(20ms);
    }
    EXPECT_EQ(rig.sampler->calls.load(), 0u);
    EXPECT_EQ(rig.watchdog->stats().stalls, 0u);
    EXPECT_TRUE(rig.reports().empty());
}

TEST(StallWatchdog, InterimReportAfterTwoSecondsIsRewrittenAtTheEnd) {
    Rig rig;
    rig.watchdog->eventLoopWillBlock();
    rig.watchdog->eventLoopWoke();
    rig.watchdog->poll();
    rig.run(2100ms);
    auto interim = rig.watchdog->lastReport();
    ASSERT_TRUE(interim.has_value());
    EXPECT_TRUE(interim->ongoing);
    ASSERT_EQ(rig.reports().size(), 1u);
    EXPECT_TRUE(rig.fs.readFile(rig.reports().front())->contains("\"ongoing\": true"));
    EXPECT_EQ(rig.watchdog->stats().reportsWritten, 0u);
    rig.run(400ms);
    rig.watchdog->eventLoopWillBlock();
    rig.run(20ms);
    const auto reports = rig.reports();
    ASSERT_EQ(reports.size(), 1u); // The same file, replaced.
    EXPECT_EQ(reports.front(), interim->path);
    const std::string json = rig.fs.readFile(reports.front()).value();
    EXPECT_TRUE(json.contains("\"ongoing\": false"));
    EXPECT_TRUE(json.contains("\"duration_ms\": 2500")) << json.substr(0, 400);
    // 100 samples 10 ms apart, then one per 100 ms.
    const auto final = rig.watchdog->lastReport();
    ASSERT_TRUE(final.has_value());
    EXPECT_GE(final->samples, 100u + 14u);
    EXPECT_LE(final->samples, 100u + 16u);
    EXPECT_EQ(rig.watchdog->stats().reportsWritten, 1u);
}

TEST(StallWatchdog, ReportsAreCappedAndRateLimited) {
    StallWatchdog::Options options;
    options.maxReports = 2;
    Rig rig(options);
    rig.watchdog->eventLoopWillBlock();
    rig.stall(100ms); // Written.
    rig.stall(100ms); // Within 10 s of the first: dropped.
    rig.run(10s);
    rig.stall(100ms); // Written: the second and last of the session.
    rig.run(10s);
    rig.stall(100ms); // Over the cap: dropped.
    const frost::diag::StallStats stats = rig.watchdog->stats();
    EXPECT_EQ(stats.stalls, 4u); // Stalls are always counted.
    EXPECT_EQ(stats.reportsWritten, 2u);
    EXPECT_EQ(stats.reportsDropped, 2u);
    EXPECT_EQ(rig.reports().size(), 2u);
}

TEST(StallWatchdog, StallsWhileDebuggedAreCountedButNotReported) {
    Rig rig;
    rig.debugger = true;
    rig.watchdog->eventLoopWillBlock();
    rig.stall(200ms);
    EXPECT_EQ(rig.watchdog->stats().stalls, 1u);
    EXPECT_EQ(rig.watchdog->stats().whileDebugged, 1u);
    EXPECT_EQ(rig.watchdog->stats().reportsWritten, 0u);
    EXPECT_TRUE(rig.reports().empty());
}

TEST(StallWatchdog, ALateWakeMarksTheStallUnreliable) {
    Rig rig;
    rig.watchdog->eventLoopWillBlock();
    rig.watchdog->eventLoopWoke();
    rig.watchdog->poll();
    rig.run(30ms);
    rig.clock.advance(500ms); // The watchdog thread did not run for 500 ms (system sleep, starvation).
    rig.watchdog->poll();
    rig.run(20ms);
    rig.watchdog->eventLoopWillBlock();
    rig.run(20ms);
    EXPECT_EQ(rig.watchdog->stats().unreliable, 1u);
    ASSERT_EQ(rig.reports().size(), 1u);
    EXPECT_TRUE(rig.fs.readFile(rig.reports().front())->contains("\"unreliable\": true"));
}

TEST(StallWatchdog, ScopedUiIdleCountsAsIdle) {
    Rig rig;
    rig.watchdog->eventLoopWillBlock();
    rig.watchdog->eventLoopWoke();
    rig.watchdog->poll();
    {
        const frost::diag::ScopedUiIdle idle(rig.watchdog.get()); // A native modal loop.
        rig.run(500ms);
    }
    StallWatchdog::install(rig.watchdog.get());
    {
        const frost::diag::ScopedUiIdle idle; // Through the installed watchdog.
        rig.run(500ms);
    }
    StallWatchdog::install(nullptr);
    {
        const frost::diag::ScopedUiIdle idle; // None installed: does nothing.
    }
    rig.run(10ms);
    rig.watchdog->eventLoopWillBlock();
    rig.run(20ms);
    EXPECT_EQ(rig.watchdog->stats().stalls, 0u);
    EXPECT_TRUE(rig.reports().empty());
}

TEST(StallWatchdog, CountAgreesWithTheWatchdogOnRandomTimelines) {
    // B21's count (UI thread, exact) and the watchdog's detections must agree when the watchdog
    // sees every change (here it polls after every hook).
    std::mt19937 random(20261101);
    StallWatchdog::Options options;
    options.maxReports = 0;
    Rig rig(options);
    rig.watchdog->eventLoopWillBlock();
    std::uint64_t expected = 0;
    for (int round = 0; round < 300; ++round) {
        rig.watchdog->eventLoopWoke();
        rig.watchdog->poll();
        const int events = static_cast<int>(random() % 4);
        for (int e = 0; e <= events; ++e) {
            const auto gap = std::chrono::milliseconds(random() % 120);
            rig.run(gap);
            expected += gap > 50ms ? 1 : 0;
            if (e < events) {
                rig.watchdog->eventStarted();
                rig.watchdog->poll();
            }
        }
        rig.watchdog->eventLoopWillBlock();
        rig.run(std::chrono::milliseconds(10 + random() % 50));
    }
    const frost::diag::StallStats stats = rig.watchdog->stats();
    EXPECT_EQ(stats.stalls, expected);
    EXPECT_EQ(stats.watchdogStalls, expected);
    EXPECT_EQ(stats.reportsDropped, expected);
}

TEST(StallWatchdog, StopIsBoundedEvenDuringAStall) {
    frost::io::MemFs fs(frost::systemClock());
    fs.addRoot(kWindows ? "C:\\" : "/");
    ASSERT_TRUE(fs.createDirectories(kFreezes).has_value());
    StallWatchdog::Options options;
    options.freezesDirectory = kFreezes;
    options.debuggerPresent = [] { return false; };
    auto watchdog =
        StallWatchdog::start(options, std::make_unique<frost::diag::FakeThreadSampler>(), fs, frost::systemClock());
    ASSERT_TRUE(watchdog.has_value()) << watchdog.error().describe();
    (*watchdog)->eventLoopWillBlock();
    (*watchdog)->eventLoopWoke();
    std::this_thread::sleep_for(100ms); // A stall in progress, being sampled.
    const auto begin = std::chrono::steady_clock::now();
    (*watchdog)->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 1s);
    (*watchdog)->stop(); // Idempotent.
}

TEST(StallWatchdog, StartRejectsBadOptions) {
    ManualClock clock;
    frost::io::MemFs fs(clock);
    StallWatchdog::Options options;
    options.threaded = false;
    EXPECT_FALSE(StallWatchdog::start(options, std::make_unique<frost::diag::FakeThreadSampler>(), fs, clock));
    options.freezesDirectory = kFreezes;
    EXPECT_FALSE(StallWatchdog::start(options, nullptr, fs, clock));
    options.pollPeriod = 0ms;
    EXPECT_FALSE(StallWatchdog::start(options, std::make_unique<frost::diag::FakeThreadSampler>(), fs, clock));
}

} // namespace
