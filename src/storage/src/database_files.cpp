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
    VfsFile(sqlite3_vfs* vfs) : m_vfs(vfs) {
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
    std::string path;   // Normalised, for messages and io::status.
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

Error vfsError(int rc, sqlite3_vfs* vfs, std::string_view operation, std::string_view path) {
    const int system = vfs->xGetLastError != nullptr ? vfs->xGetLastError(vfs, 0, nullptr) : 0;
    return makeSqliteError(rc, sqlite3_errstr(rc), system, operation, path);
}

/*
 * Opens the main file and takes SHARED, RESERVED and EXCLUSIVE, retrying on the busy_wait.hpp schedule
 * until `timeout`. Busy: some connection, in this process or another, has the database open
 * (WAL connections hold SHARED until they close; the unix VFS also sees in-process holders).
 */
Result<void> lockExclusive(VfsFile& file, const VfsNames& names, const Target& target, IClock::Duration timeout) {
    if (const int rc = file.open(names.main(), SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_READWRITE); rc != SQLITE_OK) {
        return std::unexpected(vfsError(rc, target.vfs, "open for locking", target.path));
    }
    const IClock::SteadyTime deadline = systemClock().steadyNow() + timeout;
    for (std::size_t attempt = 0;; ++attempt) {
        int rc = file.io().xLock(file.file(), SQLITE_LOCK_SHARED);
        if (rc == SQLITE_OK) {
            rc = file.io().xLock(file.file(), SQLITE_LOCK_RESERVED);
        }
        if (rc == SQLITE_OK) {
            rc = file.io().xLock(file.file(), SQLITE_LOCK_EXCLUSIVE);
        }
        if (rc == SQLITE_OK) {
            return {};
        }
        static_cast<void>(file.io().xUnlock(file.file(), SQLITE_LOCK_NONE));
        if ((rc & 0xff) != SQLITE_BUSY) {
            return std::unexpected(vfsError(rc, target.vfs, "lock", target.path));
        }
        if (!sleepBeforeRetry(attempt, deadline)) {
            return fail(ErrorCode::Busy,
                        std::format("lock '{}': the database is open in this or another process", target.path));
        }
    }
}

/*
 * Removes the files in the safe order: main, then -wal, -shm and -journal. On Windows the lock
 * file is closed first (SQLite opens files without FILE_SHARE_DELETE, so our own handle would
 * block the delete; a racing opener makes the delete fail with Busy, which is fail-safe). On
 * POSIX the lock is held across the unlinks.
 */
Result<void> removeInOrder(std::optional<VfsFile>& lock, const VfsNames& names, const Target& target) {
#ifdef _WIN32
    lock.reset();
#endif
    if (exists(target.path)) {
        const int rc = target.vfs->xDelete(target.vfs, names.main(), 1);
        if (rc != SQLITE_OK && rc != SQLITE_IOERR_DELETE_NOENT && exists(target.path)) {
            const int system =
                target.vfs->xGetLastError != nullptr ? target.vfs->xGetLastError(target.vfs, 0, nullptr) : 0;
#ifdef _WIN32
            const ErrorCode os =
                system != 0 ? io::errorCodeFromWin32(static_cast<std::uint32_t>(system)) : ErrorCode::Busy;
#else
            const ErrorCode os = system != 0 ? io::errorCodeFromErrno(system) : ErrorCode::Busy;
#endif
            const ErrorCode code = os == ErrorCode::PermissionDenied ? ErrorCode::PermissionDenied : ErrorCode::Busy;
            lock.reset();
            return fail(code, std::format("remove database '{}': the file could not be deleted (sqlite {} {})",
                                          target.path, rc, sqliteCodeName(rc)));
        }
    }
    // Harmless if they stay: SQLite deletes a -wal beside a missing database on the next open.
    for (const char* companion : {names.wal(), names.shm(), names.journal()}) {
        static_cast<void>(target.vfs->xDelete(target.vfs, companion, 0));
    }
    lock.reset();
    return {};
}

/*
 * Streams `from` (whose normalised path is `fromPath`) into a new file `to` (created
 * exclusively), then syncs it. O(64 KiB) memory. The size comes from io::status (a stat, safe
 * while the lock is held), not xFileSize: the unix VFS reports a 1-byte file as empty
 * (os_unix.c, ticket #3260), which would lose that byte.
 */
Result<void> copyFile(VfsFile& from, const std::string& fromPath, sqlite3_vfs* vfs, sqlite3_filename to, int type,
                      std::string_view toPath) {
    auto status = io::realFileSystem().status(fromPath);
    if (!status) {
        return std::unexpected(std::move(status.error()).withContext("size for quarantine"));
    }
    const auto size = static_cast<sqlite3_int64>(status->size);
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
    std::optional<VfsFile> lock;
    if (exists(target->path)) {
        lock.emplace(target->vfs);
        if (auto locked = lockExclusive(*lock, names, *target, lockTimeout); !locked) {
            if (!exists(target->path)) {
                lock.reset(); // Removed meanwhile: only companions can be left.
            } else {
                return std::unexpected(std::move(locked.error()));
            }
        }
    }
    return removeInOrder(lock, names, *target);
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
    std::optional<VfsFile> lock(std::in_place, target->vfs);
    if (auto locked = lockExclusive(*lock, names, *target, lockTimeout); !locked) {
        return std::unexpected(std::move(locked.error()).withContext("quarantine database"));
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
    VfsNames copyNames(*copyOs);
#else
    VfsNames copyNames(copyPath);
#endif
    if (!copyNames.valid()) {
        return fail(ErrorCode::IoError, "quarantine database: out of memory");
    }
    // The copies exist before anything is removed; any failure deletes them again.
    auto copied = copyFile(*lock, target->path, target->vfs, copyNames.main(), SQLITE_OPEN_MAIN_DB, copyPath);
    if (copied && exists(target->path + "-wal")) {
        VfsFile wal(target->vfs);
        if (const int rc = wal.open(names.wal(), SQLITE_OPEN_WAL | SQLITE_OPEN_READWRITE); rc != SQLITE_OK) {
            copied = std::unexpected(vfsError(rc, target->vfs, "open -wal for quarantine", target->path));
        } else {
            copied =
                copyFile(wal, target->path + "-wal", target->vfs, copyNames.wal(), SQLITE_OPEN_WAL, copyPath + "-wal");
        }
    }
    const auto deleteCopies = [&target, &copyNames] {
        for (const char* name : {copyNames.main(), copyNames.wal(), copyNames.shm(), copyNames.journal()}) {
            static_cast<void>(target->vfs->xDelete(target->vfs, name, 0));
        }
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
     */
    if (stillCorrupt) {
        const bool corrupt = stillCorrupt(copyPath);
        static_cast<void>(target->vfs->xDelete(target->vfs, copyNames.shm(), 0));
        if (!corrupt) {
            deleteCopies();
            return std::optional<std::string>{};
        }
    }
    if (auto removed = removeInOrder(lock, names, *target); !removed) {
        deleteCopies();
        return std::unexpected(std::move(removed.error()).withContext("quarantine database"));
    }
    return std::optional<std::string>{std::move(copyPath)};
}

} // namespace frost::storage::detail
