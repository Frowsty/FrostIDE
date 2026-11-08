// SPDX-License-Identifier: Apache-2.0
//
/*
 * IFileWatcher: the file-watching seam of frost::io (PLAN.md section 2, "File watching"; section 6.2:
 * IFileWatcher -> MemFs with a virtual clock). RealFileWatcher watches the disk (ReadDirectoryChangesW
 * on IOCP on Windows, inotify on Linux, polling as the fallback); MemFsWatcher (mem_fs_watcher.hpp) is
 * the deterministic double. ADR 0020 records the design.
 */
/*
 * Model. watch(root) starts recording every change below `root` and returns an IDirectoryWatch. Its
 * next() hands out FileChangeBatch values: the net changes since the previous batch, coalesced and
 * debounced, never empty. A consumer keeps its own model of the tree (the File System view, settings,
 * the SaveManager) and applies each batch to it; the watcher never keeps a mirror of the tree.
 *
 * Batch semantics. Entries are net changes against the tree as it was after the previous batch. The
 * disk may already be ahead of the batch when the consumer applies it; the rules below make that safe.
 * A consumer applies a batch in this order:
 *   1. Lift every Renamed source subtree (oldPath) out of the model, deepest oldPath first, then place
 *      each at its destination (path), shallowest destination first, replacing whatever is there. A
 *      Renamed whose source the consumer does not have is handled as a re-read of its destination.
 *   2. Delete every Removed subtree; a no-op if the consumer does not have it.
 *   3. Re-read from disk, now, every Created, Modified, Rescan and Renamed-destination path:
 *      - a re-read that finds nothing removes the path and its subtree from the model;
 *      - a re-read that finds a different type replaces the node, dropping the old subtree or listing
 *        the new one;
 *      - Created, Rescan and Renamed destinations that are directories are re-listed with their subtree;
 *      - Modified on a directory (the root included) means its attributes or its list of children may
 *        have changed, never that it was replaced: re-list its direct children only, dropping those
 *        no longer on disk and reading new ones with their subtree; children already in the model keep
 *        their state (their own changes arrive as their own entries). Polling reports a directory whose
 *        mtime changed this way, which removes entries a lagged re-read saw between two scans.
 * Created, Modified and Rescan may name paths the consumer does not have (rename-over, moves from
 * outside the watch); treat them as new. Removed may name paths the consumer never had: an entry that
 * appeared and vanished within one batch is still reported, because a consumer that applied the
 * previous batch late may have read it during its re-reads. The order of entries inside the vector does
 * not matter beyond the lift and place order of step 1. A consumer interested in one path p (settings, the SaveManager)
 * reacts to: any entry whose path is p; Created, Removed or Rescan of an ancestor of p, and Modified of
 * an ancestor that is no directory; and a Renamed whose source or destination is p or an ancestor of p.
 * (A Removed below a Created entry, a Renamed source or destination or a Modified non-directory is left
 * out: lifting or re-reading that ancestor covers it.)
 *
 * Guidance for dependent slices (ADR 0020): one recursive watch per project root, owned by the File
 * System view's model, which fans batches out; settings and the SaveManager use that fan-out or cheap
 * non-recursive watches of specific directories. Overlapping watches are supported; on Linux they share
 * one inotify instance per RealFileWatcher.
 *
 * Backpressure. Events coalesce while the consumer is away: the pending set is bounded by
 * WatchOptions::maxPendingPaths, past which it collapses into Rescan(root).
 *
 * Exit order (also in reactor.hpp): Reactor::shutdown (pending next() calls end with Cancelled), then
 * destroy the watches, then the watchers, then shut down the schedulers they resume on, then destroy
 * the Reactor.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {

class Reactor;

enum class FileChangeKind : std::uint8_t {
    Created,  // Something new exists at `path` (re-read it; a directory is re-listed with its subtree).
    Modified, // The entry at `path` changed in place (content or attributes; a directory: re-list its children).
    Removed,  // Nothing exists at `path` any more; delete its subtree.
    Renamed,  // The entry at `oldPath` (at the previous batch) now lives at `path`.
    Rescan,   // Anything at or below `path` may have changed (overflow, replaced directories); re-list it.
};

struct FileChange {
    FileChangeKind kind = FileChangeKind::Modified;
    // The end-of-batch path: the normalised watch root plus the OS-reported relative name, in the root's
    // path style. Not re-validated: names Win32 would rewrite can appear (path.hpp's verbatim form is the
    // escape hatch for re-reading them). POSIX names are the kernel's bytes, valid UTF-8 or not (as DirEntry).
    std::string path{};
    std::string oldPath{}; // Renamed only: the start-of-batch path.
    // The end type when known (Removed: the last known type). RDCW-Ex, polling and MemFs know every type;
    // inotify knows directories and the exact type of created and moved entries; plain ReadDirectoryChangesW
    // knows only resolved rename destinations.
    std::optional<FileType> type{};

    friend bool operator==(const FileChange&, const FileChange&) = default;
};

enum class WatchMode : std::uint8_t {
    Native,  // ReadDirectoryChanges(Ex)W, inotify, or MemFs's observer hook.
    Polling, // Periodic snapshots and diffs (no rename pairing).
};

// Why a watch is not (or no longer) Native. The UI formats its text from this and reasonValue.
enum class DegradeReason : std::uint8_t {
    None,
    Forced,            // WatchOptions::forcePolling.
    Unsupported,       // The file system refused native watching (e.g. \\wsl.localhost over 9P).
    NetworkFileSystem, // Linux: the root's statfs magic is a network or virtual file system (reasonValue).
    WatchLimit,        // Linux: fs.inotify.max_user_watches was hit (reasonValue: the limit).
    InstanceLimit,     // Linux: fs.inotify.max_user_instances or the fd limit was hit (reasonValue: the limit).
    RepeatedErrors,    // Three consecutive read failures with the root intact.
};

struct FileChangeBatch {
    // Never empty. Sorted byte-wise by `path` (a Renamed by its destination). No path appears in two
    // entries (both paths of a Renamed count).
    std::vector<FileChange> changes{};
    std::uint64_t sequence = 0; // 1, 2, ... per watch.
    WatchMode mode = WatchMode::Native;
};

struct WatchOptions {
    bool recursive = true;
    IClock::Duration debounce = std::chrono::milliseconds(50);  // Quiet period before a batch is released.
    IClock::Duration maxDelay = std::chrono::milliseconds(500); // A batch is due at most this long after its
                                                                // first event, even in a storm.
    std::size_t maxPendingPaths = 65'536;                       // Beyond: the pending set collapses into Rescan(root).
    // Absolute directories under root. Entries strictly below them are never reported (Linux: never
    // watched); the directory's own creation, removal and renames are reported. The real Windows watcher
    // matches them ordinal case-insensitively; MemFsWatcher matches as MemFs does (case-sensitively).
    std::vector<std::string> excluded{};
    /*
     * More exclusions the caller decides (the project's directory roles, UX-23): true when `path`
     * (absolute, in the root's path style, strictly below the root) lies strictly below a directory
     * whose entries are never reported, with the same effect as `excluded` (Linux and polling: such
     * directories are never watched or listed). Asked for every OS event and, on Linux and with
     * polling, for every directory the watcher would watch, on reactor and scheduler threads,
     * possibly concurrently: keep it cheap and thread-safe. Empty: no extra exclusions.
     */
    std::function<bool(std::string_view path)> excludedBelow{};
    IClock::Duration pollInterval = std::chrono::seconds(2); // Minimum polling period (>= 100 ms).
    bool forcePolling = false;
    // false: names containing the writeFileAtomic temp marker (".frost-tmp-") are filtered, matching the
    // tree scanner's default; the target of an atomic save then arrives as Modified (or Rescan) only.
    bool reportFrostTempFiles = false;
};

