// SPDX-License-Identifier: Apache-2.0
//
// Private: operations on a database's files through SQLite's VFS (store.hpp explains why
// nothing else may touch them): the EXCLUSIVE lock that proves no connection has the file open,
// streaming copies for quarantine, and removal in the safe order.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace frost::storage::detail {

// The default wait for the lock in removeDatabaseFiles and the journal's removal (5 s).
inline constexpr IClock::Duration kFileLockTimeout = std::chrono::seconds(5);

// The longest suffix quarantineFiles adds to a path: ".corrupt-yyyymmddTHHMMSSZ" (25 bytes) and a
// "-n" counter of up to three digits. Units: bytes.
inline constexpr std::size_t kQuarantineSuffixBytes = 25 + 4;

/*
 * Removes the main file, then -wal, -shm and -journal, after taking SQLite's EXCLUSIVE lock on
 * the main file through `vfsName` (empty: the default VFS of database.hpp), retrying with the
 * busy delay table. `lockTimeout` bounds the whole wait for other handles: for the lock and, on
 * Windows, for the delete of the main file (another handle without FILE_SHARE_DELETE makes it
 * fail; frost::io deletes it, retried on that schedule, instead of the win32 VFS, which would
 * retry for about 1.4 s on its own). When the locked file turns out to have been removed or
 * replaced meanwhile (POSIX), the path is looked at again (3 rounds, then Busy). A missing main
 * file with companions beside it is first re-created empty and exclusively (a placeholder that
 * SQLite would take for a fresh database), so the companions are only ever removed under the lock
 * of a main file, never those of a store that someone creates at that instant. Errors:
 * InvalidArgument (path), Busy (lock not granted, the main file could not be deleted, or it kept
 * being replaced), PermissionDenied, IoError. A companion that cannot be removed after the main
 * file is gone is ignored (SQLite discards a stale -wal beside a missing database).
 */
[[nodiscard]] Result<void> removeFiles(std::string_view path, IClock::Duration lockTimeout,
                                       std::string_view vfsName = {});

/*
 * Judges a quarantine copy (main file and -wal, byte-identical to the original under its lock):
 * true when it is still corrupt, false when it is not (it opens, is empty or is a newer
 * version's store: the original was replaced since the verdict), or an error when the judgement
 * itself failed (then nothing is removed). Called with the original's EXCLUSIVE lock held, so it
 * must not open the original; it may open the copy, and may leave a -wal, -shm or -journal beside
 * it.
 */
using StillCorrupt = std::function<Result<bool>(const std::string& copyPath)>;

/*
 * InvalidArgument when the names quarantineFiles would give `path`'s copy (with
 * kQuarantineSuffixBytes added) exceed the VFS's limit as Database::open applies it, so a store
 * that could never be quarantined is refused before it is created. On Linux that is a path within
 * 37 bytes of the unix VFS's 512. Errors also: those of path normalisation; unknown VFS.
 */
[[nodiscard]] Result<void> checkQuarantineRoom(std::string_view path, std::string_view vfsName = {});

/*
 * Quarantines a database: under the EXCLUSIVE lock, streams <path> and (if present) <path>-wal
 * into "<path>.corrupt-<UTC yyyymmddTHHMMSSZ>[-n]" and "<that>-wal" (synced, O(64 KiB) memory,
 * sized from the locked handles, not from the names), then removes the originals as removeFiles
 * does; `lockTimeout` bounds the waits for the lock and for the delete together. Returns the
 * copy's path. Returns nullopt, with nothing copied or removed, when the path no longer names the
 * file that was judged: it vanished before the lock, or (POSIX) it was removed or replaced while
 * we waited. With `stillCorrupt`, the copy is judged first, still under the lock; when it is no
 * longer corrupt (another process quarantined the file and created a fresh store after our
 * verdict), the copies are deleted, the originals stay, and the result is nullopt too. On failure
 * before the removal, the copies are deleted again and the originals are untouched. Errors: as
 * removeFiles; InvalidArgument when the copy's name exceeds the VFS's limit; a failed judgement.
 */
[[nodiscard]] Result<std::optional<std::string>> quarantineFiles(std::string_view path, IClock& clock,
                                                                 IClock::Duration lockTimeout,
                                                                 std::string_view vfsName = {},
                                                                 const StillCorrupt& stillCorrupt = {});

} // namespace frost::storage::detail
