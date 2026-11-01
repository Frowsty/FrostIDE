// SPDX-License-Identifier: Apache-2.0
//
// The Linux backend of RealFileWatcher (ADR 0020): one inotify instance per RealFileWatcher, read on the
// reactor (epoll) and shared by its watches; polling when forced, on network file systems and when an
// inotify limit is hit.
//
/*
 * Watch descriptors. inotify watches directories, not trees, so every directory below a recursive root
 * gets its own watch descriptor (wd). The wd table (wd -> path and owning watches, path -> wd) is shared
 * by the instance's watches under one mutex: the reactor strand reads events and renames paths, while
 * set-up walks run on Lane::Background. Overlapping watches share wds (inotify returns the same wd for
 * the same directory); a wd is removed when its last owner stops.
 *
 * Set-up. watch() walks the root on Background, adding a wd to each directory before listing it, and
 * yields every 64 directories; it completes when every directory has its wd. A directory that appears
 * later (IN_CREATE or IN_MOVED_TO of a directory from outside the tree) gets a set-up task of its own
 * that adds the wds and reports Created for every entry it lists, since events inside the directory
 * before its wd existed were never queued. status().pendingSetup counts those tasks.
 *
 * Events. IN_CREATE, IN_DELETE, IN_MODIFY, IN_ATTRIB, IN_MOVED_FROM and IN_MOVED_TO of a directory's
 * entries become raw events (IN_ISDIR gives directories; created and moved-in entries are lstat'ed for
 * their exact type). A paired directory move renames the paths of its wds; a directory IN_MOVED_FROM
 * whose IN_MOVED_TO has not arrived 20 ms later moved out of the tree, and its wds are removed.
 * IN_DELETE_SELF, IN_MOVE_SELF or IN_UNMOUNT of a watch's root ends that watch with Removed(root), then
 * NotFound. IN_Q_OVERFLOW becomes Rescan(root) for every watch.
 *
 * Limits. ENOSPC or ENOMEM from inotify_add_watch (fs.inotify.max_user_watches) and EMFILE or ENFILE
 * from inotify_init1 (fs.inotify.max_user_instances, the descriptor limit) switch the watch to polling
 * with the limit in status().reasonValue; mid-life the watch reports Rescan(root) first. statfs of the
 * root selects polling for 9P (WSL's /mnt drives), NFS, SMB/CIFS and FUSE, whose changes made by other
 * machines or the host inotify does not see.
 */

#include "frost/core/assert.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/path.hpp"

#include "internal.hpp"
#include "real_file_watcher_impl.hpp" // asio first.
#include "watcher_testing.hpp"
#include <asio/posix/stream_descriptor.hpp>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace frost::io {
namespace {

// Test hooks (watcher_testing.hpp). Relaxed: tests set them before starting the watch they target.
constinit std::atomic<std::size_t> g_watchLimit{0};
constinit std::atomic<bool> g_instanceLimit{false};

} // namespace

void testing::setInotifyWatchLimitForTesting(std::size_t limit) noexcept {
    g_watchLimit.store(limit, std::memory_order_relaxed);
}

void testing::setInotifyInstanceLimitForTesting(bool fail) noexcept {
    g_instanceLimit.store(fail, std::memory_order_relaxed);
}

namespace detail {
namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t kMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO |
                                IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW | IN_EXCL_UNLINK;
constexpr auto kUnpairedMoveDelay = 20ms;
constexpr std::size_t kReadBuffer = 64 * 1024;

// statfs f_type values of file systems inotify cannot watch for changes made elsewhere (linux/magic.h).
struct NetworkMagic {
    std::uint64_t magic;
    const char* name;
};
constexpr std::array<NetworkMagic, 6> kNetworkMagics{{
    {0x01021997, "9p (v9fs)"},
    {0x6969, "NFS"},
    {0x517B, "SMB"},
    {0xFF534D42, "CIFS"},
    {0xFE534D42, "SMB2"},
    {0x65735546, "FUSE"},
}};

// A /proc/sys value, or 0.
[[nodiscard]] std::uint64_t procValue(const char* path) {
    std::ifstream in(path);
    std::uint64_t value = 0;
    in >> value;
    return in ? value : 0;
}

[[nodiscard]] std::string join(const std::string& directory, std::string_view name) {
    std::string path = directory;
    if (path.back() != '/') {
        path += '/';
    }
    path += name;
    return path;
}

// The exact type of `path` without following links, or nullopt if it is gone.
[[nodiscard]] std::optional<FileType> lstatType(const std::string& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        return std::nullopt;
    }
    if (S_ISDIR(st.st_mode)) {
        return FileType::Directory;
    }
    if (S_ISREG(st.st_mode)) {
        return FileType::Regular;
    }
    return S_ISLNK(st.st_mode) ? FileType::Symlink : FileType::Other;
}

} // namespace

