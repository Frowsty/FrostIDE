// SPDX-License-Identifier: Apache-2.0
//
// Tests for the annotated lock types: mutual exclusion, try_lock and waiting
// on a condition variable through UniqueLock. Compiling this file under
// -Wthread-safety -Werror also checks that the annotations are well-formed.

#include "frost/core/mutex.hpp"

#include <gtest/gtest.h>

#include <condition_variable>
#include <thread>
#include <vector>

namespace {

// A counter whose value is guarded by its mutex; the analysis rejects unguarded access.
class Counter {
public:
    void increment() {
        frost::LockGuard lock(m_mutex);
        ++m_value;
    }
    [[nodiscard]] int value() {
        frost::LockGuard lock(m_mutex);
        return m_value;
    }

private:
    frost::Mutex m_mutex;
    int m_value FROST_GUARDED_BY(m_mutex) = 0;
};

TEST(Mutex, LockGuardSerialisesIncrements) {
    Counter counter;
    std::vector<std::jthread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&counter] {
            for (int i = 0; i < 10'000; ++i) {
                counter.increment();
            }
        });
    }
    threads.clear(); // joins
    EXPECT_EQ(counter.value(), 40'000);
}

TEST(Mutex, TryLockFailsWhileHeld) {
    frost::Mutex mutex;
    frost::LockGuard lock(mutex);
    bool acquired = true;
    std::jthread other([&] {
        // The analysis follows try_lock only when it is the branch condition.
        if (mutex.try_lock()) {
            mutex.unlock();
        } else {
            acquired = false;
        }
    });
    other.join();
    EXPECT_FALSE(acquired);
}

TEST(Mutex, UniqueLockWorksWithConditionVariable) {
    frost::Mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    std::jthread producer([&] {
        frost::LockGuard lock(mutex);
        ready = true;
        cv.notify_one();
    });
    frost::UniqueLock lock(mutex);
    cv.wait(lock.native(), [&] { return ready; });
    EXPECT_TRUE(ready);
}

} // namespace