// A point-in-time view of a watch, for diagnostics and tests; every field may be stale on return.
struct WatchStatus {
    WatchMode mode = WatchMode::Native;
    DegradeReason reason = DegradeReason::None;
    std::uint64_t reasonValue = 0;         // WatchLimit/InstanceLimit: the limit; NetworkFileSystem: the statfs magic.
    std::string detail{};                  // English, for logs only (UI text is formatted from reason/reasonValue).
    std::uint64_t rawEvents = 0;           // OS events accepted by the filter so far.
    std::uint64_t batches = 0;             // Batches handed out.
    std::uint64_t rescans = 0;             // Rescan entries handed out.
    std::size_t osWatches = 0;             // Linux: inotify watches this watch added; Windows: open directory handles.
    std::size_t pendingPaths = 0;          // Paths waiting in the coalescer.
    std::size_t pendingSetup = 0;          // Linux: new-directory setup tasks still running.
    std::size_t unreadableDirectories = 0; // Directories skipped with PermissionDenied.
    // Polling: the regular files of the last snapshot (what the watch compares between scans); 0 in
    // the native modes, which keep no per-file state.
    std::size_t watchedFiles = 0;
};

/*
 * One active watch. Owned by the caller through std::unique_ptr. Thread safety: next() has a single
 * consumer (a second concurrent next() is a contract violation, checked); root() and status() may be
 * called from any thread; the destructor may run on any thread, also while a next() is pending.
 */
