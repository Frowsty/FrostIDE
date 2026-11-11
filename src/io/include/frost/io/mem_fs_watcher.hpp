// SPDX-License-Identifier: Apache-2.0
//
/*
 * MemFsWatcher: the deterministic IFileWatcher double over MemFs (PLAN.md section 6.2; ADR 0020).
 * Watches MemFs through its observer hook and releases batches on the scheduler's clock, so under a
 * SimExecutor every batch boundary is a function of virtual time. Shares WatchQueue and ChangeCoalescer
 * with the real watchers, so coalescing behaves identically.
 */
/*
 * Semantics. watch() checks the root with MemFs::status (NotFound, InvalidArgument for a file) and
 * starts observing before it completes, so every mutation after completion is reported. MemFs reports
 * inotify-like sequences (mem_fs.hpp), so batches look like the Linux watcher's. Removing or moving the
 * root (or an ancestor) delivers Removed(root) and then ends the watch with NotFound. Exclusions match
 * case-sensitively, as MemFs paths do.
 *
 * Test controls apply to the watches live at the call:
 *   injectOverflow(path)   Rescan(path) in every watch whose root is at or above `path` (or below it:
 *                          then Rescan(root)), as a kernel queue overflow would;
 *   injectFailure(code)    the next next() of every watch fails once with `code`; pending changes stay;
 *   setDegraded(reason, v) every watch switches to WatchMode::Polling with that reason and reports
 *                          Rescan(root), as a real watcher does when it degrades.
 *
 * Thread safety: all members may be called from any thread. With a SimExecutor, MemFs mutations and the
 * controls must run on its owner thread, because they resume a waiting next() through enqueue().
 * Lifetime: the MemFs and the scheduler outlive the watcher, which outlives its watches.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"

#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {

class MemFsWatcher final : public IFileWatcher {
public:
    MemFsWatcher(MemFs& fs, sched::IScheduler& scheduler);
    // Precondition: no live watch (checked).
    ~MemFsWatcher();
    MemFsWatcher(const MemFsWatcher&) = delete ("watches refer to their watcher");
    MemFsWatcher& operator=(const MemFsWatcher&) = delete ("watches refer to their watcher");

    // See IFileWatcher::watch. The root must be absolute in the MemFs's path style.
    sched::Task<Result<std::unique_ptr<IDirectoryWatch>>> watch(std::string root, WatchOptions options,
                                                                std::stop_token token = {}) override;

    void injectOverflow(std::string_view path);
    void injectFailure(ErrorCode code);
    void setDegraded(DegradeReason reason, std::uint64_t reasonValue = 0);

    // The final implementation is deleted as WatchState; IMemFsObserver only borrows it.
    // NOLINTNEXTLINE(cppcoreguidelines-virtual-class-destructor): final type has concrete shared_ptr ownership.
    struct WatchState; // mem_fs_watcher.cpp.

private:
    friend class MemFsDirectoryWatch;
    void forget(WatchState* state);

    MemFs& m_fs;
    sched::IScheduler& m_scheduler;
    mutable Mutex m_mutex;
    std::vector<std::shared_ptr<WatchState>> m_live FROST_GUARDED_BY(m_mutex);
};

} // namespace frost::io
