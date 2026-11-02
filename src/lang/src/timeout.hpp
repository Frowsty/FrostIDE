// SPDX-License-Identifier: Apache-2.0
//
// frost::lang internals: a deadline as a std::stop_source that a scheduler timer requests, so
// any token-taking coroutine (a request, a process exit wait) can be bounded in time and, when
// linked to a parent token, cancelled by it too.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <stop_token>

namespace frost::lang::detail {

/*
 * token() is stopped once `timeout` has passed on `scheduler`'s clock, or when `parent` is
 * stopped, unless disarmed first. expired() is true only for the timeout. The timer coroutine
 * holds only the scheduler and shared copies of its state, so the Deadline may be destroyed at
 * any time (destruction disarms it). Not thread-safe; one owner.
 */
class Deadline {
public:
    Deadline(sched::IScheduler& scheduler, IClock::Duration timeout, std::stop_token parent = {})
        : m_expired(std::make_shared<std::atomic<bool>>(false)) {
        if (parent.stop_possible()) {
            m_link.emplace(parent, Fire{m_fire});
        }
        scheduler.spawn(sched::Lane::UserVisible, timer(scheduler, timeout, m_fire, m_disarm.get_token(), m_expired))
            .detach();
    }
    ~Deadline() { disarm(); }
    Deadline(const Deadline&) = delete ("owns a running timer");
    Deadline& operator=(const Deadline&) = delete ("owns a running timer");

    [[nodiscard]] std::stop_token token() const noexcept { return m_fire.get_token(); }
    [[nodiscard]] bool expired() const noexcept { return m_expired->load(std::memory_order_acquire); }
    void disarm() noexcept { m_disarm.request_stop(); }

private:
    struct Fire {
        std::stop_source source;
        void operator()() noexcept { static_cast<void>(source.request_stop()); }
    };

    static sched::Task<void> timer(sched::IScheduler& scheduler, IClock::Duration timeout, std::stop_source fire,
                                   std::stop_token disarm, std::shared_ptr<std::atomic<bool>> expired) {
        if (co_await scheduler.sleepFor(timeout, disarm)) {
            expired->store(true, std::memory_order_release);
            fire.request_stop();
        }
    }

    std::stop_source m_fire;
    std::stop_source m_disarm;
    std::shared_ptr<std::atomic<bool>> m_expired;
    std::optional<std::stop_callback<Fire>> m_link;
};

} // namespace frost::lang::detail
