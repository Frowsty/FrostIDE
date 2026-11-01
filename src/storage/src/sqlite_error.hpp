// SPDX-License-Identifier: Apache-2.0
//
// Private: mapping of SQLite result codes to frost::ErrorCode, and construction of frost::Error
// values that keep SQLite's message, extended code and OS error for logs (ADR 0014).
//
/*
 * The mapping, by intent rather than by the letter of each code:
 *   Busy              BUSY (incl. _RECOVERY, _SNAPSHOT, _TIMEOUT), LOCKED, PROTOCOL, SCHEMA
 *   Cancelled         INTERRUPT; ABORT except ABORT_ROLLBACK; and a Busy result on a connection
 *                     that sqlite3_is_interrupted reports (decided by the callers that know)
 *   Corrupt           CORRUPT (all extended codes), NOTADB, FORMAT, IOERR_CORRUPTFS, IOERR_DATA
 *   IoError           FULL ("database or disk is full" stays in the message), every other IOERR,
 *                     NOMEM, READONLY_DBMOVED ("database file moved or deleted")
 *   InvalidArgument   CANTOPEN_ISDIR, CANTOPEN_FULLPATH; CONSTRAINT codes other than UNIQUE and
 *                     PRIMARYKEY; MISMATCH; ERROR (SQL errors)
 *   (refined)         other CANTOPEN: by the OS error (io::errorCodeFromWin32 / errorCodeFromErrno),
 *                     IoError when there is none or it maps to nothing more specific
 *   PermissionDenied  PERM, AUTH, READONLY (other extended codes)
 *   OutOfRange        TOOBIG
 *   AlreadyExists     CONSTRAINT_UNIQUE, CONSTRAINT_PRIMARYKEY
 *   Internal          MISUSE, RANGE, INTERNAL, EMPTY, NOTFOUND, ABORT_ROLLBACK (FrostIDE bugs)
 *   Unsupported       NOLFS
 *   Unknown           anything else (OK, ROW and DONE are not errors and map here too)
 *
 * Messages read "<operation> '<path>': <SQLite message> (sqlite <code> <NAME>[, win32 error N |
 * errno N])"; statement errors append " [sql: <first 120 characters>]". Pure functions, thread-safe.
 */
#pragma once

#include "frost/core/error.hpp"

#include <source_location>
#include <string>
#include <string_view>

#include <sqlite3.h>

namespace frost::storage::detail {

// The ErrorCode for an extended SQLite result code; `systemErrno` (sqlite3_system_errno) refines
// CANTOPEN only. Never fails.
[[nodiscard]] ErrorCode errorCodeFromSqlite(int extendedCode, int systemErrno) noexcept;

// The macro name of a result code ("SQLITE_IOERR_FSYNC"), or "SQLITE_<n>" style "unknown" text
// ("SQLITE_UNKNOWN") for codes the table does not know.
[[nodiscard]] std::string_view sqliteCodeName(int extendedCode) noexcept;

/*
 * An Error for `extendedCode`. `message` is SQLite's text (sqlite3_errmsg or sqlite3_errstr);
 * `systemErrno` is appended for CANTOPEN and IOERR codes when non-zero; `sql`, when given, is
 * appended cut to 120 characters. `path` may be empty.
 */
[[nodiscard]] Error makeSqliteError(int extendedCode, std::string_view message, int systemErrno,
                                    std::string_view operation, std::string_view path, std::string_view sql = {},
                                    std::source_location where = std::source_location::current());

/*
 * An Error from the connection's current error state (sqlite3_extended_errcode, sqlite3_errmsg
 * and sqlite3_system_errno), with `rc` as the code when the connection reports a different one.
 * `db` may be null (then sqlite3_errstr(rc) is the message).
 */
[[nodiscard]] Error sqliteError(sqlite3* db, int rc, std::string_view operation, std::string_view path,
                                std::string_view sql = {},
                                std::source_location where = std::source_location::current());

} // namespace frost::storage::detail
