// SPDX-License-Identifier: Apache-2.0
//
// WatchQueue: the consumer side shared by every IDirectoryWatch implementation (real, polling and MemFs
// watchers; ADR 0020). Producers push raw events from any thread; the queue filters them, folds them
// into a ChangeCoalescer and hands out debounced batches through next(). Asio-free.
//
/*
 * Ownership. Shared (std::shared_ptr) by the public watch object, the producer state (a reactor-bound
 * state, a polling task or a MemFs observer) and every running next() task, so a next() keeps working
 * when the watch is destroyed while it runs (it then ends with Cancelled).
 *
 * Locking. One Mutex guards everything. Producers and next() hold it only for O(event) work. The
 * waiter is resumed under the lock: a scheduler stop callback that wakes the waiter therefore waits for
 * a concurrent push that took it, so no resume can be enqueued after ThreadPool::shutdown has drained.
 *
 * Release rule. A batch is due at min(last event + debounce, first event + maxDelay), measured on the
 * scheduler's clock (virtual under SimExecutor), or at once after close() with deliverPending. A next()
 * waits in two ways: with nothing pending it suspends until a push, close() or a stop wakes it; with
 * changes pending it sleeps until the due time on the scheduler. Both waits observe a per-call stop
 * source fed by the caller's token, the queue's close token and the scheduler's stop token.
 *
 * Filters (in push, before the coalescer): paths outside the root; entries strictly below an excluded
 * directory (ordinal case-insensitive when configured, as on Windows) or for which the caller's
 * WatchOptions::excludedBelow says so; entries deeper than the root's
 * children for non-recursive watches (an overflow there becomes Rescan(root)); and final names
 * containing ".frost-tmp-" unless reportFrostTempFiles.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/io/path.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"

#include "change_coalescer.hpp"

#include <coroutine>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io::detail {

struct WatchQueueConfig {
    std::string root;     // Normalised.
    char separator = '/'; // The root's path style.
    WatchOptions options; // Validated (validateWatchOptions).
    bool foldCaseForExclusions = false;
    WatchMode mode = WatchMode::Native;
};

// Checks the WatchOptions rules of IFileWatcher::watch and normalises `excluded` against `root` (already
// normalised, `style` its path style). Errors: InvalidArgument.
[[nodiscard]] Result<WatchOptions> validateWatchOptions(const std::string& root, WatchOptions options, PathStyle style);

class WatchQueue {
public:
    // `scheduler` resumes next() and owns the clock; it outlives every next() task (exit order).
    [[nodiscard]] static std::shared_ptr<WatchQueue> create(sched::IScheduler& scheduler, WatchQueueConfig config);
    WatchQueue(sched::IScheduler& scheduler, WatchQueueConfig config);
    WatchQueue(const WatchQueue&) = delete ("shared by pointer");
    WatchQueue& operator=(const WatchQueue&) = delete ("shared by pointer");
    ~WatchQueue() = default;

    // --- Producers (any thread) ---
    // Filters and folds events in; returns how many were accepted. Ignored after close().
    std::size_t push(const RawEvent& event);
    std::size_t push(std::span<const RawEvent> events);
    // Rescan(path) (kernel overflow, a replaced directory, a mode change).
    void pushOverflow(std::string path);
    /*
     * Terminal. With deliverPending, next() first hands out what is pending (at once, without waiting
     * for the debounce), then fails with `error`; otherwise it fails with `error` at once. Idempotent:
     * the first close wins.
     */
    void close(Error error, bool deliverPending);
    // The next next() (or the running one) fails once with `error`; pending changes stay (MemFsWatcher).
    void injectFailure(Error error);
    // Updates status fields other than the counters the queue keeps itself.
    template<class F>
    void updateStatus(F&& update) {
        LockGuard lock(m_mutex);
        update(m_status);
    }
    void setMode(WatchMode mode, DegradeReason reason, std::uint64_t value, std::string detail);

    // --- Consumer ---
    // IDirectoryWatch::next. Takes the queue by value: the task owns its own reference.
    [[nodiscard]] static sched::Task<Result<FileChangeBatch>> next(std::shared_ptr<WatchQueue> queue,
                                                                   std::stop_token token);

    [[nodiscard]] WatchStatus status() const;
    [[nodiscard]] const std::string& root() const noexcept { return m_config.root; }
    [[nodiscard]] const WatchQueueConfig& config() const noexcept { return m_config; }
    [[nodiscard]] bool closed() const;
    // Accepts `path` (absolute, in the root's style) as push() would; for producers that filter early
    // (the Linux watcher never watches excluded directories).
    [[nodiscard]] bool accepts(std::string_view path) const;
    [[nodiscard]] bool excludedBelow(std::string_view path) const;

private:
    class WaitForEvents;
    struct Wake;

    bool acceptLocked(RawEvent& event) const FROST_REQUIRES(m_mutex);
    void pushLocked(const RawEvent& event) FROST_REQUIRES(m_mutex);
    void wakeLocked() FROST_REQUIRES(m_mutex);
    void wakeWaiter();
    [[nodiscard]] bool readyLocked() const FROST_REQUIRES(m_mutex);

    sched::IScheduler& m_scheduler;
    const WatchQueueConfig m_config;
    std::stop_source m_closeSource; // Requested by close(); thread-safe by itself.

    mutable Mutex m_mutex;
    ChangeCoalescer m_coalescer FROST_GUARDED_BY(m_mutex);
    IClock::SteadyTime m_firstEvent FROST_GUARDED_BY(m_mutex){};
    IClock::SteadyTime m_lastEvent FROST_GUARDED_BY(m_mutex){};
    bool m_closed FROST_GUARDED_BY(m_mutex) = false;
    bool m_deliverPending FROST_GUARDED_BY(m_mutex) = false;
    std::optional<Error> m_closeError FROST_GUARDED_BY(m_mutex);
    std::optional<Error> m_injected FROST_GUARDED_BY(m_mutex);
    bool m_consumerActive FROST_GUARDED_BY(m_mutex) = false;
    std::uint64_t m_sequence FROST_GUARDED_BY(m_mutex) = 0;
    WatchStatus m_status FROST_GUARDED_BY(m_mutex);
    // The suspended next(), if any, and the lane to resume it on.
    std::coroutine_handle<> m_waiter FROST_GUARDED_BY(m_mutex);
    sched::Lane m_waiterLane FROST_GUARDED_BY(m_mutex) = sched::Lane::UserVisible;
};

// Ordinal case-insensitive "text starts with prefix" (ASCII fast path; CompareStringOrdinal otherwise on
// Windows, byte-wise elsewhere). Pure.
[[nodiscard]] bool startsWithFoldCase(std::string_view text, std::string_view prefix);

} // namespace frost::io::detail
