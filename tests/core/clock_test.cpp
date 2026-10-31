// SPDX-License-Identifier: Apache-2.0
//
// Tests for the clock seam: SystemClock is monotonic, ManualClock moves only
// forward and only when advanced, and both are usable through IClock&.

#include "frost/core/clock.hpp"

#include "support/crash_dialogs.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <type_traits>

namespace {

using namespace std::chrono_literals;

// Reads time through the interface, the way production code does.
frost::IClock::SteadyTime readSteady(const frost::IClock& clock) { return clock.steadyNow(); }

TEST(Clock, SystemClockIsMonotonic) {
    const auto a = readSteady(frost::systemClock());
    const auto b = readSteady(frost::systemClock());
    EXPECT_LE(a, b);
}

TEST(Clock, ManualClockMovesOnlyWhenAdvanced) {
    frost::ManualClock clock;
    const auto start = readSteady(clock);
    EXPECT_EQ(readSteady(clock), start);
    clock.advance(250ms);
    EXPECT_EQ(readSteady(clock) - start, 250ms);
    EXPECT_EQ(clock.systemNow().time_since_epoch(), 250ms);
}

TEST(Clock, SettingSystemTimeLeavesSteadyTimeAlone) {
    frost::ManualClock clock;
    clock.advance(1s);
    clock.setSystemTime(frost::IClock::SystemTime(10s));
    EXPECT_EQ(clock.systemNow().time_since_epoch(), 10s);
    EXPECT_EQ(clock.steadyNow().time_since_epoch(), 1s);
}

TEST(ClockDeathTest, ManualClockRejectsNegativeAdvance) {
    // Steady time never goes backwards; advance() enforces that as a precondition.
    frost::ManualClock clock;
    EXPECT_DEATH(
        {
            frost::test::disableCrashDialogs();
            clock.advance(-1ns);
        },
        "precondition violated: delta >= Duration::zero");
}

TEST(Clock, ManualClockAcceptsZeroAdvance) {
    frost::ManualClock clock;
    clock.advance(0ns);
    EXPECT_EQ(clock.steadyNow().time_since_epoch(), 0ns);
}

TEST(Clock, ImplementationsAreTriviallyDestructible) {
    // Required so the shared SystemClock can be a constinit global (startup hygiene).
    static_assert(std::is_trivially_destructible_v<frost::SystemClock>);
}

} // namespace
