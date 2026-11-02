// SPDX-License-Identifier: Apache-2.0
//
// WorkBudget: the work cap shared by every phase of one diff, merge or refinement (ADR 0019, D7).
// A phase charges units as it goes; once the cap is reached (or a stop is seen) every later charge
// fails, and the phase finishes with a coarse but valid result. Budget: B21.
#pragma once

#include <cstdint>
#include <limits>
#include <stop_token>

namespace frost::diff::detail {

// How often the stop token is polled, in work units.
inline constexpr std::uint64_t kStopPollUnits = 4096;

/*
 * A counter of work units with a cap and a stop token. charge() succeeds while the total stays
 * within the cap; the first charge that would exceed it, or a poll that sees a stop request, makes
 * the budget exhausted for good (and, for a stop, cancelled). Deterministic: for a given cap the
 * same sequence of charges fails at the same point. Not thread-safe (one per call). O(1) per call.
 */
class WorkBudget {
public:
    WorkBudget(std::uint64_t maxWork, std::stop_token stop) noexcept
        : m_max(maxWork), m_nextPoll(kStopPollUnits), m_stop(std::move(stop)) {}

    // Charges `units`. Returns false when that would exceed the cap or a stop was requested; the
    // budget is then exhausted and every later call returns false.
    [[nodiscard]] bool charge(std::uint64_t units = 1) noexcept {
        if (m_exhausted) {
            return false;
        }
        if (units > m_max - m_used) {
            m_used = m_max;
            m_exhausted = true;
            return false;
        }
        m_used += units;
        if (m_used >= m_nextPoll) {
            m_nextPoll = m_used > std::numeric_limits<std::uint64_t>::max() - kStopPollUnits
                             ? std::numeric_limits<std::uint64_t>::max()
                             : m_used + kStopPollUnits;
            if (m_stop.stop_requested()) {
                m_cancelled = true;
                m_exhausted = true;
                return false;
            }
        }
        return true;
    }

    // True once a charge failed (cap reached or stop seen).
    [[nodiscard]] bool exhausted() const noexcept { return m_exhausted; }
    // True when a stop request was seen; the caller reports Cancelled.
    [[nodiscard]] bool cancelled() const noexcept { return m_cancelled; }
    // Units charged so far (the cap once exhausted by the cap).
    [[nodiscard]] std::uint64_t used() const noexcept { return m_used; }
    // Units left before the cap.
    [[nodiscard]] std::uint64_t remaining() const noexcept { return m_max - m_used; }

private:
    std::uint64_t m_max;
    std::uint64_t m_used = 0;
    std::uint64_t m_nextPoll;
    std::stop_token m_stop;
    bool m_exhausted = false;
    bool m_cancelled = false;
};

} // namespace frost::diff::detail