class IDirectoryWatch {
public:
    // Stops watching and never blocks (Budget: B21). A pending next() resumes with Cancelled.
    virtual ~IDirectoryWatch() = default;
    IDirectoryWatch(const IDirectoryWatch&) = delete ("a watch has one owner");
    IDirectoryWatch& operator=(const IDirectoryWatch&) = delete ("a watch has one owner");

    /*
     * The next batch. Not a coroutine itself: implementations capture their shared queue before
     * returning, so the task never touches `this` and stays valid if the watch is destroyed before or
     * while it runs. Resumes on the lane the awaiter was on when it suspended (Lane::UserVisible when it
     * was not running on the watch's scheduler). A next() cancelled through `token` returns Cancelled and
     * loses nothing: the pending changes stay for the next call. Never blocks a thread (Budget: B21).
     * Errors: Cancelled (the token, the watch destroyed, the reactor or scheduler shutting down);
     * NotFound (the root was removed or moved, after a batch carrying Removed(root), which is always
     * delivered first); IoError (unrecoverable, every fallback failed); an injected code (MemFsWatcher).
     * Complexity: O(P log P) to take P pending paths.
     */
    [[nodiscard]] virtual sched::Task<Result<FileChangeBatch>> next(std::stop_token token = {}) = 0;

    // The normalised root this watch reports under.
    [[nodiscard]] virtual const std::string& root() const noexcept = 0;

    // Any thread.
    [[nodiscard]] virtual WatchStatus status() const = 0;

protected:
    IDirectoryWatch() = default;
};

class IFileWatcher {
public:
    /*
     * Starts watching `root` (normalised with normalizePath; IDirectoryWatch::root() returns that form).
     * When the task completes, recording has started: every change after completion is reported. On
     * Windows the first read has been issued, on Linux every directory has its watch, with polling the
     * first snapshot has been taken. Resource limits never fail watch(): the watch degrades to Polling
     * and status() gives the reason. Parameters are taken by value because the task is lazy. Thread-safe.
     * Errors: InvalidArgument (a relative root; a root that is not a directory; debounce < 0,
     * maxDelay < debounce, maxPendingPaths == 0, pollInterval < 100 ms, or an excluded path that is not
     * absolute or not under root); NotFound; PermissionDenied; Cancelled (token or reactor); IoError.
     */
    [[nodiscard]] virtual sched::Task<Result<std::unique_ptr<IDirectoryWatch>>>
    watch(std::string root, WatchOptions options, std::stop_token token = {}) = 0;

protected:
    constexpr IFileWatcher() noexcept = default;
    constexpr IFileWatcher(const IFileWatcher&) noexcept = default;
    constexpr IFileWatcher& operator=(const IFileWatcher&) noexcept = default;
    ~IFileWatcher() = default;
};

/*
 * The real watcher. Windows: ReadDirectoryChangesExW, then plain ReadDirectoryChangesW, then Polling.
 * Linux: one inotify instance per RealFileWatcher shared by its watches, or Polling when forced, when an
 * inotify limit is hit, or when statfs of the root reports a network or virtual file system (9P, NFS,
 * SMB/CIFS, FUSE); network mounts below the root are not detected. Lifetime: the Reactor and the
 * scheduler outlive the watcher, and the watcher outlives its watches.
 */
class RealFileWatcher final : public IFileWatcher {
public:
    // Errors: InvalidArgument (scheduler.callerDriven(): a SimExecutor cannot take wakeups from reactor
    // threads; use MemFsWatcher), Cancelled (the reactor is shut down).
    [[nodiscard]] static Result<std::unique_ptr<RealFileWatcher>> create(Reactor& reactor,
                                                                         sched::IScheduler& scheduler);
    // Precondition: no live watch and no pending watch() task (checked).
    ~RealFileWatcher();
    RealFileWatcher(const RealFileWatcher&) = delete ("watches refer to their watcher");
    RealFileWatcher& operator=(const RealFileWatcher&) = delete ("watches refer to their watcher");

    // See IFileWatcher::watch. Hops to Lane::Background first, so blocking calls (CreateFileW, statfs, the
    // inotify walk) never run on the caller's thread; the caller continues on Background. Budget: B21.
    sched::Task<Result<std::unique_ptr<IDirectoryWatch>>> watch(std::string root, WatchOptions options,
                                                                std::stop_token token = {}) override;

    struct Impl; // src/io/src: frost::io internals only.

private:
    explicit RealFileWatcher(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> m_impl; // Shared with every watch state (the Linux inotify instance lives here).
};

} // namespace frost::io
