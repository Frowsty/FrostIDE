// SPDX-License-Identifier: Apache-2.0
//
// Private: lock, copy and removal of a database's files through SQLite's VFS (see
// database_files.hpp and the "Database files" rules in frost/storage/store.hpp).

#include "database_files.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/path.hpp"

#include "busy_wait.hpp"
#include "sqlite_error.hpp"
#include "sqlite_init.hpp"
#include "test_hooks.hpp"
#include "time_names.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <utility>

#include <sqlite3.h>

namespace frost::storage::detail {

namespace {

#ifdef _WIN32
constexpr const char* kDefaultVfs = "win32-longpath";
#else
constexpr const char* kDefaultVfs = nullptr;
#endif

// Copies stream through one buffer of this size. Units: bytes. The unix VFS asserts (in
// SQLITE_DEBUG builds) that one read is under 128 KiB (os_unix.c, seekAndRead).
constexpr int kCopyBlockBytes = 64 * 1024;

// "-journal", the longest suffix SQLite derives from a database name (Database::open's rule).
constexpr std::size_t kLongestSqliteSuffix = 8;

// removeFiles looks at the path again this often when the file it locked was replaced meanwhile.
constexpr int kRemoveRounds = 3;

/*
 * The names a VFS expects. xOpen walks the buffer past the name for URI parameters, so the
 * names come from sqlite3_create_filename (main, -journal and -wal in one SQLite-owned buffer)
 * and are freed with sqlite3_free_filename. The -shm name is only ever passed to xDelete,
 * which takes a plain string.
 */
class VfsNames {
public:
    explicit VfsNames(const std::string& osPath) {
        const std::string journal = osPath + "-journal";
        const std::string wal = osPath + "-wal";
        m_names = sqlite3_create_filename(osPath.c_str(), journal.c_str(), wal.c_str(), 0, nullptr);
        m_shm = osPath + "-shm";
    }
    ~VfsNames() { sqlite3_free_filename(m_names); }
    VfsNames(const VfsNames&) = delete ("owns an SQLite name buffer");
    VfsNames& operator=(const VfsNames&) = delete ("owns an SQLite name buffer");

