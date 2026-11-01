// SPDX-License-Identifier: Apache-2.0
//
// Implementation of the Windows timing primitives (see frost/perf/win/timing.hpp).

#include "frost/perf/win/timing.hpp"

#include <algorithm>

#include <windows.h>

namespace frost::perf::win {

std::int64_t qpcNow() noexcept {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

TickRate qpcRate() noexcept {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return TickRate(frequency.QuadPart);
}

void spinUntil(std::int64_t deadlineTicks) noexcept {
    while (qpcNow() < deadlineTicks) {
        YieldProcessor();
    }
}

void waitUntil(std::int64_t deadlineTicks) noexcept {
    const TickRate rate = qpcRate();
    const std::int64_t spinTicks = rate.fromMs(1.0);
    const std::int64_t remaining = deadlineTicks - qpcNow();
    if (remaining > spinTicks) {
        // High-resolution timers (Windows 10 1803+) wake within about 0.5 ms without
        // changing the global timer resolution the way timeBeginPeriod does.
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (timer != nullptr) {
            const double sleepMs = rate.toMs(remaining - spinTicks);
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(sleepMs * 10'000.0); // Relative, in 100 ns units.
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE) {
                WaitForSingleObject(timer, INFINITE);
            }
            CloseHandle(timer);
        }
    }
    spinUntil(deadlineTicks);
}

ThreadCycleMeter::ThreadCycleMeter() noexcept {
    const TickRate rate = qpcRate();
    double best = 0.0;
    for (int round = 0; round < 3; ++round) {
        const std::uint64_t cycles0 = cyclesNow();
        const std::int64_t start = qpcNow();
        spinUntil(start + rate.fromMs(20.0));
        const std::uint64_t cycles1 = cyclesNow();
        const double elapsedMs = rate.toMs(qpcNow() - start);
        if (elapsedMs > 0.0) {
            best = std::max(best, static_cast<double>(cycles1 - cycles0) / elapsedMs);
        }
    }
    if (best > 0.0) {
        m_cyclesPerMs = best;
    }
}

std::uint64_t ThreadCycleMeter::cyclesNow() const noexcept {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
}

double ThreadCycleMeter::toMs(std::uint64_t cycles) const noexcept {
    return static_cast<double>(cycles) / m_cyclesPerMs;
}

DisplayRequiredGuard::DisplayRequiredGuard() noexcept {
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
}

DisplayRequiredGuard::~DisplayRequiredGuard() { SetThreadExecutionState(ES_CONTINUOUS); }

} // namespace frost::perf::win