// LinuxWatch and InotifyState live in detail itself: RealFileWatcher::Impl holds the instance.
// One watch of the instance. Owned by its RealDirectoryWatch (as the producer) and, until it stops, by
// the instance's registry; requestStop() removes it there, which breaks the cycle.
class LinuxWatch final : public WatchProducer, public std::enable_shared_from_this<LinuxWatch> {
public:
    LinuxWatch(std::uint64_t watchId, std::shared_ptr<RealFileWatcher::Impl> owner, std::shared_ptr<WatchQueue> queue,
               std::shared_ptr<InotifyState> instance)
        : id(watchId), m_owner(std::move(owner)), m_queue(std::move(queue)), m_instance(std::move(instance)) {}

    void requestStop() noexcept override;

    // Ends this watch: Removed(root), then NotFound.
    void rootGone();
    // Switches to polling (limit hit after start-up).
    void degrade(DegradeReason reason, std::uint64_t value, std::string detail);
    void updateCounts();

    const std::uint64_t id;
    std::shared_ptr<RealFileWatcher::Impl> m_owner;
    std::shared_ptr<WatchQueue> m_queue;
    std::shared_ptr<InotifyState> m_instance;
    std::atomic<bool> m_stopped{false};
    std::atomic<std::int64_t> m_pendingSetup{0};
    std::atomic<std::size_t> m_osWatches{0};
    std::atomic<std::size_t> m_unreadable{0};
    Mutex m_fallbackMutex;
    std::shared_ptr<WatchProducer> m_fallback FROST_GUARDED_BY(m_fallbackMutex);
};

// The outcome of adding one wd.
enum class AddOutcome : std::uint8_t { Added, Vanished, Unreadable, Limit, Closed };

class InotifyState final : public BoundState {
public:
    InotifyState(Reactor::Impl& reactor, int fd)
        : BoundState(reactor), m_fd(fd), m_descriptor(strand()), m_moveTimer(strand()) {
        m_buffer.resize(kReadBuffer);
    }

    void shutdownOnStrand() noexcept override {
        std::map<std::uint64_t, std::shared_ptr<LinuxWatch>> watches;
        {
            LockGuard lock(m_mutex);
            m_closed = true;
            watches.swap(m_watches);
            m_wds.clear();
            m_byPath.clear();
        }
        m_moveTimer.cancel();
        asio::error_code ignored;
        static_cast<void>(m_descriptor.close(ignored)); // Drops every wd.
        for (auto& [id, watch] : watches) {
            watch->m_queue->close(Error(ErrorCode::Cancelled, "the reactor is shut down"), /*deliverPending=*/false);
        }
    }

    // Registers the descriptor with epoll; called once, before the state is published. Closes the
    // descriptor on failure. Errors: IoError.
    [[nodiscard]] Result<void> assign() {
        asio::error_code ec;
        static_cast<void>(m_descriptor.assign(m_fd, ec));
        if (ec) {
            ::close(m_fd);
            return fail(ErrorCode::IoError, std::format("inotify: cannot register the descriptor: {}", ec.message()));
        }
        return {};
    }

    // Starts reading (on the strand).
    [[nodiscard]] Result<void> startOnStrand() {
        if (!m_descriptor.is_open()) {
            return fail(ErrorCode::Cancelled, "the reactor is shut down");
        }
        armRead();
        return {};
    }

    // Set by the first watch, which starts the reader.
    std::atomic<bool> m_reading{false};

