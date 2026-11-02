// SPDX-License-Identifier: Apache-2.0
// Tests for the isolated FrostEngine automation helper process.
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/engine_discovery_process.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/sched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace {
using namespace std::chrono_literals;
#ifdef _WIN32
constexpr auto kExe = "C:/tools/frost.exe";
#else
constexpr auto kExe = "/tools/frost";
#endif
TEST(EngineDiscoveryProcess, StalledHelperTimesOutAndIsContained) {
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor);
    frost::sched::SimExecutor scheduler;
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect(frost::io::ScriptedProcess{.neverExits = true});
    auto pending = scheduler.spawn(
        frost::sched::Lane::Background,
        frost::lang::requestEngineInHelper(launcher, {**reactor, scheduler}, kExe, frost::lang::Json::object(), 1s));
    scheduler.runFor(2s);
    auto result = pending.join();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Timeout);
    const auto calls = launcher.launches();
    ASSERT_EQ(calls.size(), 1U);
    EXPECT_TRUE(calls[0].spec.killOnClose);
    EXPECT_TRUE(calls[0].terminated);
    EXPECT_EQ(calls[0].spec.arguments[0], "--internal-engine-request");
}
TEST(EngineDiscoveryProcess, CancellationStopsDelayedHelper) {
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor);
    frost::sched::SimExecutor scheduler;
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect(frost::io::ScriptedProcess{.neverExits = true});
    std::stop_source stop;
    auto pending =
        scheduler.spawn(frost::sched::Lane::Background,
                        frost::lang::requestEngineInHelper(launcher, {**reactor, scheduler}, kExe,
                                                           frost::lang::Json::object(), 30s, stop.get_token()));
    scheduler.runFor(1ms);
    stop.request_stop();
    scheduler.runFor(10ms);
    auto result = pending.join();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Cancelled);
}
TEST(EngineDiscoveryProcess, RealStalledChildDoesNotBlockCancellation) {
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor);
    frost::sched::ThreadPool scheduler({.workerCount = 2});
    auto executable = frost::io::normalizePath(FROST_DISCOVERY_STALL_CHILD);
    ASSERT_TRUE(executable);
    std::stop_source stop;
    const auto started = std::chrono::steady_clock::now();
    auto pending = scheduler.spawn(
        frost::sched::Lane::Background,
        frost::lang::requestEngineInHelper(frost::io::realProcessLauncher(), {**reactor, scheduler}, *executable,
                                           frost::lang::Json::object(), 30s, stop.get_token()));
    std::this_thread::sleep_for(100ms);
    stop.request_stop();
    auto result = pending.join();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Cancelled);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 4s);
}
} // namespace
