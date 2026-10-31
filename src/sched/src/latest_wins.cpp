// SPDX-License-Identifier: Apache-2.0
//
// LatestWins: generation counter plus stop source (see frost/sched/latest_wins.hpp).

#include "frost/sched/latest_wins.hpp"

namespace frost::sched {

LatestWins::Ticket LatestWins::next() {
    std::stop_source fresh;
    std::stop_source previous{std::nostopstate};
    Ticket ticket;
    {
        LockGuard lock(m_mutex);
        previous = std::exchange(m_source, fresh);
        ticket.generation = ++m_current;
        m_cancelled = false;
        // Release: publishes the new generation to isCurrent()/generation() readers.
        m_generation.store(ticket.generation, std::memory_order_release);
    }
    ticket.token = fresh.get_token();
    // Outside the lock: stop callbacks may take other locks (the scheduler's timer lock).
    if (previous.stop_possible()) {
        previous.request_stop();
    }
    return ticket;
}

void LatestWins::cancel() {
    std::stop_source current{std::nostopstate};
    {
        LockGuard lock(m_mutex);
        current = m_source;
        m_cancelled = true;
    }
    if (current.stop_possible()) {
        current.request_stop();
    }
}

bool LatestWins::isCurrent(const Ticket& ticket) const noexcept {
    // Acquire: pairs with the release store in next(). A stopped token means cancel() or a newer request.
    return ticket.generation == m_generation.load(std::memory_order_acquire) && !ticket.token.stop_requested();
}

} // namespace frost::sched
