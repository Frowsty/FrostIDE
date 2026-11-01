// SPDX-License-Identifier: Apache-2.0
//
// Private: operations on a database's files through SQLite's VFS (store.hpp explains why
// nothing else may touch them): the EXCLUSIVE lock that proves no connection has the file open,
// streaming copies for quarantine, and removal in the safe order.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace frost::storage::detail {

// The default wait for the lock in removeDatabaseFiles and the journal's removal (5 s).
inline constexpr IClock::Duration kFileLockTimeout = std::chrono::seconds(5);

/*
 * Removes the main file, then -wal, -shm and -journal, after taking SQLite's EXCLUSIVE lock on
 * the main file through `vfsName` (empty: the default VFS of database.hpp), retrying with the
 * busy delay table until `lockTimeout`. A missing main file means nothing is locked, and only
 * stale companions are removed. Errors: InvalidArgument (path), Busy (lock not granted, or the
 * main file could not be deleted), PermissionDenied, IoError. A companion that cannot be removed
 * after the main file is gone is ignored (SQLite discards a stale -wal beside a missing database).
 */
[[nodiscard]] Result<void> removeFiles(std::string_view path, IClock::Duration lockTimeout,
                                       std::string_view vfsName = {});

/*
 * Judges a quarantine copy (main file and -wal, byte-identical to the original under its lock):
 * true when it is still corrupt. Called with the original's EXCLUSIVE lock held, so it must not
 * open the original; it may open the copy, and may leave a -wal, -shm or -journal beside it.
 */
using StillCorrupt = std::function<bool(const std::string& copyPath)>;

/*
 * Quarantines a database: under the EXCLUSIVE lock, streams <path> and (if present) <path>-wal
 * into "<path>.corrupt-<UTC yyyymmddTHHMMSSZ>[-n]" and "<that>-wal" (synced, O(64 KiB) memory),
 * then removes the originals as removeFiles does. Returns the copy's path. With `stillCorrupt`,
 * the copy is judged first, still under the lock; when it is no longer corrupt (another process
 * quarantined the file and created a fresh store after our verdict), the copies are deleted, the
 * originals stay, and the result is nullopt. On failure before the removal, the copies are
 * deleted again and the originals are untouched. Errors: as removeFiles.
 */
[[nodiscard]] Result<std::optional<std::string>> quarantineFiles(std::string_view path, IClock& clock,
                                                                 IClock::Duration lockTimeout,
                                                                 std::string_view vfsName = {},
                                                                 const StillCorrupt& stillCorrupt = {});

} // namespace frost::storage::detail