    // Registers a new watch; false once the instance is shut down.
    [[nodiscard]] bool registerWatch(const std::shared_ptr<LinuxWatch>& watch) {
        LockGuard lock(m_mutex);
        if (m_closed) {
            return false;
        }
        m_watches.emplace(watch->id, watch);
        return true;
    }

    [[nodiscard]] std::uint64_t nextWatchId() {
        LockGuard lock(m_mutex);
        return ++m_nextId;
    }

    // Removes `id` from the registry and drops the wds only it owned (any thread; O(wds)).
    void unregisterWatch(std::uint64_t id) {
        std::shared_ptr<LinuxWatch> keep; // Destroyed outside the lock.
        LockGuard lock(m_mutex);
        if (auto it = m_watches.find(id); it != m_watches.end()) {
            keep = std::move(it->second);
            m_watches.erase(it);
        }
        for (auto it = m_wds.begin(); it != m_wds.end();) {
            it->second.owners.erase(id);
            if (it->second.owners.empty()) {
                static_cast<void>(::inotify_rm_watch(m_fd, it->first));
                m_byPath.erase(it->second.path);
                it = m_wds.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Adds a wd for `directory` owned by `watch` (any thread).
    [[nodiscard]] AddOutcome addWatch(LinuxWatch& watch, const std::string& directory, int& error) {
        LockGuard lock(m_mutex);
        if (m_closed || watch.m_stopped.load(std::memory_order_relaxed)) {
            return AddOutcome::Closed;
        }
        const std::size_t limit = g_watchLimit.load(std::memory_order_relaxed);
        int wd = -1;
        if (limit != 0 && m_wds.size() >= limit) {
            errno = ENOSPC;
        } else {
            wd = ::inotify_add_watch(m_fd, directory.c_str(), kMask);
        }
        if (wd < 0) {
            error = errno;
            switch (error) {
            case ENOENT:
            case ENOTDIR: return AddOutcome::Vanished;
            case EACCES:
            case EPERM: return AddOutcome::Unreadable;
            case ENOSPC:
            case ENOMEM: return AddOutcome::Limit;
            default: return AddOutcome::Unreadable;
            }
        }
        Wd& entry = m_wds[wd];
        if (!entry.path.empty() && entry.path != directory) {
            m_byPath.erase(entry.path); // The same directory under a newer path.
        }
        entry.path = directory;
        if (entry.owners.insert(watch.id).second) {
            watch.m_osWatches.fetch_add(1, std::memory_order_relaxed);
        }
        m_byPath[directory] = wd;
        return AddOutcome::Added;
    }

    [[nodiscard]] std::shared_ptr<LinuxWatch> findWatch(std::uint64_t id) {
        LockGuard lock(m_mutex);
        const auto it = m_watches.find(id);
        return it == m_watches.end() ? nullptr : it->second;
    }

private:
    struct Wd {
        std::string path;
        std::set<std::uint64_t> owners;
    };
    struct PendingMove {
        std::string path;
        std::chrono::steady_clock::time_point at;
    };

    void armRead() {
        m_descriptor.async_wait(asio::posix::stream_descriptor::wait_read,
                                bindHandler([this](const asio::error_code& ec) { onReadable(ec); }));
    }

    void onReadable(const asio::error_code& ec) {
        if (ec) {
            return; // Closed (shutdown).
        }
        for (;;) {
            const ssize_t got = ::read(m_fd, m_buffer.data(), m_buffer.size());
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break; // EAGAIN: drained.
            }
            if (got == 0) {
                break;
            }
            process(static_cast<std::size_t>(got));
        }
        armRead();
    }

    // Converts one read's events and dispatches them to every watch.
    void process(std::size_t bytes) {
        std::vector<RawEvent> events;
        std::vector<std::shared_ptr<LinuxWatch>> watches;
        std::vector<std::shared_ptr<LinuxWatch>> rootsGone;
        std::vector<std::string> newDirectories;
        bool overflow = false;
        {
            LockGuard lock(m_mutex);
            std::size_t offset = 0;
            while (offset + sizeof(inotify_event) <= bytes) {
                inotify_event header{};
                std::memcpy(&header, m_buffer.data() + offset, sizeof(header));
                const char* rawName = m_buffer.data() + offset + sizeof(inotify_event);
                offset += sizeof(inotify_event) + header.len;
                if ((header.mask & IN_Q_OVERFLOW) != 0) {
                    overflow = true;
                    continue;
                }
                const auto wdIt = m_wds.find(header.wd);
                if (wdIt == m_wds.end()) {
                    continue; // A wd removed since (moved out, unregistered).
                }
                if ((header.mask & IN_IGNORED) != 0) {
                    m_byPath.erase(wdIt->second.path);
                    m_wds.erase(wdIt);
                    continue;
                }
                const std::string directory = wdIt->second.path;
                if ((header.mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) != 0) {
                    for (auto& [id, watch] : m_watches) {
                        if (watch->m_queue->root() == directory) {
                            rootsGone.push_back(watch);
                        }
                    }
                    continue;
                }
                const std::string_view name(rawName, ::strnlen(rawName, header.len));
                if (name.empty()) {
                    continue; // An event of the directory itself (IN_ATTRIB): reported by its parent.
                }
                std::string path = join(directory, name);
                const bool isDirectory = (header.mask & IN_ISDIR) != 0;
                const std::optional<FileType> knownType =
                    isDirectory ? std::optional<FileType>(FileType::Directory) : std::nullopt;
                if ((header.mask & IN_CREATE) != 0) {
                    std::optional<FileType> type = lstatType(path);
                    if (!type) {
                        type = knownType;
                    }
                    events.push_back({RawKind::Created, path, type, !isDirectory, 0});
                    if (isDirectory) {
                        newDirectories.push_back(path);
                    }
                } else if ((header.mask & IN_DELETE) != 0) {
                    events.push_back({RawKind::Removed, path, knownType, !isDirectory, 0});
                } else if ((header.mask & (IN_MODIFY | IN_ATTRIB)) != 0) {
                    events.push_back({RawKind::Modified, path, knownType, !isDirectory, 0});
                } else if ((header.mask & IN_MOVED_FROM) != 0) {
                    events.push_back({RawKind::MovedFrom, path, knownType, !isDirectory, header.cookie});
                    if (isDirectory && header.cookie != 0) {
                        m_pendingMoves[header.cookie] = PendingMove{path, std::chrono::steady_clock::now()};
                    }
                } else if ((header.mask & IN_MOVED_TO) != 0) {
                    std::optional<FileType> type = lstatType(path);
                    if (!type) {
                        type = knownType;
                    }
                    events.push_back({RawKind::MovedTo, path, type, !isDirectory, header.cookie});
                    if (isDirectory) {
                        const auto pending = m_pendingMoves.find(header.cookie);
                        if (header.cookie != 0 && pending != m_pendingMoves.end()) {
                            renameLocked(pending->second.path, path);
                            m_pendingMoves.erase(pending);
                        } else {
                            newDirectories.push_back(path);
                        }
                    }
                }
            }
            for (auto& [id, watch] : m_watches) {
                watches.push_back(watch);
            }
        }
        for (const std::shared_ptr<LinuxWatch>& watch : watches) {
            if (overflow) {
                watch->m_queue->pushOverflow(watch->m_queue->root());
            }
            if (!events.empty()) {
                static_cast<void>(watch->m_queue->push(events));
            }
        }
        for (const std::shared_ptr<LinuxWatch>& watch : rootsGone) {
            watch->rootGone();
        }
        for (const std::string& directory : newDirectories) {
            for (const std::shared_ptr<LinuxWatch>& watch : watches) {
                if (watch->m_queue->config().options.recursive && watch->m_queue->accepts(directory) &&
                    isStrictlyBelow(directory, watch->m_queue->root(), '/') &&
                    !watch->m_queue->excludedBelow(join(directory, "x"))) {
                    startSetup(watch, directory);
                }
            }
        }
        if (!m_pendingMoves.empty()) {
            armMoveTimer();
        }
    }

    // A paired directory move: every wd at or below `from` now lives below `to`.
    void renameLocked(const std::string& from, const std::string& to) FROST_REQUIRES(m_mutex) {
        std::vector<std::pair<std::string, int>> moved;
        for (auto it = m_byPath.lower_bound(from); it != m_byPath.end() && isAtOrBelow(it->first, from, '/');) {
            moved.emplace_back(it->first, it->second);
            it = m_byPath.erase(it);
        }
        for (auto& [path, wd] : moved) {
            std::string renamed = to + path.substr(from.size());
            m_wds[wd].path = renamed;
            m_byPath[std::move(renamed)] = wd;
        }
    }

    // Arms the unpaired-move timer unless it is running (re-arming would postpone it in an event storm).
    void armMoveTimer() {
        if (m_moveTimerArmed) {
            return;
        }
        m_moveTimerArmed = true;
        m_moveTimer.expires_after(kUnpairedMoveDelay);
        m_moveTimer.async_wait(bindHandler([this](const asio::error_code& ec) {
            m_moveTimerArmed = false;
            if (ec) {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            LockGuard lock(m_mutex);
            for (auto it = m_pendingMoves.begin(); it != m_pendingMoves.end();) {
                if (now - it->second.at < kUnpairedMoveDelay) {
                    ++it;
                    continue;
                }
                // Moved out of every watched tree: its wds report under stale paths, so drop them.
                for (auto path = m_byPath.lower_bound(it->second.path);
                     path != m_byPath.end() && isAtOrBelow(path->first, it->second.path, '/');) {
                    static_cast<void>(::inotify_rm_watch(m_fd, path->second));
                    m_wds.erase(path->second);
                    path = m_byPath.erase(path);
                }
                it = m_pendingMoves.erase(it);
            }
            if (!m_pendingMoves.empty()) {
                armMoveTimer();
            }
        }));
    }

    void startSetup(const std::shared_ptr<LinuxWatch>& watch, const std::string& directory);

    const int m_fd;
    asio::posix::stream_descriptor m_descriptor;
    asio::steady_timer m_moveTimer;
    std::vector<char> m_buffer;                          // Strand only.
    std::map<std::uint32_t, PendingMove> m_pendingMoves; // Strand only.
    bool m_moveTimerArmed = false;                       // Strand only.

    Mutex m_mutex;
    bool m_closed FROST_GUARDED_BY(m_mutex) = false;
    std::uint64_t m_nextId FROST_GUARDED_BY(m_mutex) = 0;
    std::unordered_map<int, Wd> m_wds FROST_GUARDED_BY(m_mutex);
    std::map<std::string, int, std::less<>> m_byPath FROST_GUARDED_BY(m_mutex);
    std::map<std::uint64_t, std::shared_ptr<LinuxWatch>> m_watches FROST_GUARDED_BY(m_mutex);
};

namespace {

struct SetupResult {
    AddOutcome limit = AddOutcome::Added; // Limit when the walk stopped at an inotify limit.
    int error = 0;
};

/*
 * Adds wds to `start` and the directories below it (Background; yields every 64 directories). With
 * `reportEntries`, pushes Created for every entry listed (a directory that appeared after start-up).
 * Errors: Cancelled; the root's own failure when `start` is the watch root.
 */
sched::Task<Result<SetupResult>> walk(std::shared_ptr<LinuxWatch> watch, std::string start, bool reportEntries,
                                      std::stop_token token) {
    sched::IScheduler& scheduler = watch->m_owner->scheduler;
    WatchQueue& queue = *watch->m_queue;
    const bool recursive = queue.config().options.recursive;
    std::vector<std::string> pending{start};
    std::vector<RawEvent> created;
    std::size_t walked = 0;
    while (!pending.empty()) {
        if (++walked % 64 == 0) {
            if (!created.empty()) {
                static_cast<void>(queue.push(created));
                created.clear();
            }
            co_await scheduler.schedule(sched::Lane::Background);
        }
        if (token.stop_requested() || watch->m_stopped.load(std::memory_order_relaxed)) {
            co_return fail(ErrorCode::Cancelled, "the watch stopped");
        }
        std::string directory = std::move(pending.back());
        pending.pop_back();
        int error = 0;
        switch (watch->m_instance->addWatch(*watch, directory, error)) {
        case AddOutcome::Added: break;
        case AddOutcome::Closed: co_return fail(ErrorCode::Cancelled, "the reactor is shut down");
        case AddOutcome::Limit: co_return SetupResult{AddOutcome::Limit, error};
        case AddOutcome::Vanished:
            if (directory == queue.root()) {
                co_return std::unexpected(errnoError(error, "watch: inotify_add_watch", directory));
            }
            continue;
        case AddOutcome::Unreadable:
            if (directory == queue.root()) {
                co_return std::unexpected(errnoError(error, "watch: inotify_add_watch", directory));
            }
            watch->m_unreadable.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // Listed after the wd exists: whatever appears later is reported by inotify.
        auto entries = realFileSystem().listDirectory(directory);
        if (!entries) {
            if (entries.error().code() == ErrorCode::PermissionDenied) {
                watch->m_unreadable.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        for (const DirEntry& entry : *entries) {
            std::string path = join(directory, entry.name);
            if (reportEntries) {
                created.push_back({RawKind::Created, path, entry.type, false, 0});
            }
            if (recursive && entry.type == FileType::Directory && !queue.excludedBelow(join(path, "x"))) {
                pending.push_back(std::move(path));
            }
        }
    }
    if (!created.empty()) {
        static_cast<void>(queue.push(created));
    }
    co_return SetupResult{};
}

// Watches a directory that appeared after start-up (spawned on Background by the reader).
sched::Task<void> setupNewDirectory(std::shared_ptr<LinuxWatch> watch, std::string directory) {
    sched::IScheduler& scheduler = watch->m_owner->scheduler;
    auto result = co_await walk(watch, std::move(directory), /*reportEntries=*/true, scheduler.stopToken());
    watch->m_pendingSetup.fetch_sub(1, std::memory_order_relaxed);
    if (result && result->limit == AddOutcome::Limit) {
        const DegradeReason reason = DegradeReason::WatchLimit;
        watch->degrade(reason, procValue("/proc/sys/fs/inotify/max_user_watches"),
                       std::format("inotify watch limit reached ({})", std::strerror(result->error)));
    }
    watch->updateCounts();
}

} // namespace

void InotifyState::startSetup(const std::shared_ptr<LinuxWatch>& watch, const std::string& directory) {
    watch->m_pendingSetup.fetch_add(1, std::memory_order_relaxed);
    watch->updateCounts();
    watch->m_owner->scheduler.spawn(sched::Lane::Background, setupNewDirectory(watch, directory)).detach();
}

void LinuxWatch::requestStop() noexcept {
    if (m_stopped.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    {
        LockGuard lock(m_fallbackMutex);
        if (m_fallback) {
            m_fallback->requestStop();
        }
    }
    // inotify_rm_watch for each wd could take a while on a big tree: do it on the instance's strand.
    std::shared_ptr<InotifyState> instance = m_instance;
    const std::uint64_t watchId = id;
    if (!instance->post([instance, watchId] { instance->unregisterWatch(watchId); })) {
        // Shutting down: shutdownOnStrand drops every wd and the registry.
    }
}

void LinuxWatch::rootGone() {
    const std::string& root = m_queue->root();
    static_cast<void>(m_queue->push(RawEvent{RawKind::Removed, root, FileType::Directory, false, 0}));
    m_queue->close(Error(ErrorCode::NotFound, std::format("watch '{}': the root was removed or moved", root)),
                   /*deliverPending=*/true);
    requestStop();
}

void LinuxWatch::degrade(DegradeReason reason, std::uint64_t value, std::string detail) {
    if (m_stopped.load(std::memory_order_relaxed)) {
        return;
    }
    m_queue->setMode(WatchMode::Polling, reason, value, std::move(detail));
    m_queue->pushOverflow(m_queue->root());
    // Drop the wds (the queue keeps going, fed by the polling task).
    std::shared_ptr<InotifyState> instance = m_instance;
    const std::uint64_t watchId = id;
    std::shared_ptr<LinuxWatch> self = shared_from_this();
    static_cast<void>(instance->post([instance, watchId, self] {
        instance->unregisterWatch(watchId);
        self->m_osWatches.store(0, std::memory_order_relaxed);
        self->updateCounts();
    }));
    NativeOutcome polling = startPollingFromStrand(m_owner, m_queue);
    LockGuard lock(m_fallbackMutex);
    m_fallback = std::move(polling.producer);
}

void LinuxWatch::updateCounts() {
    const auto pending =
        static_cast<std::size_t>(std::max<std::int64_t>(0, m_pendingSetup.load(std::memory_order_relaxed)));
    m_queue->updateStatus([&](WatchStatus& status) {
        status.osWatches = m_osWatches.load(std::memory_order_relaxed);
        status.pendingSetup = pending;
        status.unreadableDirectories = m_unreadable.load(std::memory_order_relaxed);
    });
}

namespace {

// The instance of `owner`, created by the first native watch. A value without instance: the reason to poll.
[[nodiscard]] Result<std::variant<std::shared_ptr<InotifyState>, DegradeTo>> instanceFor(RealFileWatcher::Impl& owner) {
    LockGuard lock(owner.inotifyMutex);
    if (owner.inotify) {
        return owner.inotify;
    }
    int fd = -1;
    if (g_instanceLimit.load(std::memory_order_relaxed)) {
        errno = EMFILE;
    } else {
        fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    }
    if (fd < 0) {
        const int error = errno;
        if (error == EMFILE || error == ENFILE || error == ENOMEM) {
            return DegradeTo{DegradeReason::InstanceLimit, procValue("/proc/sys/fs/inotify/max_user_instances"),
                             std::format("inotify instance limit reached ({})", std::strerror(error))};
        }
        return std::unexpected(errnoError(error, "watch: inotify_init1"));
    }
    auto bound = makeBound<InotifyState>(owner.reactor, fd);
    if (!bound) {
        ::close(fd);
        return std::unexpected(std::move(bound.error()));
    }
    if (auto assigned = (*bound)->assign(); !assigned) {
        return std::unexpected(std::move(assigned.error()));
    }
    owner.inotify = std::move(*bound);
    return owner.inotify;
}

} // namespace

sched::Task<Result<NativeOutcome>> startNativeWatch(std::shared_ptr<RealFileWatcher::Impl> owner,
                                                    std::shared_ptr<WatchQueue> queue, std::stop_token token) {
    if (token.stop_requested()) {
        co_return fail(ErrorCode::Cancelled, "watch() was cancelled");
    }
    struct statfs fs{};
    if (::statfs(queue->root().c_str(), &fs) == 0) {
        const auto magic = static_cast<std::uint64_t>(static_cast<std::uint32_t>(fs.f_type));
        for (const NetworkMagic& entry : kNetworkMagics) {
            if (entry.magic == magic) {
                co_return NativeOutcome{
                    nullptr, DegradeTo{DegradeReason::NetworkFileSystem, magic,
                                       std::format("{} file system: inotify misses remote changes", entry.name)}};
            }
        }
    }
    auto instance = instanceFor(*owner);
    if (!instance) {
        co_return std::unexpected(std::move(instance.error()));
    }
    if (auto* degrade = std::get_if<DegradeTo>(&*instance)) {
        co_return NativeOutcome{nullptr, std::move(*degrade)};
    }
    std::shared_ptr<InotifyState> state = std::get<std::shared_ptr<InotifyState>>(std::move(*instance));
    // The first watch starts the reader; later ones may add wds before it runs (the kernel queues events).
    {
        if (!state->m_reading.exchange(true, std::memory_order_relaxed)) {
            auto start = [&]() -> Result<void> { return state->startOnStrand(); };
            Result<void> started = co_await RunOnStrand<decltype(start)>(*state, owner->scheduler, start);
            if (!started) {
                co_return std::unexpected(std::move(started.error()));
            }
        }
    }
    auto watch = std::make_shared<LinuxWatch>(state->nextWatchId(), owner, queue, state);
    if (!state->registerWatch(watch)) {
        co_return fail(ErrorCode::Cancelled, "the reactor is shut down");
    }
    auto walked = co_await walk(watch, queue->root(), /*reportEntries=*/false, token);
    if (!walked || walked->limit == AddOutcome::Limit) {
        watch->requestStop();
        if (!walked) {
            co_return std::unexpected(std::move(walked.error()));
        }
        co_return NativeOutcome{
            nullptr, DegradeTo{DegradeReason::WatchLimit, procValue("/proc/sys/fs/inotify/max_user_watches"),
                               std::format("inotify watch limit reached ({})", std::strerror(walked->error))}};
    }
    watch->updateCounts();
    co_return NativeOutcome{std::move(watch), {}};
}

} // namespace detail
} // namespace frost::io