    [[nodiscard]] bool valid() const noexcept { return m_names != nullptr; }
    [[nodiscard]] sqlite3_filename main() const noexcept { return m_names; }
    [[nodiscard]] const char* wal() const noexcept { return sqlite3_filename_wal(m_names); }
    [[nodiscard]] const char* journal() const noexcept { return sqlite3_filename_journal(m_names); }
    [[nodiscard]] const char* shm() const noexcept { return m_shm.c_str(); }

private:
    sqlite3_filename m_names = nullptr;
    std::string m_shm;
};

// One sqlite3_file opened through a VFS; closed (and so unlocked) on destruction.
class VfsFile {
public:
    explicit VfsFile(sqlite3_vfs* vfs) : m_vfs(vfs) {
        const auto words =
            (static_cast<std::size_t>(vfs->szOsFile) + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
        m_storage = std::make_unique<std::max_align_t[]>(std::max<std::size_t>(words, 1));
    }
    ~VfsFile() { close(); }
    VfsFile(const VfsFile&) = delete ("owns an open file");
    VfsFile& operator=(const VfsFile&) = delete ("owns an open file");

    // xOpen; on failure the file is closed again if the VFS left methods behind.
    [[nodiscard]] int open(sqlite3_filename name, int flags) noexcept {
        int outFlags = 0;
        const int rc = m_vfs->xOpen(m_vfs, name, file(), flags, &outFlags);
        m_open = file()->pMethods != nullptr;
        if (rc != SQLITE_OK) {
            close();
        }
        return rc;
    }

    void close() noexcept {
        if (m_open) {
            static_cast<void>(file()->pMethods->xClose(file()));
            m_open = false;
        }
    }

    [[nodiscard]] sqlite3_file* file() noexcept { return reinterpret_cast<sqlite3_file*>(m_storage.get()); }
    [[nodiscard]] const sqlite3_io_methods& io() noexcept { return *file()->pMethods; }

private:
    sqlite3_vfs* m_vfs;
    std::unique_ptr<std::max_align_t[]> m_storage;
    bool m_open = false;
};

// The VFS and the OS spelling of `path` (the \\?\ form on Windows).
struct Target {
    std::string path;   // Normalised, for messages, io::status and (Windows) io::removeFile.
    std::string osPath; // What the VFS sees.
    sqlite3_vfs* vfs = nullptr;
};

Result<Target> resolveTarget(std::string_view rawPath, std::string_view vfsName) {
    if (auto initialized = initializeSqlite(); !initialized) {
        return std::unexpected(std::move(initialized.error()));
    }
    auto normalized = io::normalizePath(rawPath);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    Target target;
    target.path = std::move(*normalized);
#ifdef _WIN32
    auto extended = io::extendedLengthPath(target.path);
    if (!extended) {
        return std::unexpected(std::move(extended.error()));
    }
    target.osPath = std::move(*extended);
#else
    target.osPath = target.path;
#endif
    const std::string vfs(vfsName);
    target.vfs = sqlite3_vfs_find(vfs.empty() ? kDefaultVfs : vfs.c_str());
    if (target.vfs == nullptr) {
        return fail(ErrorCode::InvalidArgument, std::format("unknown SQLite VFS '{}'", vfsName));
    }
    return target;
}

bool exists(const std::string& path) { return io::realFileSystem().status(path).has_value(); }

// Whether SQLite accepts `osPath` and every name it derives from it (Database::open's rule).
bool fitsVfs(std::size_t osPathBytes, const sqlite3_vfs* vfs) noexcept {
    return osPathBytes + kLongestSqliteSuffix < static_cast<std::size_t>(vfs->mxPathname);
}

Error vfsError(int rc, sqlite3_vfs* vfs, std::string_view operation, std::string_view path) {
    const int system = vfs->xGetLastError != nullptr ? vfs->xGetLastError(vfs, 0, nullptr) : 0;
    return makeSqliteError(rc, sqlite3_errstr(rc), system, operation, path);
}

/*
 * Opens the main file and takes SHARED, RESERVED and EXCLUSIVE, retrying on the busy_wait.hpp
 * schedule until `deadline` (one attempt once it has passed). Busy: some connection, in this
 * process or another, has the database open (WAL connections hold SHARED until they close; the
 * unix VFS also sees in-process holders).
 *
 * Returns true with the lock held on the file the path names, or false, with `file` closed,
 * when the path no longer names a file we could lock: it was missing at the open, or (POSIX) the
 * inode we opened was unlinked, and perhaps replaced by a new file, while we waited. A waiter
 * that only looked at its descriptor would lock the old inode and then judge, copy or remove
 * whatever the path names now, such as another instance's fresh store. SQLITE_FCNTL_HAS_MOVED
 * makes the unix VFS compare the descriptor's inode with stat(path) (os_unix.c, fileHasMoved);
 * every remover in this file takes the lock before it unlinks, so the answer holds until we
 * release the lock. The win32 VFS does not implement it (SQLITE_NOTFOUND) and needs no check:
 * SQLite opens files without FILE_SHARE_DELETE, so nobody can delete the file while our handle
 * is open.
 */
Result<bool> lockExclusive(VfsFile& file, const VfsNames& names, const Target& target, IClock::SteadyTime deadline) {
    if (const int rc = file.open(names.main(), SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_READWRITE); rc != SQLITE_OK) {
        if (!exists(target.path)) {
            return false;
        }
        return std::unexpected(vfsError(rc, target.vfs, "open for locking", target.path));
    }
    for (std::size_t attempt = 0;; ++attempt) {
        int rc = file.io().xLock(file.file(), SQLITE_LOCK_SHARED);
        if (rc == SQLITE_OK) {
            rc = file.io().xLock(file.file(), SQLITE_LOCK_RESERVED);
        }
        if (rc == SQLITE_OK) {
            rc = file.io().xLock(file.file(), SQLITE_LOCK_EXCLUSIVE);
        }
        if (rc == SQLITE_OK) {
            int moved = 0;
            if (file.io().xFileControl(file.file(), SQLITE_FCNTL_HAS_MOVED, &moved) == SQLITE_OK && moved != 0) {
                file.close();
                return false;
            }
            return true;
        }
        static_cast<void>(file.io().xUnlock(file.file(), SQLITE_LOCK_NONE));
        if ((rc & 0xff) != SQLITE_BUSY) {
            return std::unexpected(vfsError(rc, target.vfs, "lock", target.path));
        }
        g_vfsLockBusy.run();
        if (!sleepBeforeRetry(attempt, deadline)) {
            return fail(ErrorCode::Busy,
                        std::format("lock '{}': the database is open in this or another process", target.path));
        }
    }
}

#ifdef _WIN32
/*
 * Deletes `path` with frost::io's removeFile (one DeleteFileW per attempt), retrying Busy (a
 * handle without FILE_SHARE_DELETE: an opener racing the removal, a backup or virus scanner) and
 * PermissionDenied (for example a delete pending on another handle) on the busy_wait.hpp
 * schedule until `deadline`. The win32 VFS's xDelete is not used: it retries those failures on
 * its own for about 1.4 s (winRetryIoerr, 10 retries 25 to 250 ms apart), past every caller's
 * bound. Windows locks belong to handles, so a delete by name drops no lock of this process (the
 * POSIX rule of store.hpp does not apply). A missing file is success.
 */
Result<void> deleteWithRetry(const std::string& path, IClock::SteadyTime deadline) {
    for (std::size_t attempt = 0;; ++attempt) {
        auto removed = io::realFileSystem().removeFile(path);
        if (removed || removed.error().code() == ErrorCode::NotFound) {
            return {};
        }
        const ErrorCode code = removed.error().code();
        if ((code != ErrorCode::Busy && code != ErrorCode::PermissionDenied) || !sleepBeforeRetry(attempt, deadline)) {
            return std::unexpected(std::move(removed.error()));
        }
    }
}
#endif

/*
 * Deletes `path` (spelled `osName` for the VFS) and ignores failures: companions and copies. On
 * Windows it retries as deleteWithRetry does until `deadline` (a virus scanner may still hold a
 * copy that was just written; the win32 VFS's xDelete would have retried for about 1.4 s), and
 * makes one attempt once the deadline has passed. POSIX unlinks once.
 */
void deleteQuietly(const Target& target, const char* osName, const std::string& path, IClock::SteadyTime deadline) {
#ifdef _WIN32
    static_cast<void>(target);
    static_cast<void>(osName);
    static_cast<void>(deleteWithRetry(path, deadline));
#else
    static_cast<void>(path);
    static_cast<void>(deadline);
    static_cast<void>(target.vfs->xDelete(target.vfs, osName, 0));
#endif
}

/*
 * Removes the files in the safe order: main, then -wal, -shm and -journal. On Windows the lock
 * file is closed first (SQLite opens files without FILE_SHARE_DELETE, so our own handle would
 * block the delete; a racing opener makes the delete fail with Busy once `deadline` has passed,
 * which is fail-safe). On POSIX the lock is held across the unlinks.
 */
Result<void> removeInOrder(std::optional<VfsFile>& lock, const VfsNames& names, const Target& target,
                           IClock::SteadyTime deadline) {
#ifdef _WIN32
    lock.reset();
    if (auto removed = deleteWithRetry(target.path, deadline); !removed) {
        const ErrorCode code =
            removed.error().code() == ErrorCode::PermissionDenied ? ErrorCode::PermissionDenied : ErrorCode::Busy;
        return fail(code, std::format("remove database '{}': the file could not be deleted ({})", target.path,
                                      removed.error().message()));
    }
#else
    static_cast<void>(deadline);
    if (exists(target.path)) {
        const int rc = target.vfs->xDelete(target.vfs, names.main(), 1);
        if (rc != SQLITE_OK && rc != SQLITE_IOERR_DELETE_NOENT && exists(target.path)) {
            const int system =
                target.vfs->xGetLastError != nullptr ? target.vfs->xGetLastError(target.vfs, 0, nullptr) : 0;
            const ErrorCode os = system != 0 ? io::errorCodeFromErrno(system) : ErrorCode::Busy;
            const ErrorCode code = os == ErrorCode::PermissionDenied ? ErrorCode::PermissionDenied : ErrorCode::Busy;
            lock.reset();
            return fail(code, std::format("remove database '{}': the file could not be deleted (sqlite {} {})",
                                          target.path, rc, sqliteCodeName(rc)));
        }
    }
#endif
    // Harmless if they stay: SQLite deletes a -wal beside a missing database on the next open.
    deleteQuietly(target, names.wal(), target.path + "-wal", deadline);
    deleteQuietly(target, names.shm(), target.path + "-shm", deadline);
    deleteQuietly(target, names.journal(), target.path + "-journal", deadline);
    lock.reset();
    return {};
}

/*
 * The size of an open file, from its handle: under the lock the handle is the file being
 * quarantined, while the path is only a name. xFileSize, except that the unix VFS reports a
 * 1-byte file as empty (os_unix.c, ticket #3260), which would lose that byte: a 1-byte read at
 * offset 0 succeeds only when the byte exists.
 */
Result<sqlite3_int64> openFileSize(VfsFile& file, sqlite3_vfs* vfs, std::string_view path) {
    sqlite3_int64 size = 0;
    if (const int rc = file.io().xFileSize(file.file(), &size); rc != SQLITE_OK) {
        return std::unexpected(vfsError(rc, vfs, "size for quarantine", path));
    }
    if (size == 0) {
        char byte = 0;
        if (file.io().xRead(file.file(), &byte, 1, 0) == SQLITE_OK) {
            size = 1;
        }
    }
    return size;
}

// Streams the first `size` bytes of `from` into a new file `to` (created exclusively), then
// syncs it. O(64 KiB) memory.
Result<void> copyFile(VfsFile& from, sqlite3_int64 size, sqlite3_vfs* vfs, sqlite3_filename to, int type,
                      std::string_view toPath) {
    VfsFile out(vfs);
    if (const int rc = out.open(to, type | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE);
        rc != SQLITE_OK) {
        return std::unexpected(vfsError(rc, vfs, "create quarantine copy", toPath));
    }
    auto buffer = std::make_unique<char[]>(static_cast<std::size_t>(kCopyBlockBytes));
    for (sqlite3_int64 offset = 0; offset < size;) {
        const int amount = static_cast<int>(std::min<sqlite3_int64>(kCopyBlockBytes, size - offset));
        if (const int rc = from.io().xRead(from.file(), buffer.get(), amount, offset); rc != SQLITE_OK) {
            return std::unexpected(vfsError(rc, vfs, "read for quarantine", toPath));
        }
        if (const int rc = out.io().xWrite(out.file(), buffer.get(), amount, offset); rc != SQLITE_OK) {
            return std::unexpected(vfsError(rc, vfs, "write quarantine copy", toPath));
        }
        offset += amount;
    }
    if (const int rc = out.io().xSync(out.file(), SQLITE_SYNC_FULL); rc != SQLITE_OK) {
        return std::unexpected(vfsError(rc, vfs, "sync quarantine copy", toPath));
    }
    return {};
}

// Streams the whole open file `from` into `to`, sized from its handle (openFileSize).
Result<void> copyOpenFile(VfsFile& from, sqlite3_vfs* vfs, sqlite3_filename to, int type, std::string_view toPath) {
    auto size = openFileSize(from, vfs, toPath);
    if (!size) {
        return std::unexpected(std::move(size.error()));
    }
    return copyFile(from, *size, vfs, to, type, toPath);
}

} // namespace

Result<void> removeFiles(std::string_view path, IClock::Duration lockTimeout, std::string_view vfsName) {
    auto target = resolveTarget(path, vfsName);
    if (!target) {
        return std::unexpected(std::move(target.error()).withContext("remove database files"));
    }
    VfsNames names(target->osPath);
    if (!names.valid()) {
        return fail(ErrorCode::IoError, "remove database files: out of memory");
    }
    const IClock::SteadyTime deadline = systemClock().steadyNow() + lockTimeout;
    for (int round = 0; round < kRemoveRounds; ++round) {
        if (!exists(target->path)) {
            if (!exists(target->path + "-wal") && !exists(target->path + "-shm") &&
                !exists(target->path + "-journal")) {
                return {};
            }
            g_removeFilesMainMissing.run();
            /*
             * Stale companions of a missing main file. A creator makes the main file first and its
             * -wal and -shm only under its lock, so deleting them by name could hit a store that
             * was created this instant. Instead an empty main file is created exclusively (O_EXCL,
             * CREATE_NEW), which SQLite would treat as a fresh database, and removed with the
             * companions under its lock as below. When someone created the main file first, the
             * exclusive create fails and their files are handled like any existing database's
             * (Busy while open).
             */
            VfsFile placeholder(target->vfs);
            const int rc = placeholder.open(names.main(), SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_READWRITE |
                                                              SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE);
            placeholder.close();
            if (rc != SQLITE_OK && !exists(target->path)) {
                return std::unexpected(
                    vfsError(rc, target->vfs, "create a placeholder to remove stale files of", target->path));
            }
        }
        std::optional<VfsFile> lock(std::in_place, target->vfs);
        auto locked = lockExclusive(*lock, names, *target, deadline);
        if (!locked) {
            return std::unexpected(std::move(locked.error()));
        }
        if (*locked) {
            return removeInOrder(lock, names, *target, deadline);
        }
        // Removed, or replaced by a new file, while we waited for the lock: look again.
    }
    return fail(ErrorCode::Busy, std::format("remove database '{}': the file was replaced {} times during the removal",
                                             target->path, kRemoveRounds));
}

Result<void> checkQuarantineRoom(std::string_view path, std::string_view vfsName) {
    auto target = resolveTarget(path, vfsName);
    if (!target) {
        return std::unexpected(std::move(target.error()).withContext("open store"));
    }
    if (!fitsVfs(target->osPath.size() + kQuarantineSuffixBytes, target->vfs)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("open store '{}': its quarantine copy's name would be longer than the SQLite VFS "
                                "limit of {} bytes",
                                target->path, target->vfs->mxPathname));
    }
    return {};
}

Result<std::optional<std::string>> quarantineFiles(std::string_view path, IClock& clock, IClock::Duration lockTimeout,
                                                   std::string_view vfsName, const StillCorrupt& stillCorrupt) {
    auto target = resolveTarget(path, vfsName);
    if (!target) {
        return std::unexpected(std::move(target.error()).withContext("quarantine database"));
    }
    VfsNames names(target->osPath);
    if (!names.valid()) {
        return fail(ErrorCode::IoError, "quarantine database: out of memory");
    }
    const IClock::SteadyTime deadline = systemClock().steadyNow() + lockTimeout;
    std::optional<VfsFile> lock(std::in_place, target->vfs);
    auto locked = lockExclusive(*lock, names, *target, deadline);
    if (!locked) {
        return std::unexpected(std::move(locked.error()).withContext("quarantine database"));
    }
    if (!*locked) {
        return std::optional<std::string>{}; // Gone or replaced since the verdict: nothing judged is left.
    }

    // "<path>.corrupt-<stamp>[-n]", with n counting up while the name or its -wal is taken.
    const std::string base = std::format("{}.corrupt-{}", target->path, utcStamp(clock.systemNow(), false));
    std::string copyPath = base;
    for (int n = 1; exists(copyPath) || exists(copyPath + "-wal"); ++n) {
        copyPath = std::format("{}-{}", base, n);
    }
#ifdef _WIN32
    auto copyOs = io::extendedLengthPath(copyPath);
    if (!copyOs) {
        return std::unexpected(std::move(copyOs.error()));
    }
#else
    const std::optional<std::string> copyOs(copyPath);
#endif
    // The copy is opened (judged) like any database, so its names obey Database::open's limit.
    if (!fitsVfs(copyOs->size(), target->vfs)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("quarantine database '{}': the copy's name '{}' is longer than the SQLite VFS "
                                "limit of {} bytes",
                                target->path, copyPath, target->vfs->mxPathname));
    }
    VfsNames copyNames(*copyOs);
    if (!copyNames.valid()) {
        return fail(ErrorCode::IoError, "quarantine database: out of memory");
    }
    // The copies exist before anything is removed; any failure deletes them again.
    auto copied = copyOpenFile(*lock, target->vfs, copyNames.main(), SQLITE_OPEN_MAIN_DB, copyPath);
    if (copied && exists(target->path + "-wal")) {
        VfsFile wal(target->vfs);
        if (const int rc = wal.open(names.wal(), SQLITE_OPEN_WAL | SQLITE_OPEN_READWRITE); rc != SQLITE_OK) {
            copied = std::unexpected(vfsError(rc, target->vfs, "open -wal for quarantine", target->path));
        } else {
            copied = copyOpenFile(wal, target->vfs, copyNames.wal(), SQLITE_OPEN_WAL, copyPath + "-wal");
        }
    }
    const auto deleteCopies = [&target, &copyNames, &copyPath, deadline] {
        deleteQuietly(*target, copyNames.main(), copyPath, deadline);
        deleteQuietly(*target, copyNames.wal(), copyPath + "-wal", deadline);
        deleteQuietly(*target, copyNames.shm(), copyPath + "-shm", deadline);
        deleteQuietly(*target, copyNames.journal(), copyPath + "-journal", deadline);
    };
    if (!copied) {
        deleteCopies();
        return std::unexpected(std::move(copied.error()).withContext("quarantine database"));
    }
    /*
     * The verdict that brought us here was reached before the lock. In between, another process
     * may have quarantined the file itself and created a fresh store, which is unlocked until
     * its first read: removing that would leave its owner writing to an unlinked inode. The
     * copy is what the lock protects now, so it is judged again and kept only if still corrupt.
     * A judgement that itself fails (an I/O error on the copy) keeps the originals.
     */
    if (stillCorrupt) {
        auto corrupt = stillCorrupt(copyPath);
        deleteQuietly(*target, copyNames.shm(), copyPath + "-shm", deadline);
        if (!corrupt) {
            deleteCopies();
            return std::unexpected(std::move(corrupt.error()).withContext("quarantine database"));
        }
        if (!*corrupt) {
            deleteCopies();
            return std::optional<std::string>{};
        }
    }
    if (auto removed = removeInOrder(lock, names, *target, deadline); !removed) {
        deleteCopies();
        return std::unexpected(std::move(removed.error()).withContext("quarantine database"));
    }
    return std::optional<std::string>{std::move(copyPath)};
}

} // namespace frost::storage::detail
