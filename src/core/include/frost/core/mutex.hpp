// SPDX-License-Identifier: Apache-2.0
//
// Clang thread-safety annotations and annotated lock types (PLAN.md section 3.3).
// The MSVC STL and libstdc++ mutexes carry no capability attributes, so code that
// wants -Wthread-safety checking uses frost::Mutex and frost::LockGuard, and
// marks shared state with FROST_GUARDED_BY(m_mutex).
#pragma once

#include <mutex>

#if defined(__clang__)
#define FROST_THREAD_ANNOTATION(x) __attribute__((x))
#else
#define FROST_THREAD_ANNOTATION(x)
#endif

#define FROST_CAPABILITY(name) FROST_THREAD_ANNOTATION(capability(name))
#define FROST_SCOPED_CAPABILITY FROST_THREAD_ANNOTATION(scoped_lockable)
#define FROST_GUARDED_BY(x) FROST_THREAD_ANNOTATION(guarded_by(x))
#define FROST_PT_GUARDED_BY(x) FROST_THREAD_ANNOTATION(pt_guarded_by(x))
#define FROST_REQUIRES(...) FROST_THREAD_ANNOTATION(requires_capability(__VA_ARGS__))
#define FROST_EXCLUDES(...) FROST_THREAD_ANNOTATION(locks_excluded(__VA_ARGS__))
#define FROST_ACQUIRE(...) FROST_THREAD_ANNOTATION(acquire_capability(__VA_ARGS__))
#define FROST_RELEASE(...) FROST_THREAD_ANNOTATION(release_capability(__VA_ARGS__))
#define FROST_TRY_ACQUIRE(...) FROST_THREAD_ANNOTATION(try_acquire_capability(__VA_ARGS__))
#define FROST_RETURN_CAPABILITY(x) FROST_THREAD_ANNOTATION(lock_returned(x))
#define FROST_NO_THREAD_SAFETY_ANALYSIS FROST_THREAD_ANNOTATION(no_thread_safety_analysis)

namespace frost {

// A std::mutex with capability annotations. Not recursive; same cost as std::mutex.
class FROST_CAPABILITY("mutex") Mutex {
public:
    constexpr Mutex() noexcept = default;
    Mutex(const Mutex&) = delete ("a mutex guards one object and cannot be copied");
    Mutex& operator=(const Mutex&) = delete ("a mutex guards one object and cannot be copied");

    void lock() FROST_ACQUIRE() { m_mutex.lock(); }
    void unlock() FROST_RELEASE() { m_mutex.unlock(); }
    [[nodiscard]] bool try_lock() FROST_TRY_ACQUIRE(true) { return m_mutex.try_lock(); }

    // The underlying mutex, for std::condition_variable. Callers must hold the lock
    // already (e.g. through UniqueLock); the analysis does not see waits.
    [[nodiscard]] std::mutex& native() noexcept { return m_mutex; }

private:
    std::mutex m_mutex;
};

// RAII lock for frost::Mutex (the annotated std::lock_guard).
class FROST_SCOPED_CAPABILITY LockGuard {
public:
    explicit LockGuard(Mutex& mutex) FROST_ACQUIRE(mutex) : m_mutex(mutex) { m_mutex.lock(); }
    ~LockGuard() FROST_RELEASE() { m_mutex.unlock(); }
    LockGuard(const LockGuard&) = delete ("scoped locks are not transferable");
    LockGuard& operator=(const LockGuard&) = delete ("scoped locks are not transferable");

private:
    Mutex& m_mutex;
};

// RAII lock usable with std::condition_variable: wait(uniqueLock.native(), ...)
// atomically releases and reacquires the mutex, which the analysis treats as held.
class FROST_SCOPED_CAPABILITY UniqueLock {
public:
    explicit UniqueLock(Mutex& mutex) FROST_ACQUIRE(mutex) : m_lock(mutex.native()) {}
    ~UniqueLock() FROST_RELEASE() = default;
    UniqueLock(const UniqueLock&) = delete ("scoped locks are not transferable");
    UniqueLock& operator=(const UniqueLock&) = delete ("scoped locks are not transferable");

    [[nodiscard]] std::unique_lock<std::mutex>& native() noexcept { return m_lock; }

private:
    std::unique_lock<std::mutex> m_lock;
};

} // namespace frost
