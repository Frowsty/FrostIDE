// SPDX-License-Identifier: Apache-2.0
//
// SerialQueue: frost::vcs's private worker, one thread that runs posted jobs in order, plus one
// delayed slot that coalesces (the debounced status refresh). Jobs get a stop token that cancel()
// and destruction stop.
#pragma once

#include "frost/core/mutex.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <optional>
#include <stop_token>
#include <thread>

namespace frost::vcs::detail {

class SerialQueue {
public:
    using Job = std::function<void(std::stop_token)>;
    using Clock = std::chrono::steady_clock;

    SerialQueue();
    // Stops the running job's token, drops what is pending and joins the thread.
    ~SerialQueue();
    SerialQueue(const SerialQueue&) = delete ("owns a thread");
    SerialQueue& operator=(const SerialQueue&) = delete ("owns a thread");

    // Runs `job` after everything posted before it.
    void post(Job job);
    /*
     * Arms the delayed slot: `job` runs once at `due`, after the posted jobs that are due. Arming
     * it again before it ran replaces the job and moves the time to min(old due, `latest`) and
     * max(that, `due`): later calls push it back (debounce) but never past `latest`.
     */
    void schedule(Clock::time_point due, Clock::time_point latest, Job job);
    // Stops the token of the job that is running now (not the pending ones).
    void cancelRunning();
    // True while a job runs, is posted, or the delayed slot is armed.
    [[nodiscard]] bool busy() const;
    // Waits until busy() is false or `timeout` passes; returns !busy().
    [[nodiscard]] bool waitIdle(std::chrono::milliseconds timeout) const;

private:
    void run(std::stop_token stop);

    mutable Mutex m_mutex;
    mutable std::condition_variable m_changed;
    std::deque<Job> m_jobs FROST_GUARDED_BY(m_mutex);
    std::optional<Job> m_delayed FROST_GUARDED_BY(m_mutex);
    Clock::time_point m_due FROST_GUARDED_BY(m_mutex){};
    Clock::time_point m_latest FROST_GUARDED_BY(m_mutex){};
    std::optional<std::stop_source> m_running FROST_GUARDED_BY(m_mutex);
    bool m_stopping FROST_GUARDED_BY(m_mutex) = false;
    std::jthread m_thread; // Last: starts after the members above exist.
};

} // namespace frost::vcs::detail
