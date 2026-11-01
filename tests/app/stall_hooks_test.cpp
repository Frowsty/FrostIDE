// SPDX-License-Identifier: Apache-2.0
//
// StallHooks (frost/app/diagnostics_qt.hpp) on the offscreen platform with a real event loop
// and a real watchdog thread: a 120 ms slot is exactly one stall and one freeze report, a
// 30 ms slot none; an idle QEventLoop and an idle modal QDialog::exec (a nested loop) are not
// stalls; and on Windows the native filter turns WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE into an
// idle section. Reports go to MemFs; the sampler is a FakeThreadSampler.

#include "frost/app/diagnostics_qt.hpp"
#include "frost/core/clock.hpp"
#include "frost/diag/stall_watchdog.hpp"
#include "frost/diag/thread_sampler.hpp"
#include "frost/io/mem_fs.hpp"

#include <QApplication>
#include <QDialog>
#include <QEventLoop>
#include <QTimer>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using namespace std::chrono_literals;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;
const std::string kFreezes = kWindows ? "C:\\cache\\crashes\\freezes" : "/cache/crashes/freezes";

// Busy for `duration` on the calling thread (no event processing).
void spin(std::chrono::milliseconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
    }
}

struct HookRig {
    frost::io::MemFs fs{frost::systemClock()};
    std::unique_ptr<frost::diag::StallWatchdog> watchdog;
    std::unique_ptr<frost::app::StallHooks> hooks;

    HookRig() {
        fs.addRoot(kWindows ? "C:\\" : "/");
        EXPECT_TRUE(fs.createDirectories(kFreezes).has_value());
        frost::diag::StallWatchdog::Options options;
        options.freezesDirectory = kFreezes;
        options.debuggerPresent = [] { return false; };
        auto started = frost::diag::StallWatchdog::start(
            std::move(options), std::make_unique<frost::diag::FakeThreadSampler>(), fs, frost::systemClock());
        EXPECT_TRUE(started.has_value());
        watchdog = std::move(*started);
        hooks = std::make_unique<frost::app::StallHooks>(*QCoreApplication::instance(), *watchdog);
    }
    ~HookRig() {
        hooks.reset();
        watchdog->stop();
    }

    // Runs the event loop for `duration` with `work` on a 20 ms timer: by then the loop has
    // blocked once, which arms the watchdog (nothing counts before the first aboutToBlock).
    void runLoop(std::chrono::milliseconds duration, std::function<void()> work) {
        QEventLoop loop;
        QTimer::singleShot(20, &loop, std::move(work));
        QTimer::singleShot(static_cast<int>(duration.count()), &loop, &QEventLoop::quit);
        loop.exec();
        // The watchdog notices the end of the last stretch at its next poll.
        std::this_thread::sleep_for(50ms);
    }

    std::size_t reports() const {
        std::size_t count = 0;
        for (const std::string& path : fs.allPaths()) {
            count += path.ends_with(".json") ? 1 : 0;
        }
        return count;
    }
};

TEST(StallHooks, ASlowSlotIsOneStallAndOneReport) {
    HookRig rig;
    rig.runLoop(400ms, [] { spin(120ms); });
    const frost::diag::StallStats stats = rig.watchdog->stats();
    EXPECT_EQ(stats.stalls, 1u);
    EXPECT_EQ(stats.watchdogStalls, 1u);
    EXPECT_EQ(stats.reportsWritten, 1u);
    EXPECT_EQ(rig.reports(), 1u);
    EXPECT_GE(stats.longest, 120ms);
}

TEST(StallHooks, AShortSlotIsNoStall) {
    HookRig rig;
    rig.runLoop(200ms, [] { spin(30ms); });
    EXPECT_EQ(rig.watchdog->stats().stalls, 0u);
    EXPECT_EQ(rig.reports(), 0u);
}

TEST(StallHooks, AnIdleEventLoopIsNoStall) {
    HookRig rig;
    rig.runLoop(200ms, [] {});
    EXPECT_EQ(rig.watchdog->stats().stalls, 0u);
    EXPECT_EQ(rig.reports(), 0u);
}

TEST(StallHooks, AnIdleModalDialogIsNoStall) {
    HookRig rig;
    rig.runLoop(500ms, [] {
        QDialog dialog;
        QTimer::singleShot(300, &dialog, &QDialog::accept);
        dialog.exec(); // A nested loop, idle for 300 ms inside this slot.
    });
    EXPECT_EQ(rig.watchdog->stats().stalls, 0u);
    EXPECT_EQ(rig.reports(), 0u);
}

#ifdef _WIN32
TEST(StallHooks, SizeMoveLoopIsIdle) {
    HookRig rig;
    rig.watchdog->eventLoopWillBlock(); // Armed.
    rig.watchdog->eventLoopWoke();
    EXPECT_GT(rig.watchdog->detector().progressNs(), 0);
    MSG message{};
    message.message = WM_ENTERSIZEMOVE;
    qintptr result = 0;
    EXPECT_FALSE(rig.hooks->nativeEventFilter("windows_generic_MSG", &message, &result));
    EXPECT_LT(rig.watchdog->detector().progressNs(), 0) << "inside the size/move loop: idle";
    EXPECT_FALSE(rig.hooks->nativeEventFilter("windows_generic_MSG", &message, &result)); // Repeated: one section.
    message.message = WM_EXITSIZEMOVE;
    EXPECT_FALSE(rig.hooks->nativeEventFilter("windows_generic_MSG", &message, &result));
    EXPECT_GT(rig.watchdog->detector().progressNs(), 0) << "busy again after the loop";
    // Other event types are ignored.
    message.message = WM_ENTERSIZEMOVE;
    EXPECT_FALSE(rig.hooks->nativeEventFilter("windows_dispatcher_MSG", &message, &result));
    EXPECT_GT(rig.watchdog->detector().progressNs(), 0);
    rig.watchdog->eventLoopWillBlock();
}
#endif

} // namespace
