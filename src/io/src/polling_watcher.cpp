// SPDX-License-Identifier: Apache-2.0
//
// The polling backend of RealFileWatcher (ADR 0020): periodic snapshots of the tree (path -> type, size,
// mtime) and their differences as raw events. Used when forced, when the file system refuses native
// watching, when an inotify limit is hit and after repeated read errors.
//
/*
 * Period: max(pollInterval, min(20 x the last scan's duration, 60 s)), so a slow tree (a network share)
 * costs at most about 5% of a Background thread. Scans are scanTree walks (tree_scan.hpp) on
 * Lane::Background, one task per directory, without ignore rules or skipped names; they follow
 * WatchOptions (recursive, excluded) and do not follow directory symlinks.
 * Polling reports Created, Removed, Modified of non-directories (size or mtime), type changes, and
 * Modified of a watched directory (the root included) whose mtime changed: an entry that appeared and
 * vanished between two scans is invisible to the diff, but a consumer's lagged re-read may have seen
 * it, and the consumer re-lists such a directory (file_watcher.hpp). A residual race: a change within
 * the file system's timestamp granularity of the previous scan's read. A directory that cannot be listed keeps its
 * previous snapshot (status().unreadableDirectories counts them). Renames are seen as Removed plus Created. Removal of
 * the root ends the watch: Removed(root), then NotFound.
 */

#include "frost/core/assert.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/tree_scan.hpp"

#include "real_file_watcher_impl.hpp" // asio first.

#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <utility>
#include <vector>

namespace frost::io::detail {
namespace {

using namespace std::chrono_literals;

struct SnapshotEntry {
    FileType type = FileType::Regular;
    std::uint64_t size = 0;
    IClock::SystemTime mtime{};
};
using Snapshot = std::map<std::string, SnapshotEntry, std::less<>>;

[[nodiscard]] std::string childPath(const std::string& directory, std::string_view name, char separator) {
    std::string path = directory;
    if (path.back() != separator) {
        path += separator;
    }
    path += name;
    return path;
}

struct ScanResult {
    Snapshot snapshot;
    IClock::SystemTime rootMtime{};
    std::size_t unreadable = 0;
};

// Forwards a stop request into another stop source.
struct Forward {
    std::stop_source* target;
    void operator()() const noexcept { static_cast<void>(target->request_stop()); }
};

class PollingState final : public BoundState, public WatchProducer {
public:
    PollingState(Reactor::Impl& reactor, std::shared_ptr<RealFileWatcher::Impl> owner,
                 std::shared_ptr<WatchQueue> queue)
        : BoundState(reactor), m_owner(std::move(owner)), m_queue(std::move(queue)) {}

    void shutdownOnStrand() noexcept override {
        requestStop();
        m_queue->close(Error(ErrorCode::Cancelled, "the reactor is shut down"), /*deliverPending=*/false);
    }
    void requestStop() noexcept override { static_cast<void>(m_stop.request_stop()); }

    // Scans the tree (Background). Errors: Cancelled, and the root's own listDirectory error.
    sched::Task<Result<ScanResult>> scan(std::stop_token token);
    // The events that turn `m_snapshot` (and `m_rootMtime`) into `scan`'s.
    [[nodiscard]] std::vector<RawEvent> diff(const ScanResult& scan) const;
    // The loop spawned on Lane::Background; `scanFirst` when no snapshot was taken yet.
    static sched::Task<void> run(std::shared_ptr<PollingState> self, bool scanFirst);
    // Handles a failed scan of the root; true when the watch ended.
    bool rootFailed(const Error& error);

