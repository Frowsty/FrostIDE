// SPDX-License-Identifier: Apache-2.0
//
// Database: frost::storage's SQLite connection (SQLite 3.54.1 in WAL mode, ADR 0014), with
// transactions, checkpoints, integrity checks, schema validation and migration on open, and a
// thread-safe interrupt handle.
//
/*
 * Every call in this module blocks on disk I/O and on other connections' locks: never call it
 * on the UI thread (PLAN.md section 3.3). Callers run it on a frost::sched Background or
 * UserVisible lane; long operations take a std::stop_token. TODO(M1): Task<T> wrappers, if
 * m1/save-manager wants them.
 *
 * Connection rules (ADR 0014): WAL journal mode; SQLITE_OPEN_NOMUTEX, so one connection is
 * used by one thread at a time; DEFENSIVE, no double-quoted string literals, untrusted schema,
 * no extension loading; foreign keys on; temp_store=MEMORY; cell_size_check on; a busy handler
 * that waits up to OpenOptions::busyTimeout per lock attempt and gives up early on an interrupt;
 * our own WAL hook instead of PRAGMA wal_autocheckpoint (so a checkpoint error never fails a
 * commit). Write transactions should use TransactionKind::Immediate: a Deferred transaction that
 * reads first and writes later gets Busy without the busy handler being called (SQLite's
 * deadlock rule).
 *
 * Database files are touched only through SQLite's VFS (store.hpp explains why). Windows opens
 * use the "win32-longpath" VFS with the \\?\ form of the path, so paths beyond MAX_PATH work
 * whatever LongPathsEnabled says.
 */
#pragma once

#include "frost/compat/function_ref.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/storage/statement.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

struct sqlite3;

