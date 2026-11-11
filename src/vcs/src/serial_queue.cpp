// SPDX-License-Identifier: Apache-2.0
//
// SerialQueue (serial_queue.hpp): the worker loop, the delayed slot and the idle wait.

#include "serial_queue.hpp"

#include <algorithm>

namespace frost::vcs::detail {

SerialQueue::SerialQueue() : m_thread([this](const std::stop_token& stop) { run(stop); }) {}

SerialQueue::~SerialQueue() {
    {
        LockGuard lock(m_mutex);
        m_stopping = true;
        m_jobs.clear();
        m_delayed.reset();
        if (m_running) {
            m_running->request_stop();
        }
    }
    m_changed.notify_all();
    m_thread.request_stop();
    m_thread.join();
}

void SerialQueue::post(Job job) {
    {
        LockGuard lock(m_mutex);
        if (m_stopping) {
            return;
        }
        m_jobs.push_back(std::move(job));
    }
    m_changed.notify_all();
}

void SerialQueue::schedule(Clock::time_point due, Clock::time_point latest, Job job) {
    {
        LockGuard lock(m_mutex);
        if (m_stopping) {
            return;
        }
        if (m_delayed) {
            m_latest = std::min(m_latest, latest);
            m_due = std::min(std::max(m_due, due), m_latest);
        } else {
            m_latest = latest;
            m_due = std::min(due, latest);
        }
        m_delayed = std::move(job);
    }
    m_changed.notify_all();
}

void SerialQueue::cancelRunning() {
    LockGuard lock(m_mutex);
    if (m_running) {
        m_running->request_stop();
    } else if (!m_jobs.empty()) {
        m_cancelNext = true;
    }
}

bool SerialQueue::busy() const {
    LockGuard lock(m_mutex);
    return !m_jobs.empty() || m_delayed.has_value() || m_running.has_value();
}

bool SerialQueue::waitIdle(std::chrono::milliseconds timeout) const {
    UniqueLock lock(m_mutex);
    return m_changed.wait_for(lock.native(), timeout, [this]() FROST_NO_THREAD_SAFETY_ANALYSIS {
        return m_jobs.empty() && !m_delayed.has_value() && !m_running.has_value();
    });
}

void SerialQueue::run(const std::stop_token& stop) {
    while (true) {
        Job job;
        std::stop_token token;
        {
            UniqueLock lock(m_mutex);
            while (true) {
                if (m_stopping || stop.stop_requested()) {
                    return;
                }
                if (!m_jobs.empty()) {
                    job = std::move(m_jobs.front());
                    m_jobs.pop_front();
                    break;
                }
                if (m_delayed && Clock::now() >= m_due) {
                    job = std::move(*m_delayed);
                    m_delayed.reset();
                    break;
                }
                if (m_delayed) {
                    m_changed.wait_until(lock.native(), m_due);
                } else {
                    m_changed.wait(lock.native());
                }
            }
            m_running.emplace();
            if (m_cancelNext) {
                m_running->request_stop();
                m_cancelNext = false;
            }
            token = m_running->get_token();
        }
        job(token);
        {
            LockGuard lock(m_mutex);
            m_running.reset();
        }
        m_changed.notify_all();
    }
}

} // namespace frost::vcs::detail
