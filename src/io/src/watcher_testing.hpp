// SPDX-License-Identifier: Apache-2.0
//
// Test hooks of the reactor and the real file watchers (frost::io internals; ADR 0020). Asio-free, so
// the io test binaries include it from src/io/src. Every hook is process-wide, thread-safe (relaxed
// atomics: a test sets them before it starts the watch it targets) and inert unless a test sets it.
#pragma once

#include "frost/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace frost::io {

class Reactor;

namespace testing {

/*
 * Binds a shared state that owns no OS resource to `reactor` (as watch states and pipes bind theirs),
 * for the lifecycle tests: it counts in stats().boundObjects until the last reference drops, and its
 * shutdown on the strand only records that it ran (shutdownRan). Errors: Cancelled once shutdown has
 * begun. Any thread.
 */
class BoundProbe {
public:
    [[nodiscard]] static Result<std::shared_ptr<BoundProbe>> bind(Reactor& reactor);
    virtual ~BoundProbe() = default;
    // True once the reactor's shutdown ran this state's shutdownOnStrand.
    [[nodiscard]] virtual bool shutdownRan() const noexcept = 0;
    // Posts `fn` to the state's strand; false (nothing posted) once shutdown has begun.
    [[nodiscard]] virtual bool post(std::function<void()> fn) = 0;

protected:
    BoundProbe() = default;
    BoundProbe(const BoundProbe&) = default;
    BoundProbe& operator=(const BoundProbe&) = default;
};

// Runs `fn` on one of `reactor`'s threads and waits for it. Precondition: the reactor is running.
void runOnReactorThread(Reactor& reactor, const std::function<void()>& fn);

/*
 * Blocks every thread of `reactor` on a gate until release() or destruction (RAII), so tests can let
 * kernel queues overflow. The constructor returns once all threads are blocked. Tests use a one-thread
 * reactor. Precondition: the reactor is running and not shutting down.
 */
class ReactorStall {
public:
    explicit ReactorStall(Reactor& reactor);
    ~ReactorStall();
    ReactorStall(const ReactorStall&) = delete ("a stall is released once");
    ReactorStall& operator=(const ReactorStall&) = delete ("a stall is released once");
    void release() noexcept;

    struct Gate;

private:
    std::shared_ptr<Gate> m_gate;
};

#ifdef _WIN32
// Faults the Windows watcher reports instead of calling the OS (file_watcher_win.cpp).
enum class WindowsWatchFault : std::uint8_t {
    None,
    NotifyEnumDir, // A completion fails with ERROR_NOTIFY_ENUM_DIR (overflow).
    ExRefused,     // The first ReadDirectoryChangesExW call fails with ERROR_INVALID_FUNCTION.
    PlainRefused,  // Every first plain ReadDirectoryChangesW call fails with ERROR_INVALID_FUNCTION.
    ReadError,     // Completions fail with ERROR_ACCESS_DENIED while the root is intact.
};
// The next `count` occurrences of `fault` in any new or running Windows watch; count 0 clears it.
void injectWindowsWatchFaultForTesting(WindowsWatchFault fault, unsigned count) noexcept;
// The ReadDirectoryChanges buffer size for watches started from now on; 0 restores the default
// (256 KiB on fixed drives, 64 KiB elsewhere).
void setWindowsReadBufferBytesForTesting(std::size_t bytes) noexcept;
#else
// Watches started from now on fail inotify_add_watch with ENOSPC once their instance holds `limit`
// watches; 0 removes the limit.
void setInotifyWatchLimitForTesting(std::size_t limit) noexcept;
// While true, creating an inotify instance fails with EMFILE (the instance limit).
void setInotifyInstanceLimitForTesting(bool fail) noexcept;
#endif

} // namespace testing
} // namespace frost::io
