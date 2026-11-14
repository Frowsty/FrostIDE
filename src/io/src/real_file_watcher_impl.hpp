// SPDX-License-Identifier: Apache-2.0
//
// Internals shared by RealFileWatcher's backends (ADR 0020): RealFileWatcher::Impl, the WatchProducer
// seam every backend implements (ReadDirectoryChangesW, inotify, polling) and the public watch object.
// Includes asio through reactor_impl.hpp, so it is included first by io .cpp files only.
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"

#include "reactor_impl.hpp" // asio first.
#include "watch_queue.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>

namespace frost::io {
namespace detail {

class InotifyState; // file_watcher_linux.cpp.

/*
 * The source of one watch's raw events. requestStop() may run on any thread, never blocks (B21) and is
 * idempotent; afterwards the producer pushes nothing more and releases its OS resources (on its strand
 * when it has one). A producer that degrades replaces itself with a polling producer it owns, so the
 * public watch only ever knows the first one.
 */
class WatchProducer {
public:
    virtual void requestStop() noexcept = 0;

protected:
    WatchProducer() = default;
    WatchProducer(const WatchProducer&) = default;
    WatchProducer& operator=(const WatchProducer&) = default;
    ~WatchProducer() = default;
};

// What a native backend reports when it cannot watch `root` natively: the reason to poll instead.
struct DegradeTo {
    DegradeReason reason = DegradeReason::Unsupported;
    std::uint64_t value = 0;
    std::string detail{};
};

} // namespace detail

struct RealFileWatcher::Impl {
    Impl(Reactor::Impl& reactorImpl, sched::IScheduler& schedulerRef) noexcept
        : reactor(reactorImpl), scheduler(schedulerRef) {}

    Reactor::Impl& reactor;
    sched::IScheduler& scheduler;
    // For ~RealFileWatcher's precondition. Relaxed: the owner checks them after joining its users.
    std::atomic<std::int64_t> liveWatches{0};
    std::atomic<std::int64_t> pendingWatchCalls{0};
#ifndef _WIN32
    // The instance shared by this watcher's inotify watches, created by the first one.
    Mutex inotifyMutex;
    std::shared_ptr<detail::InotifyState> inotify FROST_GUARDED_BY(inotifyMutex);
#endif
};

namespace detail {

// A running producer (owning: converted from the backend's bound state), or the reason to poll instead
// (producer null).
struct NativeOutcome {
    std::shared_ptr<WatchProducer> producer{};
    DegradeTo degrade{};
};

/*
 * Starts the platform's native watch of `queue->root()` (already validated, a directory) on the calling
 * Background task. Completes once recording has started. Errors: Cancelled (token, reactor), NotFound,
 * PermissionDenied, IoError; limits and unsupported file systems are a NativeOutcome without producer.
 * file_watcher_win.cpp / file_watcher_linux.cpp.
 */
[[nodiscard]] sched::Task<Result<NativeOutcome>> startNativeWatch(std::shared_ptr<RealFileWatcher::Impl> owner,
                                                                  std::shared_ptr<WatchQueue> queue,
                                                                  std::stop_token token);

/*
 * Starts polling `queue->root()` (polling_watcher.cpp). With `initialSnapshot` the first scan runs before
 * the task completes (watch() start-up); otherwise it runs on the polling task, which then reports the
 * differences from its first scan on (mid-life degradation, after the caller pushed Rescan(root)).
 * Errors: Cancelled (token, reactor), NotFound, PermissionDenied, IoError (the root cannot be listed).
 */
[[nodiscard]] sched::Task<Result<NativeOutcome>> startPollingWatch(std::shared_ptr<RealFileWatcher::Impl> owner,
                                                                   std::shared_ptr<WatchQueue> queue,
                                                                   std::stop_token token);
// The same without a coroutine, for a backend that degrades on its strand: binds the polling state and
// spawns its loop (the first scan runs there). Returns null if the reactor is shutting down.
[[nodiscard]] NativeOutcome startPollingFromStrand(const std::shared_ptr<RealFileWatcher::Impl>& owner,
                                                   const std::shared_ptr<WatchQueue>& queue);

// The public watch object of RealFileWatcher: stops its producer and closes its queue on destruction.
class RealDirectoryWatch final : public IDirectoryWatch {
public:
    RealDirectoryWatch(std::shared_ptr<RealFileWatcher::Impl> owner, std::shared_ptr<WatchQueue> queue,
                       NativeOutcome producer) noexcept;
    ~RealDirectoryWatch() override;
    RealDirectoryWatch(const RealDirectoryWatch&) = delete ("a watch has one owner");
    RealDirectoryWatch& operator=(const RealDirectoryWatch&) = delete ("a watch has one owner");

    sched::Task<Result<FileChangeBatch>> next(std::stop_token token) override {
        return WatchQueue::next(m_queue, std::move(token));
    }
    const std::string& root() const noexcept override { return m_queue->root(); }
    WatchStatus status() const override { return m_queue->status(); }

private:
    std::shared_ptr<RealFileWatcher::Impl> m_owner;
    std::shared_ptr<WatchQueue> m_queue;
    NativeOutcome m_producer;
};

} // namespace detail
} // namespace frost::io
