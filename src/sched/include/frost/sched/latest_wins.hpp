// SPDX-License-Identifier: Apache-2.0
//
// LatestWins: "latest request wins" cancellation (PLAN.md section 2,
// "Scheduler": std::stop_token plus generation counters).
//
// Typical use, e.g. completion or hover requests that a newer keystroke supersedes:
//
//   LatestWins m_requests;
//   void onEdit() {
//       const LatestWins::Ticket ticket = m_requests.next(); // stops the previous request
//       scheduler.spawn(Lane::Interactive, compute(ticket.token), ticket.token).detach();
//   }
//   // ...when compute() has its result, inside the task:
//   m_requests.commitIfCurrent(ticket, [&] { publish(result); });  // stale results are dropped
//
// next() requests stop on the previous ticket's token, so a superseded task
// that has not started is skipped by spawn(), a sleeping one wakes up, and a
// running one sees stop_requested(). Because a task may finish between the stop
// and noticing it, results must still be published through commitIfCurrent(),
// which runs the commit only while the ticket is the latest, atomically with
// respect to next().
//
// Thread safety: all members are thread-safe.
#pragma once

#include "frost/core/mutex.hpp"

#include <atomic>
#include <cstdint>
#include <stop_token>
#include <utility>

namespace frost::sched {

class LatestWins {
public:
    struct Ticket {
        std::uint64_t generation = 0; // 1 for the first request; strictly increasing.
        std::stop_token token;        // Stopped when a newer request starts or cancel() is called.
    };

    LatestWins() = default;
    LatestWins(const LatestWins&) = delete ("tickets refer to one generation counter");
    LatestWins& operator=(const LatestWins&) = delete ("tickets refer to one generation counter");

    // Starts a new request: stops the previous ticket's token and returns a fresh
    // ticket. Stop callbacks of the old token run on the calling thread, outside
    // the internal lock. Allocates one stop state. O(1) plus those callbacks.
    [[nodiscard]] Ticket next();

    // Stops the current request without starting a new one (e.g. the view closed).
    // Later commits for every ticket issued so far are refused.
    void cancel();

    // True if `ticket` is still the latest and has not been cancelled. A hint:
    // it can change right after returning; use commitIfCurrent() to publish.
    [[nodiscard]] bool isCurrent(const Ticket& ticket) const noexcept;

    // The generation of the latest ticket (0 before the first next()).
    [[nodiscard]] std::uint64_t generation() const noexcept {
        // Acquire: pairs with the release store in next().
        return m_generation.load(std::memory_order_acquire);
    }

    // Runs commit() and returns true if `ticket` is the latest and not cancelled;
    // otherwise returns false without running it. Holds the internal lock while
    // commit() runs, so a concurrent next() waits; keep commits short and never
    // call back into this object from one.
    template<class Commit>
    bool commitIfCurrent(const Ticket& ticket, Commit&& commit) {
        LockGuard lock(m_mutex);
        if (ticket.generation != m_current || m_cancelled) {
            return false;
        }
        std::forward<Commit>(commit)();
        return true;
    }

private:
    Mutex m_mutex;
    std::stop_source m_source FROST_GUARDED_BY(m_mutex){std::nostopstate};
    std::uint64_t m_current FROST_GUARDED_BY(m_mutex) = 0;
    bool m_cancelled FROST_GUARDED_BY(m_mutex) = false;
    // Mirrors m_current for lock-free reads; written under m_mutex.
    std::atomic<std::uint64_t> m_generation{0};
};

} // namespace frost::sched
