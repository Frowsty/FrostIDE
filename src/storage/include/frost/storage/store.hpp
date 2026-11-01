// SPDX-License-Identifier: Apache-2.0
//
// Store files: opening a schema'd store with corrupt-file quarantine, and removing a cache
// database's files safely (ADR 0014).
//
/*
 * Database files are touched only through SQLite's VFS, never with io::readFile or
 * writeFileAtomic. On POSIX, advisory locks belong to the process and the inode: closing any
 * descriptor of a database file drops every fcntl lock the process holds on it, so reading a
 * database with a plain open/close would silently unlock a connection of this process ("How To
 * Corrupt An SQLite Database File", section 2.2). SQLite's VFS defers its own closes while locks
 * are held. io::status (a stat, no descriptor) is always safe. On Windows, locks belong to
 * handles, so files are deleted by name with io::removeFile (DeleteFileW) rather than the win32
 * VFS's xDelete, which retries a sharing violation for about 1.4 s on its own; our own retries
 * stay within the caller's time bound.
 *
 * Removal order is the main file, then -wal, then -shm: SQLite replays a self-consistent -wal
 * beside a non-empty database whatever database wrote it, but deletes a -wal beside a missing
 * or empty one on the first read, so removing the main file first is the safe order. A rollback
 * journal (-journal) goes last, for the same reason. Companions are removed only under the lock
 * of a main file: when it is missing, an empty placeholder is created exclusively first. This
 * narrows, but on POSIX does not close, the race with a process that creates a store at the same
 * path at that instant: one whose open lands on the placeholder, or whose -wal and -shm appear
 * during the main file's unlink and directory sync, can lose its new store (ADR 0014 decision 8,
 * residual POSIX race). On Windows such an open fails with Busy instead.
 *
 * Quarantine (CorruptPolicy::Quarantine): with SQLite's EXCLUSIVE lock held through the VFS, the
 * main file and its -wal are streamed in 64 KiB blocks into "<path>.corrupt-<UTC
 * yyyymmddTHHMMSSZ>[-n]" and "<same>-wal" (each synced, sized from the locked handles), the copy
 * is judged again (openStore below), then the originals are removed. The copies exist before
 * anything is removed, so a failure never loses data; when a later step fails the copies are
 * deleted again and the originals stay. A lock that was granted on a file the path no longer
 * names (POSIX: another instance removed it, and perhaps created a fresh store, while we waited;
 * SQLITE_FCNTL_HAS_MOVED tells) quarantines nothing. TODO(M3): count quarantined copies in the
 * cache budget and prune them (I11); until then they stay and are reported to the caller.
 *
 * Known limit (POSIX): a process that opened the file in the instant before the unlink keeps an
 * unlinked inode; the window is the duration of the unlink calls. On Windows SQLite opens files
 * without FILE_SHARE_DELETE, so a racing opener makes the removal fail with Busy instead.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/storage/database.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace frost::storage {

enum class CorruptPolicy : unsigned char { Fail, Quarantine };

// A damaged file moved aside: where the copy of the main file went, and why it was refused.
struct Quarantined {
    std::string movedTo;
    Error reason;
};

struct StoreOpenOutcome {
    Database database;
    std::optional<Quarantined> quarantined; // Set when a corrupt file was moved aside first.
};

/*
 * Database::open(path, options); then, on Corrupt only, with CorruptPolicy::Quarantine and a
 * read-write mode, quarantines the file (above) and opens a fresh store in its place. Unsupported,
 * Busy, PermissionDenied and IoError are never quarantined, so a newer FrostIDE's data is never
 * destroyed; a ReadOnly open returns Corrupt and removes nothing. The verdict comes before the
 * lock, so under the lock the copy is opened with the same options and the original is removed
 * only if the copy is still Corrupt; otherwise another process replaced the file meanwhile (it
 * quarantined it first and created a fresh store), the copy is deleted and the file is opened
 * again. A file that vanished, or (POSIX) was replaced, before the lock was granted is opened
 * again the same way. With Quarantine and a read-write mode, a path whose quarantine copy's name
 * would exceed the VFS's limit (detail::checkQuarantineRoom; on Linux the last 37 bytes before
 * the unix VFS's 512) is refused up front with InvalidArgument, so no store is ever created that
 * could not be quarantined. Precondition (FROST_PRE): options.schema is not null. Errors: those
 * of open; InvalidArgument (that path limit); Busy (the file is still open in this or another
 * process: the lock is not granted within options.busyTimeout; or the file changed during
 * quarantine three times), PermissionDenied, IoError or a failed judgement of the copy from the
 * quarantine, in which case the originals are intact. Memory O(64 KiB) plus one open of the copy
 * (a quick_check of it with Verify::QuickCheck). Blocking; thread-safe for different paths.
 */
[[nodiscard]] Result<StoreOpenOutcome> openStore(std::string_view path, const OpenOptions& options,
                                                 CorruptPolicy policy);

/*
 * Removes <path>, then <path>-wal, <path>-shm and <path>-journal; missing files are fine. It
 * takes SQLite's EXCLUSIVE lock through the VFS first, so it fails with Busy while any
 * connection in any process has the database open (WAL connections hold a SHARED lock until they
 * close); it waits up to 5 s in all for the lock and, on Windows, for the delete. A file that
 * was removed or replaced while it waited (POSIX; SQLITE_FCNTL_HAS_MOVED) is looked at again,
 * never removed by name. Stale companions of a missing main file are removed under the lock of an
 * empty placeholder (see the rules above). A main file that cannot be removed is Busy
 * (PermissionDenied when the OS says so) and nothing else is removed. For caches (Invalidate
 * Caches, M3) only: local history and hot-exit journals are user data and are never removed by
 * cache maintenance (ADR 0014).
 * Errors: InvalidArgument (path), Busy, PermissionDenied, IoError.
 */
[[nodiscard]] Result<void> removeDatabaseFiles(std::string_view path);

} // namespace frost::storage
