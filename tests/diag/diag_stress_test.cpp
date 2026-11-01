// SPDX-License-Identifier: Apache-2.0
//
// frost::diag's long concurrency runs (label "stress", skipped by the Debug legs of verify):
// the thread sampler against a target that churns the heap and the loader, which must never
// deadlock, and the stall watchdog's park/wake handshake on real threads, which must never
// lose a wake-up.

#include "frost/core/clock.hpp"
#include "frost/diag/module_identity.hpp"
#include "frost/diag/stall_watchdog.hpp"
#include "frost/diag/thread_sampler.hpp"
#include "frost/io/mem_fs.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

using namespace std::chrono_literals;

// Loops allocations and a library load/unload until told to stop.
void churn(const std::atomic<bool>& stop) {
    while (!stop.load(std::memory_order_acquire)) {
        void* block = std::malloc(4096 + static_cast<std::size_t>(std::rand() % 4096));
        std::free(block);
#ifdef _WIN32
        if (const HMODULE library = LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            FreeLibrary(library);
        }
#else
        if (void* library = ::dlopen("libm.so.6", RTLD_NOW | RTLD_LOCAL)) {
            ::dlclose(library);
        }
#endif
    }
}

TEST(ThreadSampler, NeverDeadlocksUnderHeapAndLoaderChurn) {
    // 5000 samples of a thread that holds the heap and loader locks much of the time: Windows
    // suspends it and copies its stack without taking any lock; Linux's signal handler unwinds
    // with libgcc's lock-free lookup. Either deadlock would hang this test (ctest's timeout).
    std::atomic<bool> stop{false};
    std::unique_ptr<frost::diag::IThreadSampler> sampler;
    std::atomic<bool> ready{false};
    std::thread target([&] {
        auto created = frost::diag::createSamplerForCurrentThread();
        if (created) {
            sampler = std::move(*created);
        }
        ready.store(true, std::memory_order_release);
        churn(stop);
    });
    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    ASSERT_NE(sampler, nullptr);
    std::size_t ok = 0;
    frost::diag::StackSample sample;
    for (int i = 0; i < 5000; ++i) {
        if (sampler->sample(sample)) {
            ++ok;
            // Module lookups take the loader lock, but only after the target was resumed.
            if (i % 100 == 0) {
                (void)frost::diag::moduleForAddress(sample.frames[0]);
            }
        }
    }
    stop = true;
    target.join();
    EXPECT_GT(ok, 4500u);
}

// A sampler that never touches a real thread.
class NullSampler final : public frost::diag::IThreadSampler {
public:
    frost::Result<void> sample(frost::diag::StackSample& out) noexcept override {
        out.count = 1;
        out.frames[0] = 0x1000;
        return {};
    }
    [[nodiscard]] std::uint64_t threadId() const noexcept override { return 1; }
};

TEST(StallWatchdog, ParkAndWakeNeverLoseAWakeUp) {
    /*
     * The watchdog parks after `parkAfter` of idleness and the UI side wakes it on the next
     * busy stretch (a seq_cst store-then-load on both sides). With a 1 ms park delay the
     * watchdog parks and wakes thousands of times here; every busy stretch longer than the
     * threshold must still be seen (watchdogStalls counts them), which a lost wake-up (the
     * watchdog asleep through a stall) would break.
     */
    frost::io::MemFs fs(frost::systemClock());
    constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;
    fs.addRoot(kWindows ? "C:\\" : "/");
    ASSERT_TRUE(fs.createDirectories(kWindows ? "C:\\f" : "/f").has_value());
    frost::diag::StallWatchdog::Options options;
    options.freezesDirectory = kWindows ? "C:\\f" : "/f";
    options.threshold = 5ms;
    options.samplingStart = 1ms;
    options.parkAfter = 1ms;
    options.pollPeriod = 1ms;
    options.lateWake = 1h; // Never unreliable: the machine may be loaded.
    options.maxReports = 0;
    options.debuggerPresent = [] { return false; };
    auto watchdog =
        frost::diag::StallWatchdog::start(options, std::make_unique<NullSampler>(), fs, frost::systemClock());
    ASSERT_TRUE(watchdog.has_value()) << watchdog.error().describe();
    (*watchdog)->eventLoopWillBlock(); // Arms.
    constexpr int kRounds = 200;
    for (int i = 0; i < kRounds; ++i) {
        std::this_thread::sleep_for(3ms); // Idle: the watchdog parks.
        (*watchdog)->eventLoopWoke();
        std::this_thread::sleep_for(12ms); // A stall well over the 5 ms threshold.
        (*watchdog)->eventLoopWillBlock();
    }
    std::this_thread::sleep_for(20ms);
    (*watchdog)->stop();
    const frost::diag::StallStats stats = (*watchdog)->stats();
    EXPECT_EQ(stats.stalls, static_cast<std::uint64_t>(kRounds));
    EXPECT_EQ(stats.watchdogStalls, static_cast<std::uint64_t>(kRounds));
    EXPECT_EQ(stats.reportsDropped, static_cast<std::uint64_t>(kRounds));
}

} // namespace
