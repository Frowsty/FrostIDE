// SPDX-License-Identifier: Apache-2.0
//
// IClock: the time source seam (PLAN.md section 6.2). Production code reads
// time only through an IClock so tests can substitute ManualClock (and the
// scheduler's SimExecutor its virtual clock) for deterministic timing.
//
// Interfaces in FrostIDE are passed by reference and never deleted through a
// base pointer, so IClock has a protected non-virtual destructor. That keeps
// implementations trivially destructible, which lets SystemClock live in a
// constinit global without an exit-time destructor (startup hygiene, PLAN.md 2.1).
#pragma once

#include <atomic>
#include <chrono>

namespace frost {

class IClock {
public:
    using Duration = std::chrono::nanoseconds;
    // Monotonic time for timeouts, budgets and scheduling; never jumps backwards.
    using SteadyTime = std::chrono::time_point<std::chrono::steady_clock, Duration>;
    // Wall-clock time for display and file timestamps; may jump.
    using SystemTime = std::chrono::time_point<std::chrono::system_clock, Duration>;

    [[nodiscard]] virtual SteadyTime steadyNow() const noexcept = 0;
    [[nodiscard]] virtual SystemTime systemNow() const noexcept = 0;

protected:
    constexpr IClock() noexcept = default;
    constexpr IClock(const IClock&) noexcept = default;
    constexpr IClock& operator=(const IClock&) noexcept = default;
    ~IClock() = default;
};

// The real clocks. Stateless and thread-safe; use systemClock() for the shared instance.
class SystemClock final : public IClock {
public:
    constexpr SystemClock() noexcept = default;
    [[nodiscard]] SteadyTime steadyNow() const noexcept override {
        return std::chrono::time_point_cast<Duration>(std::chrono::steady_clock::now());
    }
    [[nodiscard]] SystemTime systemNow() const noexcept override {
        return std::chrono::time_point_cast<Duration>(std::chrono::system_clock::now());
    }
};

// The process-wide SystemClock instance.
[[nodiscard]] IClock& systemClock() noexcept;

// A clock that moves only when told to. Thread-safe: reads and advances are
// atomic, so a test thread can advance time while workers read it.
class ManualClock final : public IClock {
public:
    explicit ManualClock(SteadyTime steadyStart = SteadyTime{}, SystemTime systemStart = SystemTime{}) noexcept
        : m_steady(steadyStart.time_since_epoch().count()), m_system(systemStart.time_since_epoch().count()) {}

    [[nodiscard]] SteadyTime steadyNow() const noexcept override {
        return SteadyTime(Duration(m_steady.load(std::memory_order_acquire)));
    }
    [[nodiscard]] SystemTime systemNow() const noexcept override {
        return SystemTime(Duration(m_system.load(std::memory_order_acquire)));
    }

    // Moves both clocks forward by `delta` (which must not be negative).
    void advance(Duration delta) noexcept {
        m_steady.fetch_add(delta.count(), std::memory_order_acq_rel);
        m_system.fetch_add(delta.count(), std::memory_order_acq_rel);
    }

    // Sets the wall clock only, e.g. to simulate a clock change; steady time is unaffected.
    void setSystemTime(SystemTime time) noexcept {
        m_system.store(time.time_since_epoch().count(), std::memory_order_release);
    }

private:
    std::atomic<Duration::rep> m_steady;
    std::atomic<Duration::rep> m_system;
};

} // namespace frost
