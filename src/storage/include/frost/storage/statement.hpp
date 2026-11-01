// SPDX-License-Identifier: Apache-2.0
//
// Statement: one prepared SQLite statement of a frost::storage Database (database.hpp), with
// Result-returning bind and step calls and view-returning column accessors (ADR 0014).
//
/*
 * Ownership. Database::prepare creates a Statement; it owns its sqlite3_stmt and finalizes it
 * on destruction. It refers to (does not own) the Database's heap-allocated connection, so a
 * Statement stays valid when its Database is moved. Every Statement must be destroyed before its
 * Database closes: close() and ~Database check the connection's live-statement count with
 * FROST_PRE, so a Statement outliving its Database is an enforced contract violation rather than
 * a use-after-free. A moved-from Statement may only be destroyed or assigned to.
 *
 * Thread safety: none. A Statement is used by the thread that currently uses its Database.
 *
 * Errors carry SQLite's message and extended result code (sqlite_error.hpp for the mapping);
 * step errors add the first 120 characters of the SQL. Values are always bound, never spliced
 * into SQL, so messages hold FrostIDE's own SQL only.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/storage/fingerprint.hpp"

#include <cstdint>
#include <string_view>

struct sqlite3_stmt;

namespace frost::storage {

namespace detail {
struct Connection;
} // namespace detail

class Database;

class Statement {
public:
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete ("a prepared statement has one owner");
    Statement& operator=(const Statement&) = delete ("a prepared statement has one owner");
    ~Statement();

    /*
     * Binding. Parameter indexes are 1-based (?1, ?2, ...). Errors: Internal for an index out of
     * range; OutOfRange for TEXT or BLOB longer than SQLITE_LIMIT_LENGTH (the 64-bit bind calls
     * are used, so sizes above 2 GiB are reported, not truncated). Precondition (FROST_PRE): the
     * statement has not been stepped since its last reset() (SQLite refuses to bind a running
     * statement). O(1) plus the copy for TEXT and BLOB.
     */
    [[nodiscard]] Result<void> bindInt64(int index, std::int64_t value);
    [[nodiscard]] Result<void> bindDouble(int index, double value);
    // Copied (SQLITE_TRANSIENT). An empty view binds '' (never NULL).
    [[nodiscard]] Result<void> bindText(int index, std::string_view utf8);
    // Copied. An empty view binds an empty BLOB (never NULL).
    [[nodiscard]] Result<void> bindBlob(int index, std::string_view bytes);
    /*
     * Binds without copying (SQLITE_STATIC). SQLite keeps bindings across reset(), so `bytes`
     * must stay valid until this parameter is rebound, clearBindings() runs or the Statement is
     * destroyed, not merely until the next step. An empty view binds an empty BLOB.
     */
    [[nodiscard]] Result<void> bindBlobNoCopy(int index, std::string_view bytes);
    // The 16 canonical bytes as a BLOB (copied).
    [[nodiscard]] Result<void> bindFingerprint(int index, const Fingerprint128& value);
    [[nodiscard]] Result<void> bindNull(int index);

    /*
     * Runs the statement to its next row: true when a row is available, false when it is done.
     * After an error, call reset() before reusing the statement. Errors: as sqlite_error.hpp maps
     * them (Busy, Cancelled after an interrupt, Corrupt, IoError, AlreadyExists for UNIQUE and
     * PRIMARY KEY violations, InvalidArgument for other constraint violations, ...). Cancelled
     * also when Transaction::rollback reset this running statement because an interrupt was
     * pending (database.hpp); the statement is then reset, and its columns read as NULL until
     * that step.
     */
    [[nodiscard]] Result<bool> step();
    // Steps once expecting no row, then resets (also on failure), so the statement can be
    // rebound and run again. A row is InvalidArgument ("statement returned a row").
    [[nodiscard]] Result<void> run();
    // Rewinds the statement for another run; bindings are kept. Never fails (an earlier step
    // error was already reported by step()).
    void reset() noexcept;
    // Sets every parameter to NULL.
    void clearBindings() noexcept;

    /*
     * Column access. Preconditions (FROST_PRE): step() returned true and nothing reset the
     * statement since, and 0 <= col < columnCount(). Views stay valid until the next step,
     * reset or destruction, or a type-converting call on the same column. NULL reads as 0, 0.0 or
     * an empty view; columnIsNull tells them apart. The implementation reads the pointer before
     * the byte count, as SQLite requires, and builds views from (pointer, length) only, so
     * embedded NULs are kept. O(1).
     */
    [[nodiscard]] int columnCount() const noexcept;
    [[nodiscard]] bool columnIsNull(int col) const noexcept;
    [[nodiscard]] std::int64_t columnInt64(int col) const noexcept;
    [[nodiscard]] double columnDouble(int col) const noexcept;
    [[nodiscard]] std::string_view columnText(int col) const noexcept;
    [[nodiscard]] std::string_view columnBlob(int col) const noexcept;
    // Errors: Corrupt unless the column holds exactly 16 bytes.
    [[nodiscard]] Result<Fingerprint128> columnFingerprint(int col) const;

    // The SQL text the statement was prepared from.
    [[nodiscard]] std::string_view sql() const noexcept;
    // Escape hatch for tests and diagnostics; bypasses the wrapper's invariants.
    [[nodiscard]] sqlite3_stmt* nativeHandle() const noexcept { return m_stmt; }

private:
    friend class Database;
    Statement(detail::Connection* connection, sqlite3_stmt* stmt) noexcept;
    [[nodiscard]] Result<void> checkBind(int rc, int index);
    void release() noexcept;

    detail::Connection* m_connection = nullptr; // Not owned; see the header comment.
    sqlite3_stmt* m_stmt = nullptr;             // Owned.
    bool m_hasRow = false;                      // The last step() returned a row.
    bool m_stepped = false;                     // Stepped since the last reset (binding is refused).
    std::uint64_t m_rowResets = 0;              // Connection::statementResets at the last row.
};

} // namespace frost::storage