    std::shared_ptr<RealFileWatcher::Impl> m_owner;
    std::shared_ptr<WatchQueue> m_queue;
    std::stop_source m_stop;
    Snapshot m_snapshot;              // Touched only by the scanning task (watch() start-up, then the loop).
    IClock::SystemTime m_rootMtime{}; // Likewise.
    IClock::Duration m_period{};
};

sched::Task<Result<ScanResult>> PollingState::scan(std::stop_token token) {
    sched::IScheduler& scheduler = m_owner->scheduler;
    const WatchQueueConfig& config = m_queue->config();
    const char separator = config.separator;
    IFileSystem& fs = realFileSystem();
    ScanResult result;
    // Directory mtimes are read before the directory is listed (scanTree's withStatus), so a change during
    // the listing shows at the next scan at the latest.
    if (auto root = fs.status(config.root); root) {
        result.rootMtime = root->mtime;
    }
    ScanOptions options;
    options.readGitignore = false;
    options.skipNames.clear();
    options.skipFrostTempFiles = false; // The watch queue filters them (WatchOptions::reportFrostTempFiles).
    options.recursive = config.options.recursive;
    // Only watched listings count: an excluded directory is an entry whose mtime is noise.
    options.descend = [queue = m_queue.get(), separator](std::string_view directory) {
        return !queue->excludedBelow(childPath(std::string(directory), "x", separator));
    };
    options.withStatus = true;
    options.lane = sched::Lane::Background;
    auto scanned = co_await scanTree(fs, scheduler, config.root, std::move(options), token);
    if (!scanned) {
        if (scanned.error().code() == ErrorCode::Cancelled) {
            co_return fail(ErrorCode::Cancelled, "polling stopped");
        }
        co_return std::unexpected(std::move(scanned.error())); // The root's own listing error.
    }
    for (ScanEntry& entry : scanned->entries) {
        // Entries scanTree did not stat (links, other files, directories not descended) have size 0 and
        // no mtime; regular files and descended directories have both from status().
        result.snapshot.emplace(std::move(entry.path),
                                SnapshotEntry{entry.type, entry.status.size, entry.status.mtime});
    }
    for (const ScanIssue& issue : scanned->issues) {
        if (issue.kind != ScanIssueKind::ListFailed || issue.error.code() == ErrorCode::NotFound) {
            continue; // A directory that vanished since its parent was listed is reported as removed.
        }
        // Unreadable: keep what the previous snapshot had below it.
        ++result.unreadable;
        const std::string prefix = childPath(issue.path, "", separator);
        for (auto it = m_snapshot.lower_bound(prefix); it != m_snapshot.end() && it->first.starts_with(prefix); ++it) {
            result.snapshot.insert(*it);
        }
    }
    co_return result;
}

std::vector<RawEvent> PollingState::diff(const ScanResult& scan) const {
    const Snapshot& next = scan.snapshot;
    const IClock::SystemTime rootMtime = scan.rootMtime;
    const char separator = m_queue->config().separator;
    std::vector<RawEvent> events;
    // Subtrees already reported whole (a removed, created or retyped directory).
    std::string skipOld;
    std::string skipNew;
    const auto below = [&](const std::string& path, const std::string& prefix) {
        return !prefix.empty() && isStrictlyBelow(path, prefix, separator);
    };
    auto oldIt = m_snapshot.begin();
    auto newIt = next.begin();
    while (oldIt != m_snapshot.end() || newIt != next.end()) {
        const bool takeOld = newIt == next.end() || (oldIt != m_snapshot.end() && oldIt->first < newIt->first);
        const bool takeNew = oldIt == m_snapshot.end() || (newIt != next.end() && newIt->first < oldIt->first);
        if (takeOld) {
            if (!below(oldIt->first, skipOld)) {
                events.push_back({RawKind::Removed, oldIt->first, oldIt->second.type, false, 0});
                skipOld = oldIt->first;
            }
            ++oldIt;
            continue;
        }
        if (takeNew) {
            if (!below(newIt->first, skipNew)) {
                events.push_back({RawKind::Created, newIt->first, newIt->second.type, false, 0});
                skipNew = newIt->first;
            }
            ++newIt;
            continue;
        }
        // The same path in both.
        const SnapshotEntry& before = oldIt->second;
        const SnapshotEntry& after = newIt->second;
        if (below(oldIt->first, skipOld) || below(newIt->first, skipNew)) {
            // Below a retyped entry: covered by its Rescan.
        } else if (before.type != after.type) {
            events.push_back({RawKind::Removed, oldIt->first, before.type, false, 0});
            events.push_back({RawKind::Created, newIt->first, after.type, false, 0});
            skipOld = oldIt->first;
            skipNew = newIt->first;
        } else if (after.type == FileType::Regular && (before.size != after.size || before.mtime != after.mtime)) {
            events.push_back({RawKind::Modified, newIt->first, after.type, false, 0});
        } else if (after.type == FileType::Directory && before.mtime != after.mtime) {
            // Its children changed, perhaps only between two scans: the consumer re-lists it.
            events.push_back({RawKind::Modified, newIt->first, after.type, false, 0});
        }
        ++oldIt;
        ++newIt;
    }
    if (rootMtime != m_rootMtime) {
        events.push_back({RawKind::Modified, m_queue->root(), FileType::Directory, false, 0});
    }
    return events;
}

bool PollingState::rootFailed(const Error& error) {
    if (error.code() == ErrorCode::Cancelled) {
        return true;
    }
    if (error.code() == ErrorCode::NotFound) {
        const std::string& root = m_queue->root();
        static_cast<void>(m_queue->push(RawEvent{RawKind::Removed, root, FileType::Directory, false, 0}));
        m_queue->close(Error(ErrorCode::NotFound, std::format("watch '{}': the root was removed or moved", root)),
                       /*deliverPending=*/true);
        return true;
    }
    // Transient (a network hiccup): keep the snapshot and try again next period.
    m_queue->updateStatus([&](WatchStatus& status) { status.detail = std::string(error.message()); });
    return false;
}

sched::Task<void> PollingState::run(std::shared_ptr<PollingState> self, bool scanFirst) {
    sched::IScheduler& scheduler = self->m_owner->scheduler;
    std::stop_source stop;
    const std::stop_token token = stop.get_token();
    std::stop_callback<Forward> fromWatch(self->m_stop.get_token(), Forward{&stop});
    std::stop_callback<Forward> fromScheduler(scheduler.stopToken(), Forward{&stop});
    const IClock& clock = scheduler.clock();
    const IClock::Duration interval = self->m_queue->config().options.pollInterval;
    self->m_period = interval;
    if (scanFirst) {
        auto first = co_await self->scan(token);
        if (!first) {
            if (self->rootFailed(first.error())) {
                co_return;
            }
        } else {
            self->m_rootMtime = first->rootMtime;
            self->m_snapshot = std::move(first->snapshot);
        }
    }
    for (;;) {
        if (!co_await scheduler.sleepFor(self->m_period, token)) {
            co_return; // Stopped, or the scheduler is shutting down.
        }
        const IClock::SteadyTime start = clock.steadyNow();
        auto next = co_await self->scan(token);
        const IClock::Duration took = clock.steadyNow() - start;
        self->m_period = std::max(interval, std::min<IClock::Duration>(took * 20, 60s));
        if (!next) {
            if (self->rootFailed(next.error())) {
                co_return;
            }
            continue;
        }
        const std::vector<RawEvent> events = self->diff(*next);
        self->m_rootMtime = next->rootMtime;
        self->m_snapshot = std::move(next->snapshot);
        self->m_queue->updateStatus([&](WatchStatus& status) { status.unreadableDirectories = next->unreadable; });
        if (!events.empty()) {
            static_cast<void>(self->m_queue->push(events));
        }
    }
}

} // namespace

sched::Task<Result<NativeOutcome>> startPollingWatch(std::shared_ptr<RealFileWatcher::Impl> owner,
                                                     std::shared_ptr<WatchQueue> queue, std::stop_token token) {
    auto bound = makeBound<PollingState>(owner->reactor, owner, queue);
    if (!bound) {
        co_return std::unexpected(std::move(bound.error()));
    }
    std::shared_ptr<PollingState> state = std::move(*bound);
    auto first = co_await state->scan(token);
    if (!first) {
        state->requestStop();
        co_return std::unexpected(std::move(first.error()));
    }
    state->m_rootMtime = first->rootMtime;
    state->m_snapshot = std::move(first->snapshot);
    queue->updateStatus([&](WatchStatus& status) { status.unreadableDirectories = first->unreadable; });
    owner->scheduler.spawn(sched::Lane::Background, PollingState::run(state, /*scanFirst=*/false)).detach();
    co_return NativeOutcome{std::move(state), {}};
}

NativeOutcome startPollingFromStrand(std::shared_ptr<RealFileWatcher::Impl> owner, std::shared_ptr<WatchQueue> queue) {
    auto bound = makeBound<PollingState>(owner->reactor, owner, queue);
    if (!bound) {
        queue->close(Error(ErrorCode::Cancelled, "the reactor is shut down"), /*deliverPending=*/false);
        return {};
    }
    std::shared_ptr<PollingState> state = std::move(*bound);
    owner->scheduler.spawn(sched::Lane::Background, PollingState::run(state, /*scanFirst=*/true)).detach();
    return NativeOutcome{std::move(state), {}};
}

} // namespace frost::io::detail