namespace frost::storage {

struct SchemaSpec;

enum class OpenMode : unsigned char {
    ReadWriteCreate, // Creates the file (not its directory) when missing.
    ReadWrite,       // The file must exist (NotFound otherwise).
    ReadOnly,        // The file must exist; writes fail with PermissionDenied.
};

/*
 * Durability of commits in WAL mode. Normal: a committed transaction survives a crash of the
 * process; a power loss may drop the last commits but never corrupts the file. Full: each commit
 * is durable once commit() returns (the WAL is fsynced per commit).
 */
enum class Synchronous : unsigned char { Normal, Full };

// What open() verifies before any write. QuickCheck runs PRAGMA quick_check, O(file size).
enum class Verify : unsigned char { Header, QuickCheck };

enum class TransactionKind : unsigned char { Deferred, Immediate, Exclusive };

// sqlite3_wal_checkpoint_v2 modes. Full, Restart and Truncate wait up to busyTimeout for readers.
enum class CheckpointMode : unsigned char { Passive, Full, Restart, Truncate };

/*
 * What checkpoint() did. walFrames and checkpointedFrames are -1 on a database that is not in
 * WAL mode (in memory). complete: every frame was copied back (for Passive, checkpointedFrames
 * == walFrames; for the waiting modes, no reader or writer stopped them).
 */
struct CheckpointResult {
    int walFrames = 0;
    int checkpointedFrames = 0;
    bool complete = true;
};

struct OpenOptions {
    OpenMode mode = OpenMode::ReadWriteCreate;
    Synchronous synchronous = Synchronous::Full;
    // The busy handler's total wait per lock attempt, against the real steady clock.
    IClock::Duration busyTimeout = std::chrono::seconds(5);
    // Page cache per connection, in KiB. Budget: B19.
    std::int64_t cacheSizeKiB = 2048;
    // Our WAL hook runs a passive checkpoint after a commit leaves at least this many frames in
    // the WAL; 0 turns automatic checkpoints off.
    std::uint32_t walAutoCheckpointPages = 1000;
    // PRAGMA journal_size_limit: the WAL is truncated to this many bytes after a checkpoint.
    std::int64_t journalSizeLimitBytes = std::int64_t{64} << 20;
    // false: SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE (close leaves the WAL in place).
    bool checkpointOnClose = true;
    // PRAGMA locking_mode=EXCLUSIVE, with the write lock taken during open() and held until close.
    // The WAL index then lives in heap memory, so no -shm file is made.
    bool exclusiveLock = false;
    // false: a fresh (empty) file is refused with NotFound and left uninitialised.
    bool allowFresh = true;
    Verify verify = Verify::Header;
    // Validated and migrated by open() (schema.hpp); nullptr skips schema handling. Not owned;
    // it must outlive the call.
    const SchemaSpec* schema = nullptr;
    // SQLite VFS name. Empty: "win32-longpath" on Windows, SQLite's default elsewhere.
    std::string vfs{};
    // Clock for quarantine names (openStore); nullptr means systemClock(). Not owned.
    IClock* clock = nullptr;
};

/*
 * Cancels the statement running on the connection it came from (sqlite3_interrupt), from any
 * thread. Copyable. Statements that the connection starts before its count of active statements
 * drops to zero fail too; when nothing runs it is a no-op (SQLite clears the flag when the next
 * statement starts), so multi-statement operations also check a std::stop_token between
 * statements.
 *
 * Precondition: the Database has not been closed, destroyed or move-assigned over, and none of
 * that runs concurrently with the call. Register it in a std::stop_callback scoped inside one
 * operation (a stop_callback's destructor waits for a running callback), never in one that
 * outlives the Database.
 */
class InterruptHandle {
public:
    InterruptHandle() noexcept = default;
    // No-op for a default-constructed handle. Thread-safe.
    void interrupt() const noexcept;

private:
    friend class Database;
    explicit InterruptHandle(sqlite3* db) noexcept : m_db(db) {}
    sqlite3* m_db = nullptr; // The heap Connection's handle; not owned.
};

class Transaction;

/*
 * One SQLite connection. Move-only; not thread-safe: it may be used from different threads one
 * at a time, with a happens-before edge between the uses (the owning actor provides it). The
 * connection lives on the heap, so moving a Database never moves the SQLite handle, and the
 * Statement, Transaction and InterruptHandle objects made from it stay valid. A moved-from
 * Database refuses every call except destruction and assignment (FROST_PRE).
 */
class Database {
public:
    /*
     * Opens `path` (absolute UTF-8, normalised as frost::io does) in WAL mode with the pragmas of
     * ADR 0014, then validates and migrates options.schema (schema.hpp). A file that is refused
     * (foreign, newer, not a database, failed quick check) is closed without being modified.
     * Errors: InvalidArgument (relative path, NUL, a directory, a path longer than the VFS limit,
     * unknown VFS), NotFound (parent directory missing; no file in ReadWrite or ReadOnly mode; a
     * fresh file with allowFresh=false), PermissionDenied (ReadWrite on a read-only file or
     * directory), Corrupt (not a database, foreign application_id, quick_check failed),
     * Unsupported (schema newer than the spec; ReadOnly on a version that needs migration; WAL
     * unavailable; the linked SQLite is not the pinned build), Busy (a lock held past
     * busyTimeout, e.g. another process's exclusive journal), IoError.
     */
    [[nodiscard]] static Result<Database> open(std::string_view path, const OpenOptions& options = {});
    // A private in-memory database for tests and scratch work: no file and no WAL; the schema
    // (if any) is created as for a fresh file. Errors: as open().
    [[nodiscard]] static Result<Database> openInMemory(const OpenOptions& options = {});

    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;
    Database(const Database&) = delete ("a connection has one owner");
    Database& operator=(const Database&) = delete ("a connection has one owner");
    // As close(), ignoring errors; same precondition. Uses sqlite3_close_v2. Budget: B20.
    ~Database();

    /*
     * Closes the connection. With checkpointOnClose, the last connection to a file checkpoints
     * the WAL (normally bounded by walAutoCheckpointPages plus the last transaction; more when
     * readers kept the hook's checkpoints from completing) and removes -wal and -shm.
     * Precondition (FROST_PRE): no Statement and no open Transaction of this Database is alive.
     * Afterwards the object is like a moved-from one, whatever the result. Errors: Busy when a
     * statement prepared through nativeHandle() is still unfinalized (SQLite then frees the
     * handle once it is finalized). Budget: B20.
     */
    [[nodiscard]] Result<void> close();

    // Prepares exactly one statement. Trailing whitespace and comments are fine; trailing SQL
    // and a NUL byte anywhere are InvalidArgument. Errors: InvalidArgument (syntax, more than one
    // statement, NUL), Busy (schema locked). O(SQL length).
    [[nodiscard]] Result<Statement> prepare(std::string_view sql);
    // Runs one or more statements without parameters (DDL, PRAGMA), discarding any rows. Errors:
    // as prepare and Statement::step (a NUL byte is InvalidArgument and runs nothing).
    [[nodiscard]] Result<void> execute(std::string_view sql);

