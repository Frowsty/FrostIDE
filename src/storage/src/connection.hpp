// SPDX-License-Identifier: Apache-2.0
//
// Private: detail::Connection, the heap state behind a frost::storage::Database (the SQLite
// handle, its path, the live-object counters and the busy-handler and WAL-hook state).
#pragma once

#include "frost/core/clock.hpp"

#include <cstdint>
#include <string>

#include <sqlite3.h>

namespace frost::storage::detail {

/*
 * Owned by exactly one Database through a unique_ptr, so its address is stable across moves and
 * Statement, Transaction and the SQLite callbacks can point at it. Not thread-safe, like the
 * Database; the busy handler and the WAL hook run on the thread that is using the connection
 * (inside a step), so they need no synchronisation. InterruptHandle reads only `db` from other
 * threads, and sqlite3_interrupt is safe to call concurrently (database.hpp has the lifetime rule).
 */
struct Connection {
    sqlite3* db = nullptr;
    std::string path;    // Normalised UTF-8 path; "" in memory.
    std::string vfsName; // The VFS the file was opened with ("" = SQLite's default).
    bool inMemory = false;

    // Statements alive and transactions open; close() requires both to be zero (FROST_PRE).
    int liveStatements = 0;
    int openTransactions = 0;
    // Times Transaction::rollback reset running statements to end a transaction under a pending
    // interrupt; a Statement that returned its row before the latest count was reset under it.
    std::uint64_t statementResets = 0;

    // Busy handler: the total wait per lock attempt, and the deadline of the current attempt
    // (set when SQLite calls the handler with count 0).
    IClock::Duration busyTimeout{};
    IClock::SteadyTime busyDeadline{};

    // WAL hook: passive checkpoint threshold in frames (0 = never), and the result code of the
    // hook's last failed checkpoint (SQLITE_OK when none failed), kept for diagnostics because
    // the hook must never fail a commit.
    std::uint32_t walAutoCheckpointPages = 0;
    int lastCheckpointError = SQLITE_OK;
};

} // namespace frost::storage::detail
