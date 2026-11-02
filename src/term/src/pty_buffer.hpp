// SPDX-License-Identifier: Apache-2.0
//
// OutputBuffer (frost::term internals): the queue between a pseudo terminal's dedicated reader thread
// and IPty::read(), shared by the ConPTY and posix_openpt implementations (pty_win.cpp, pty_posix.cpp).
#pragma once

#include "frost/core/mutex.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace frost::term::detail {

/*
 * Bytes read from the terminal and not yet taken. The reader thread appends and blocks while more than
 * kHighWater bytes wait (backpressure, so a flood of output cannot grow memory without bound); finish()
 * marks the end of output; abandon() (close) drops the backpressure, so the reader drains the terminal
 * to its end. Thread-safe: one producer and one consumer.
 */
class OutputBuffer {
public:
    static constexpr std::size_t kHighWater = 1024 * 1024;

    // Reader thread. Returns false once abandon() ran (the bytes are dropped then).
    bool append(std::string_view bytes) {
        UniqueLock lock(m_mutex);
        while (!m_abandoned && m_data.size() - m_head > kHighWater) {
            m_changed.wait(lock.native());
        }
        if (m_abandoned) {
            return false;
        }
        // A consumer can leave a small backlog forever. Reclaim the consumed prefix
        // periodically even when the queue never becomes empty, bounding retained bytes.
        if (m_head >= 64 * 1024 && m_head >= m_data.size() / 2) {
            m_data.erase(0, m_head);
            m_head = 0;
        }
        m_data.append(bytes);
        m_changed.notify_all();
        return true;
    }

    // Reader thread: no more output.
    void finish() {
        LockGuard lock(m_mutex);
        m_finished = true;
        m_changed.notify_all();
    }

    // Blocks until bytes are queued or the output ended; returns 0 at the end.
    std::size_t take(std::span<char> out) {
        UniqueLock lock(m_mutex);
        while (m_head == m_data.size() && !m_finished) {
            m_changed.wait(lock.native());
        }
        const std::size_t count = std::min(out.size(), m_data.size() - m_head);
        std::copy_n(m_data.data() + m_head, count, out.data());
        m_head += count;
        if (m_head == m_data.size()) {
            m_data.clear();
            m_head = 0;
        }
        m_changed.notify_all();
        return count;
    }

    // Allocated storage including the consumed prefix (for the bounded-storage regression test).
    [[nodiscard]] std::size_t retainedCapacityForTesting() {
        LockGuard lock(m_mutex);
        return m_data.capacity();
    }

    // Closing: the reader stops waiting for space and drops what it reads from now on.
    void abandon() {
        LockGuard lock(m_mutex);
        m_abandoned = true;
        m_changed.notify_all();
    }

private:
    Mutex m_mutex;
    std::condition_variable m_changed;
    std::string m_data FROST_GUARDED_BY(m_mutex);
    std::size_t m_head FROST_GUARDED_BY(m_mutex) = 0;
    bool m_finished FROST_GUARDED_BY(m_mutex) = false;
    bool m_abandoned FROST_GUARDED_BY(m_mutex) = false;
};

} // namespace frost::term::detail
