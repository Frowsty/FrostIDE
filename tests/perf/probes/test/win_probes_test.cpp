// SPDX-License-Identifier: Apache-2.0
//
// Headless tests of the Windows probe primitives: the QPC time base, precise
// waits, thread CPU calibration, and SystemSessionState reading facts without
// failing. Nothing here opens a window or duplicates the desktop, so the tests
// run in any session (validity of the session is not asserted).

#include "frost/perf/win/screen_probes.hpp"
#include "frost/perf/win/system_session_state.hpp"
#include "frost/perf/win/timing.hpp"

#include <gtest/gtest.h>

#include <windows.h>

namespace {

using namespace frost::perf;
using namespace frost::perf::win;

TEST(WinProbes, QpcIsMonotonicWithAPositiveRate) {
    EXPECT_GT(qpcRate().ticksPerSecond(), 0);
    const auto a = qpcNow();
    const auto b = qpcNow();
    EXPECT_LE(a, b);
}

TEST(WinProbes, WaitUntilReachesTheDeadline) {
    const TickRate rate = qpcRate();
    const auto start = qpcNow();
    const auto deadline = start + rate.fromMs(5.0);
    waitUntil(deadline);
    const auto end = qpcNow();
    EXPECT_GE(end, deadline);
    // Generous upper bound: the machine may be busy with parallel builds.
    EXPECT_LT(rate.toMs(end - start), 200.0);
}

TEST(WinProbes, ThreadCycleMeterMeasuresSpinningAsCpuTime) {
    const ThreadCycleMeter meter;
    EXPECT_GT(meter.cyclesPerMs(), 1000.0); // Any x64 cycle counter ticks faster than 1 MHz.
    const TickRate rate = qpcRate();
    const auto cycles0 = meter.cyclesNow();
    const auto start = qpcNow();
    spinUntil(start + rate.fromMs(10.0));
    const double cpuMs = meter.toMs(meter.cyclesNow() - cycles0);
    // Spinning is all CPU; preemption can only lower it, calibration error is a few percent.
    EXPECT_GT(cpuMs, 1.0);
    EXPECT_LT(cpuMs, 12.0);
}

TEST(WinProbes, SystemSessionStateReadsFacts) {
    const SystemSessionState state;
    const auto facts = state.facts();
    ASSERT_TRUE(facts.has_value()) << facts.error().describe();
    DWORD session = 0;
    ASSERT_NE(ProcessIdToSessionId(GetCurrentProcessId(), &session), FALSE);
    EXPECT_EQ(facts->processSession, session);
    // The verdict depends on where the test runs (locked, RDP, ...), so it is only computed.
    const auto verdict = evaluateSession(*facts);
    EXPECT_EQ(verdict.valid, verdict.problems.empty());
}

TEST(WinProbes, DwmCountersAreReadableOrUnsupported) {
    const auto counters = readDwmCounters();
    if (!counters) {
        EXPECT_EQ(counters.error().code(), frost::ErrorCode::Unsupported);
        GTEST_SKIP() << "DWM timing unavailable in this session: " << counters.error().describe();
    }
    EXPECT_GT(counters->refreshPeriodTicks, 0);
    EXPECT_GT(counters->refreshCount, 0u);
}

TEST(WinProbes, ScreenRectGeometry) {
    const ScreenRect a{0, 0, 100, 50};
    const ScreenRect b{99, 49, 200, 60};
    const ScreenRect c{100, 0, 120, 10};
    EXPECT_TRUE(a.intersects(b));
    EXPECT_FALSE(a.intersects(c)); // Right edges are exclusive.
    EXPECT_TRUE(a.contains(ScreenRect{10, 10, 20, 20}));
    EXPECT_FALSE(a.contains(b));
    EXPECT_TRUE(ScreenRect{}.empty());
    EXPECT_EQ(a.width(), 100);
    EXPECT_EQ(a.height(), 50);
}

} // namespace