    /*
     * Begins a transaction. Nested begin is InvalidArgument. Use Immediate for writes (see the
     * header comment); Exclusive takes the write lock and, outside WAL, blocks readers too.
     */
    [[nodiscard]] Result<Transaction> begin(TransactionKind kind);
    // begin(kind); body(); commit when body succeeds, otherwise roll back and return body's error.
    [[nodiscard]] Result<void> inTransaction(TransactionKind kind, frost::function_ref<Result<void>()> body);

    /*
     * Copies WAL frames back into the database. Busy is not an error here: it yields
     * complete = false. Full, Restart and Truncate wait up to busyTimeout for readers and
     * writers. Errors: IoError, Corrupt.
     */
    [[nodiscard]] Result<CheckpointResult> checkpoint(CheckpointMode mode = CheckpointMode::Passive);
    // PRAGMA quick_check. Corrupt carries the first 10 problems. Cancelled on a stop request. O(size).
    [[nodiscard]] Result<void> quickCheck(std::stop_token stop = {});
    // PRAGMA integrity_check, then foreign_key_check. As quickCheck, but slower and thorough; a
    // stop request cancels either pass.
    [[nodiscard]] Result<void> integrityCheck(std::stop_token stop = {});

    [[nodiscard]] std::int64_t lastInsertRowId() const noexcept;
    // Rows changed by the last INSERT, UPDATE or DELETE.
    [[nodiscard]] std::int64_t changes() const noexcept;
    // The normalised path (never the \\?\ form); "" for an in-memory database.
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] InterruptHandle interruptHandle() const noexcept;
    // Whether a moved-from or closed object (false) or a live connection (true).
    [[nodiscard]] bool isOpen() const noexcept { return m_connection != nullptr; }
    // Escape hatch for tests and diagnostics; using it bypasses the wrapper's invariants.
    [[nodiscard]] sqlite3* nativeHandle() const noexcept;

private:
    explicit Database(std::unique_ptr<detail::Connection> connection) noexcept;
    friend struct DatabaseAccess; // database.cpp and schema.cpp: the open sequence.

    std::unique_ptr<detail::Connection> m_connection; // Heap, so moving never moves the handle.
};

/*
 * An open transaction (RAII). Move-only; ends (commit or rollback) before its Database closes,
 * which close() checks with FROST_PRE. The destructor rolls back unless commit() succeeded.
 */
class Transaction {
public:
    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&& other) noexcept;
    Transaction(const Transaction&) = delete ("a transaction has one owner");
    Transaction& operator=(const Transaction&) = delete ("a transaction has one owner");
    ~Transaction();

    /*
     * Commits. On Busy the transaction stays open: retry, or let the destructor roll back. On
     * IoError the outcome is unknown: a failed sync can leave the commit record in the WAL, so
     * the transaction may be durable after a crash, while SQLite may already have rolled back
     * in memory; callers re-read or retry idempotently. Errors from the checkpoint that may follow
     * a commit never reach commit() (our WAL hook keeps them).
     */
    [[nodiscard]] Result<void> commit();
    /*
     * Rolls back. Idempotent; a no-op when SQLite already rolled back (sqlite3_get_autocommit).
     * When an interrupt is pending (InterruptHandle::interrupt while another statement of the
     * connection was running), SQLite fails every new statement, ROLLBACK too, until no statement
     * is active, so rollback() then resets the connection's running statements and retries: the
     * interrupt has cancelled them anyway, and their next step() reports Cancelled (their current
     * row is gone; columns read as NULL). Should ROLLBACK still fail (only a new interrupt in the
     * instant it starts, several times over, can do that), the transaction stays open and
     * active() stays true; the destructor then ends the object, and begin() keeps refusing until
     * close() rolls back. O(statements of the connection).
     */
    void rollback() noexcept;
    // Whether the transaction is still open.
    [[nodiscard]] bool active() const noexcept { return m_connection != nullptr; }

private:
    friend class Database;
    explicit Transaction(detail::Connection* connection) noexcept : m_connection(connection) {}
    void finish() noexcept;
    void end() noexcept; // rollback(), then finish() even if that failed: destructor, assignment.

    detail::Connection* m_connection = nullptr; // Not owned; null once ended.
};

} // namespace frost::storage
