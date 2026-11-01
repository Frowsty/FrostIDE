// SPDX-License-Identifier: Apache-2.0
//
// Private: the one retry schedule for SQLite locks, shared by the busy handler, the WAL switch
// and the VFS lock of quarantine and removal.
#pragma once

#include "frost/core/clock.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <thread>

namespace frost::storage::detail {

// sqliteDefaultBusyCallback's delays (SQLite main.c, public domain); 100 ms after the last.
// Units: milliseconds.
inline constexpr std::array<int, 12> kBusyDelaysMs{1, 2, 5, 10, 15, 20, 25, 25, 25, 50, 50, 100};

/*
 * Sleeps before retry number `attempt` (0 for the first retry): the schedule's delay, cut to the
 * time left before `deadline` on systemClock's steady clock. Returns false, without sleeping,
 * once the deadline has passed, so a caller's total wait never exceeds its deadline.
 * Thread-safe; blocks the calling thread.
 */
inline bool sleepBeforeRetry(std::size_t attempt, IClock::SteadyTime deadline) noexcept {
    const IClock::SteadyTime now = systemClock().steadyNow();
    if (now >= deadline) {
        return false;
    }
    const std::chrono::milliseconds planned(attempt < kBusyDelaysMs.size() ? kBusyDelaysMs[attempt] : 100);
    std::this_thread::sleep_for(std::min<IClock::Duration>(planned, deadline - now));
    return true;
}

} // namespace frost::storage::detail
