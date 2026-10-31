// SPDX-License-Identifier: Apache-2.0
//
// Windows timing primitives for the perf harnesses: the QueryPerformanceCounter
// time base (the clock DXGI and DWM timestamps use), precise waits, per-thread
// CPU time from QueryThreadCycleTime, and the display-awake guard that PLAN.md
// section 6.3 requires for on-screen runs. Windows only; no Qt.
#pragma once

#include "frost/perf/frame_analysis.hpp"

#include <cstdint>

namespace frost::perf::win {

// QueryPerformanceCounter now. Thread-safe, wait-free.
[[nodiscard]] std::int64_t qpcNow() noexcept;

// The QPC frequency (fixed at boot).
[[nodiscard]] TickRate qpcRate() noexcept;

// Busy-waits until qpcNow() >= deadline: exact to about a microsecond, burns a core.
void spinUntil(std::int64_t deadlineTicks) noexcept;

// Waits until qpcNow() >= deadline: sleeps on a high-resolution waitable timer
// until about 1 ms before the deadline, then spins. For delays up to seconds.
void waitUntil(std::int64_t deadlineTicks) noexcept;

// Measures the calling thread's CPU time in cycles (QueryThreadCycleTime) and
// converts it to milliseconds with a factor calibrated against QPC by spinning
// (so it is right whatever the cycle counter's unit is). Only meaningful on the
// thread that created it; each call reads the current thread.
class ThreadCycleMeter {
public:
    // Calibrates by spinning about 60 ms in three rounds and keeping the highest rate
    // (preemption during a round can only lower it). Call on the measured thread.
    ThreadCycleMeter() noexcept;

    [[nodiscard]] std::uint64_t cyclesNow() const noexcept;
    [[nodiscard]] double toMs(std::uint64_t cycles) const noexcept;
    [[nodiscard]] double cyclesPerMs() const noexcept { return m_cyclesPerMs; }

private:
    double m_cyclesPerMs = 1.0;
};

// Keeps the display and system awake while alive (SetThreadExecutionState with
// ES_DISPLAY_REQUIRED). Not copyable; restores the default when destroyed. The
// execution state is per thread, so destroy it on the thread that created it.
class DisplayRequiredGuard {
public:
    DisplayRequiredGuard() noexcept;
    ~DisplayRequiredGuard();
    DisplayRequiredGuard(const DisplayRequiredGuard&) = delete ("restores the execution state exactly once");
    DisplayRequiredGuard& operator=(const DisplayRequiredGuard&) = delete ("restores the execution state exactly once");
};

} // namespace frost::perf::win
