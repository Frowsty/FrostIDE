// SPDX-License-Identifier: Apache-2.0
//
// frost::storage::Database, Transaction and InterruptHandle (see frost/storage/database.hpp):
// the open sequence, the busy handler, the WAL hook, transactions, checkpoints and integrity
// checks.

#include "frost/storage/database.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/storage/schema.hpp"

#include "busy_wait.hpp"
#include "connection.hpp"
#include "database_internal.hpp"
#include "sqlite_error.hpp"
#include "sqlite_init.hpp"
#include "test_hooks.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <sqlite3.h>

namespace frost::storage {

namespace {

using detail::Connection;

/*
 * Busy handler: sleeps through the busy_wait.hpp schedule until the connection's busyTimeout
 * has passed since the first call of this lock attempt (count == 0), or until the connection is
 * interrupted. Returning 0 makes SQLite give up with SQLITE_BUSY. Unlike sqlite3_busy_timeout it
 * measures a real steady deadline (not the sum of the planned delays) and checks for interrupts.
 */
int busyHandler(void* context, int count) noexcept {
    auto* connection = static_cast<Connection*>(context);
    if (count == 0) {
        connection->busyDeadline = systemClock().steadyNow() + connection->busyTimeout;
    }
    if (sqlite3_is_interrupted(connection->db) != 0) {
        return 0;
    }
    return detail::sleepBeforeRetry(static_cast<std::size_t>(count), connection->busyDeadline) ? 1 : 0;
}

/*
 * WAL hook (replaces PRAGMA wal_autocheckpoint, whose default hook turns a failed checkpoint into
 * a failed COMMIT although the commit has happened; see sqlite3_wal_hook). After each commit
 * that leaves at least walAutoCheckpointPages frames, it runs a passive checkpoint, which never
 * waits for readers, so it never blocks the commit. It always returns SQLITE_OK and keeps a
 * failure in lastCheckpointError for diagnostics.
 */
int walHook(void* context, sqlite3* db, const char* databaseName, int frames) noexcept {
    auto* connection = static_cast<Connection*>(context);
    if (connection->walAutoCheckpointPages != 0 &&
        static_cast<std::uint32_t>(frames) >= connection->walAutoCheckpointPages) {
        const int rc = sqlite3_wal_checkpoint_v2(db, databaseName, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
        if (rc != SQLITE_OK && rc != SQLITE_BUSY) {
            connection->lastCheckpointError = rc;
        }
    }
    return SQLITE_OK;
}

#ifdef _WIN32
constexpr const char* kDefaultVfs = "win32-longpath";
#else
constexpr const char* kDefaultVfs = nullptr; // SQLite's default ("unix").
#endif

// The open flags for a mode; SQLITE_OPEN_NOMUTEX: one thread at a time per connection.
int openFlags(OpenMode mode) noexcept {
    int flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE;
    switch (mode) {
    case OpenMode::ReadWriteCreate: flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE; break;
    case OpenMode::ReadWrite: flags |= SQLITE_OPEN_READWRITE; break;
    case OpenMode::ReadOnly: flags |= SQLITE_OPEN_READONLY; break;
    }
    return flags;
}

// Step 2 of the open sequence: the target and its parent, checked with stat (no descriptor).
Result<void> checkTarget(const std::string& path, OpenMode mode) {
    io::IFileSystem& fs = io::realFileSystem();
    auto status = fs.status(path);
    if (status) {
        if (status->type == io::FileType::Directory) {
            return fail(ErrorCode::InvalidArgument, std::format("open database '{}': is a directory", path));
        }
        if (status->type != io::FileType::Regular) {
            return fail(ErrorCode::InvalidArgument, std::format("open database '{}': not a regular file", path));
        }
        /*
         * SQLite's unix VFS reports a 1-byte file as empty (os_unix.c, ticket #3260, an OS X
         * msdos workaround), so it would be overwritten as fresh and a -wal beside it deleted;
         * the Windows VFS calls it "not a database". FrostIDE never writes such a file, so on
         * every platform it is Corrupt, and openStore keeps its bytes (and the -wal) in quarantine.
         */
        if (status->size == 1) {
            return fail(ErrorCode::Corrupt, std::format("open database '{}': not a database (1-byte file)", path));
        }
        return {};
    }
    if (status.error().code() != ErrorCode::NotFound) {
        return std::unexpected(std::move(status.error()).withContext("open database"));
    }
    const std::string_view parent = io::parentPath(path);
    auto parentStatus = fs.status(std::string(parent));
    if (!parentStatus) {
        if (parentStatus.error().code() == ErrorCode::NotFound) {
            return fail(ErrorCode::NotFound, std::format("open database '{}': parent directory missing", path));
        }
        return std::unexpected(std::move(parentStatus.error()).withContext("open database"));
    }
    if (parentStatus->type != io::FileType::Directory) {
        return fail(ErrorCode::NotFound, std::format("open database '{}': parent is not a directory", path));
    }
    if (mode != OpenMode::ReadWriteCreate) {
        return fail(ErrorCode::NotFound, std::format("open database '{}': no such database file", path));
    }
    return {};
}

// Step 6: the hardening dbconfig settings.
Result<void> configureConnection(Connection& connection, bool checkpointOnClose) {
    struct Setting {
        int op;
        int value;
        const char* name;
    };
    const std::array<Setting, 6> settings{{
        {SQLITE_DBCONFIG_DEFENSIVE, 1, "DEFENSIVE"},
        {SQLITE_DBCONFIG_DQS_DML, 0, "DQS_DML"},
        {SQLITE_DBCONFIG_DQS_DDL, 0, "DQS_DDL"},
        {SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, "TRUSTED_SCHEMA"},
        {SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, "ENABLE_LOAD_EXTENSION"},
        {SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, checkpointOnClose ? 0 : 1, "NO_CKPT_ON_CLOSE"},
    }};
    for (const Setting& setting : settings) {
        int applied = -1;
        const int rc = sqlite3_db_config(connection.db, setting.op, setting.value, &applied);
        if (rc != SQLITE_OK || applied != setting.value) {
            return fail(ErrorCode::Internal, std::format("open database '{}': sqlite3_db_config({}) failed (sqlite {})",
                                                         connection.path, setting.name, rc));
        }
    }
    return {};
}

/*
 * PRAGMA journal_mode=WAL, retried on Busy with the busy_wait.hpp schedule until busyTimeout.
 * Switching a rollback-mode file upgrades a read transaction to a write one, and SQLite does not
 * call the busy handler for that upgrade (btree.c, sqlite3BtreeBeginTrans: it could deadlock), so
 * first opens racing to switch the same new file would otherwise see Busy at once. Each attempt
 * may also wait in the busy handler, which starts its own busyTimeout per lock, so busyTimeout is
 * cut to the time left before every attempt: the whole switch waits at most busyTimeout. An
 * interrupt surfaces as Cancelled (queryText), which ends the loop.
 */
Result<std::string> switchToWal(Connection& connection) {
    const IClock::Duration configured = connection.busyTimeout;
    const IClock::SteadyTime deadline = systemClock().steadyNow() + configured;
    for (std::size_t attempt = 0;; ++attempt) {
        connection.busyTimeout =
            std::max(std::chrono::duration_cast<IClock::Duration>(deadline - systemClock().steadyNow()),
                     IClock::Duration::zero());
        auto mode = detail::queryText(connection, "PRAGMA journal_mode=WAL");
        if (mode || mode.error().code() != ErrorCode::Busy || !detail::sleepBeforeRetry(attempt, deadline)) {
            connection.busyTimeout = configured;
            return mode;
        }
    }
}

// Steps 12 and 13: WAL mode and the per-connection pragmas, then our WAL hook.
Result<void> applyPragmas(Connection& connection, const OpenOptions& options) {
    if (!connection.inMemory && options.mode != OpenMode::ReadOnly) {
        auto mode = switchToWal(connection);
        if (!mode) {
            return std::unexpected(std::move(mode.error()));
        }
        std::string lower = *mode;
        std::ranges::transform(lower, lower.begin(),
                               [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
        if (lower != "wal") {
            return fail(
                ErrorCode::Unsupported,
                std::format("open database '{}': WAL mode unavailable (journal_mode is '{}')", connection.path, *mode));
        }
    }
    const std::string pragmas = std::format(
        "PRAGMA synchronous={}; PRAGMA foreign_keys=ON; PRAGMA cache_size=-{}; PRAGMA journal_size_limit={}; "
        "PRAGMA temp_store=MEMORY; PRAGMA cell_size_check=ON;",
        options.synchronous == Synchronous::Full ? "FULL" : "NORMAL", std::max<std::int64_t>(options.cacheSizeKiB, 1),
        options.journalSizeLimitBytes);
    if (auto applied = detail::executeSql(connection, pragmas); !applied) {
        return applied;
    }
    connection.walAutoCheckpointPages = options.walAutoCheckpointPages;
    if (!connection.inMemory) {
        sqlite3_wal_hook(connection.db, &walHook, &connection);
    }
    return {};
}

Result<Database> openImpl(std::string_view rawPath, const OpenOptions& options, bool inMemory) {
    if (auto initialized = detail::initializeSqlite(); !initialized) {
        return std::unexpected(std::move(initialized.error()));
    }
    if (options.schema != nullptr) {
        detail::checkSchemaSpec(*options.schema);
    }

    auto connection = std::make_unique<Connection>();
    connection->inMemory = inMemory;
    connection->busyTimeout = options.busyTimeout;
    std::string fileName = ":memory:";
    const char* vfsName = nullptr;
    bool walExisted = false;
    if (!inMemory) {
        // Step 1: normalise.
        auto normalized = io::normalizePath(rawPath);
        if (!normalized) {
            return std::unexpected(std::move(normalized.error()).withContext("open database"));
        }
        connection->path = std::move(*normalized);
        // Step 2: the target and its parent.
        if (auto checked = checkTarget(connection->path, options.mode); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        auto wal = io::realFileSystem().status(connection->path + "-wal");
        walExisted = wal.has_value();
        // Step 3: the VFS and the name it sees.
        connection->vfsName = options.vfs.empty() ? (kDefaultVfs != nullptr ? kDefaultVfs : "") : options.vfs;
        vfsName = connection->vfsName.empty() ? nullptr : connection->vfsName.c_str();
        sqlite3_vfs* vfs = sqlite3_vfs_find(vfsName);
        if (vfs == nullptr) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("open database '{}': unknown SQLite VFS '{}'", connection->path, options.vfs));
        }
#ifdef _WIN32
        auto extended = io::extendedLengthPath(connection->path);
        if (!extended) {
            return std::unexpected(std::move(extended.error()).withContext("open database"));
        }
        fileName = std::move(*extended);
#else
        fileName = connection->path;
#endif
        // "-journal" is the longest name SQLite derives from the database name.
        constexpr std::size_t kLongestSuffix = 8;
        if (fileName.size() + kLongestSuffix >= static_cast<std::size_t>(vfs->mxPathname)) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("open database '{}': path longer than the SQLite VFS limit of {} bytes",
                                    connection->path, vfs->mxPathname));
        }
    }

    // Step 4: open. The handle exists even when the open fails, so it is always closed.
    sqlite3* handle = nullptr;
    const int openRc = sqlite3_open_v2(
        fileName.c_str(), &handle, inMemory ? openFlags(OpenMode::ReadWriteCreate) : openFlags(options.mode), vfsName);
    if (openRc != SQLITE_OK) {
        Error error = detail::sqliteError(handle, openRc, "open database", connection->path);
        static_cast<void>(sqlite3_close_v2(handle));
        return std::unexpected(std::move(error));
    }
    connection->db = handle;
    detail::g_afterSqliteOpen.run();
    // `raw` stays valid while `database` owns the connection; abandon() ends that, so error
    // messages after an abandon use the copy in `path`.
    Connection* raw = connection.get();
    const std::string path = connection->path;
    Database database = DatabaseAccess::make(std::move(connection));

    // Step 5: sqlite3_open_v2 silently opens a write-protected file read-only.
    if (!inMemory && options.mode != OpenMode::ReadOnly && sqlite3_db_readonly(raw->db, "main") == 1) {
        DatabaseAccess::abandon(database, walExisted);
        return fail(ErrorCode::PermissionDenied,
                    std::format("open database '{}': the file or its directory is read-only", path));
    }
    // Step 6: hardening. Step 7: the busy handler.
    if (auto configured = configureConnection(*raw, options.checkpointOnClose); !configured) {
        DatabaseAccess::abandon(database, true);
        return std::unexpected(std::move(configured.error()));
    }
    sqlite3_busy_handler(raw->db, &busyHandler, raw);
    // Step 8: exclusive locking before any access, so the WAL index lives in heap memory.
    if (options.exclusiveLock) {
        if (auto exclusive = detail::executeSql(*raw, "PRAGMA locking_mode=EXCLUSIVE"); !exclusive) {
            DatabaseAccess::abandon(database, true);
            return std::unexpected(std::move(exclusive.error()));
        }
    }
    // Step 9: the header. NOTADB surfaces here as Corrupt; another process's lock as Busy.
    auto header = detail::readHeader(*raw);
    if (!header) {
        DatabaseAccess::abandon(database, walExisted);
        return std::unexpected(std::move(header.error()).withContext(std::format("open database '{}'", path)));
    }
    if (header->fresh && !options.allowFresh) {
        DatabaseAccess::abandon(database, walExisted);
        return fail(ErrorCode::NotFound, std::format("open database '{}': empty database", path));
    }
    if (options.schema != nullptr) {
        if (auto accepted =
                detail::checkHeaderAgainstSpec(*header, *options.schema, options.mode, options.allowFresh, path);
            !accepted) {
            DatabaseAccess::abandon(database, walExisted);
            return std::unexpected(std::move(accepted.error()));
        }
    }
    // Step 10: quick_check before any write, so a damaged file is never migrated.
    if (options.verify == Verify::QuickCheck && !header->fresh) {
        if (auto checked = database.quickCheck(); !checked) {
            DatabaseAccess::abandon(database, walExisted);
            return std::unexpected(std::move(checked.error()));
        }
    }
    // Step 11: atomic creation of a fresh file's schema, still in rollback-journal mode.
    if (options.schema != nullptr && header->fresh) {
        if (auto created = detail::createFreshSchema(database, *options.schema); !created) {
            DatabaseAccess::abandon(database, walExisted);
            return std::unexpected(std::move(created.error()));
        }
    }
    // Steps 12 and 13: WAL and pragmas.
    if (auto applied = applyPragmas(*raw, options); !applied) {
        DatabaseAccess::abandon(database, false);
        return std::unexpected(std::move(applied.error()));
    }
    // Step 14: take the write lock now and keep it until close.
    if (options.exclusiveLock && options.mode != OpenMode::ReadOnly) {
        if (auto locked = detail::executeSql(*raw, "BEGIN EXCLUSIVE; COMMIT;"); !locked) {
            DatabaseAccess::abandon(database, false);
            return std::unexpected(std::move(locked.error()));
        }
        /*
         * In exclusive locking mode a rollback-journal commit (the fresh creation of step 11)
         * leaves a zeroed -journal behind instead of deleting it, and the switch to WAL keeps
         * it (SQLite's pager_end_transaction and sqlite3PagerSetJournalMode). It is not hot
         * (any hot journal was rolled back by step 9's read) and we hold the write lock, so it
         * is removed through the VFS; a failure only leaves the harmless file.
         */
        if (!inMemory && io::realFileSystem().status(path + "-journal")) {
            sqlite3_vfs* vfs = sqlite3_vfs_find(vfsName);
            const char* journal = sqlite3_filename_journal(sqlite3_db_filename(raw->db, "main"));
            if (vfs != nullptr && journal != nullptr && journal[0] != '\0') {
                static_cast<void>(vfs->xDelete(vfs, journal, 0));
            }
        }
    }
    // Step 15: pending migrations of an existing file.
    if (options.schema != nullptr && !header->fresh) {
        const auto newest = static_cast<std::int64_t>(options.schema->migrations.size());
        if (header->userVersion < newest) {
            if (auto migrated = detail::migrateExisting(database, *options.schema, header->userVersion); !migrated) {
                DatabaseAccess::abandon(database, false);
                return std::unexpected(std::move(migrated.error()));
            }
        }
    }
    return database;
}

// Removes the cached statement-free connection: the shared body of close() and ~Database.
int closeConnection(Connection& connection, bool reportBusy) noexcept {
    FROST_PRE(connection.liveStatements == 0 && connection.openTransactions == 0);
    if (connection.db == nullptr) {
        return SQLITE_OK;
    }
    int rc = reportBusy ? sqlite3_close(connection.db) : sqlite3_close_v2(connection.db);
    if (rc != SQLITE_OK) {
        // Only an unfinalized statement can make sqlite3_close fail; close_v2 defers instead.
        rc = sqlite3_close_v2(connection.db);
    }
    connection.db = nullptr;
    return rc;
}

} // namespace

void DatabaseAccess::abandon(Database& db, bool keepWal) noexcept {
    if (!db.m_connection) {
        return;
    }
    Connection& connection = *db.m_connection;
    if (keepWal && connection.db != nullptr) {
        static_cast<void>(sqlite3_db_config(connection.db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, nullptr));
    }
    if (connection.db != nullptr) {
        static_cast<void>(sqlite3_close_v2(connection.db));
        connection.db = nullptr;
    }
    db.m_connection.reset();
}

namespace detail {

Result<void> executeSql(Connection& connection, std::string_view sql) {
    sqlite3* db = connection.db;
    while (!sql.empty()) {
        if (sql.size() > static_cast<std::size_t>(INT_MAX)) {
            return fail(ErrorCode::OutOfRange, "SQL text longer than 2 GiB");
        }
        sqlite3_stmt* stmt = nullptr;
        const char* tail = nullptr;
        int rc = sqlite3_prepare_v3(db, sql.data(), static_cast<int>(sql.size()), 0, &stmt, &tail);
        if (rc != SQLITE_OK) {
            return std::unexpected(sqliteError(db, rc, "prepare", connection.path, sql));
        }
        const std::string_view current = sql.substr(0, static_cast<std::size_t>(tail - sql.data()));
        sql.remove_prefix(current.size());
        if (stmt == nullptr) {
            continue; // Whitespace or a comment.
        }
        do {
            rc = sqlite3_step(stmt);
        } while (rc == SQLITE_ROW);
        if (rc != SQLITE_DONE) {
            Error error = sqliteError(db, rc, "execute", connection.path, current);
            const bool interrupted = error.code() == ErrorCode::Busy && sqlite3_is_interrupted(db) != 0;
            static_cast<void>(sqlite3_finalize(stmt));
            if (interrupted) {
                return fail(ErrorCode::Cancelled, error.message());
            }
            return std::unexpected(std::move(error));
        }
        static_cast<void>(sqlite3_finalize(stmt));
    }
    return {};
}

namespace {

// Prepares and steps `sql` once, calling `read` on the first row (if any).
template<class Read>
Result<void> queryFirstRow(Connection& connection, std::string_view sql, Read read) {
    sqlite3* db = connection.db;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v3(db, sql.data(), static_cast<int>(sql.size()), 0, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(sqliteError(db, rc, "prepare", connection.path, sql));
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        read(stmt);
        // Finish the statement (PRAGMA journal_mode returns one row; drain for completeness).
        do {
            rc = sqlite3_step(stmt);
        } while (rc == SQLITE_ROW);
    }
    if (rc != SQLITE_DONE) {
        Error error = sqliteError(db, rc, "query", connection.path, sql);
        const bool interrupted = error.code() == ErrorCode::Busy && sqlite3_is_interrupted(db) != 0;
        static_cast<void>(sqlite3_finalize(stmt));
        if (interrupted) {
            return fail(ErrorCode::Cancelled, error.message());
        }
        return std::unexpected(std::move(error));
    }
    static_cast<void>(sqlite3_finalize(stmt));
    return {};
}

} // namespace

Result<std::int64_t> queryInt64(Connection& connection, std::string_view sql) {
    std::int64_t value = 0;
    if (auto done = queryFirstRow(connection, sql, [&](sqlite3_stmt* stmt) { value = sqlite3_column_int64(stmt, 0); });
        !done) {
        return std::unexpected(std::move(done.error()));
    }
    return value;
}

Result<std::string> queryText(Connection& connection, std::string_view sql) {
    std::string value;
    if (auto done = queryFirstRow(connection, sql,
                                  [&](sqlite3_stmt* stmt) {
                                      const unsigned char* text = sqlite3_column_text(stmt, 0);
                                      const int bytes = sqlite3_column_bytes(stmt, 0);
                                      if (text != nullptr && bytes > 0) {
                                          value.assign(reinterpret_cast<const char*>(text),
                                                       static_cast<std::size_t>(bytes));
                                      }
                                  });
        !done) {
        return std::unexpected(std::move(done.error()));
    }
    return value;
}

} // namespace detail

// ---------------------------------------------------------------------------------------------
// InterruptHandle

void InterruptHandle::interrupt() const noexcept {
    if (m_db != nullptr) {
        sqlite3_interrupt(m_db);
    }
}

// ---------------------------------------------------------------------------------------------
// Database

Database::Database(std::unique_ptr<detail::Connection> connection) noexcept : m_connection(std::move(connection)) {}

Database::Database(Database&& other) noexcept = default;

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        if (m_connection) {
            static_cast<void>(closeConnection(*m_connection, false));
        }
        m_connection = std::move(other.m_connection);
    }
    return *this;
}

Database::~Database() {
    if (m_connection) {
        static_cast<void>(closeConnection(*m_connection, false));
    }
}

Result<Database> Database::open(std::string_view path, const OpenOptions& options) {
    return openImpl(path, options, false);
}

Result<Database> Database::openInMemory(const OpenOptions& options) { return openImpl({}, options, true); }

Result<void> Database::close() {
    FROST_PRE(m_connection != nullptr);
    const std::string path = m_connection->path;
    const int rc = closeConnection(*m_connection, true);
    m_connection.reset();
    if (rc != SQLITE_OK) {
        return std::unexpected(detail::sqliteError(nullptr, rc, "close database", path));
    }
    return {};
}

Result<Statement> Database::prepare(std::string_view sql) {
    FROST_PRE(m_connection != nullptr);
    detail::Connection& connection = *m_connection;
    if (sql.size() > static_cast<std::size_t>(INT_MAX)) {
        return fail(ErrorCode::OutOfRange, "SQL text longer than 2 GiB");
    }
    sqlite3_stmt* stmt = nullptr;
    const char* tail = nullptr;
    const int rc = sqlite3_prepare_v3(connection.db, sql.data(), static_cast<int>(sql.size()), 0, &stmt, &tail);
    if (rc != SQLITE_OK) {
        return std::unexpected(detail::sqliteError(connection.db, rc, "prepare", connection.path, sql));
    }
    if (stmt == nullptr) {
        return fail(ErrorCode::InvalidArgument, std::format("prepare: no SQL statement in '{}'", sql));
    }
    Statement statement(&connection, stmt);
    // Only whitespace and comments may follow: preparing the rest must yield no statement.
    const std::string_view rest = sql.substr(static_cast<std::size_t>(tail - sql.data()));
    if (!rest.empty()) {
        sqlite3_stmt* extra = nullptr;
        const int restRc =
            sqlite3_prepare_v3(connection.db, rest.data(), static_cast<int>(rest.size()), 0, &extra, nullptr);
        if (extra != nullptr) {
            static_cast<void>(sqlite3_finalize(extra));
        }
        if (restRc != SQLITE_OK || extra != nullptr) {
            return fail(ErrorCode::InvalidArgument, std::format("prepare: more than one SQL statement in '{}'", sql));
        }
    }
    return statement;
}

Result<void> Database::execute(std::string_view sql) {
    FROST_PRE(m_connection != nullptr);
    return detail::executeSql(*m_connection, sql);
}

Result<Transaction> Database::begin(TransactionKind kind) {
    FROST_PRE(m_connection != nullptr);
    detail::Connection& connection = *m_connection;
    if (connection.openTransactions > 0 || sqlite3_get_autocommit(connection.db) == 0) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("begin transaction '{}': a transaction is already open", connection.path));
    }
    const char* sql = kind == TransactionKind::Deferred    ? "BEGIN DEFERRED"
                      : kind == TransactionKind::Immediate ? "BEGIN IMMEDIATE"
                                                           : "BEGIN EXCLUSIVE";
    if (auto begun = detail::executeSql(connection, sql); !begun) {
        return std::unexpected(std::move(begun.error()));
    }
    ++connection.openTransactions;
    return Transaction(&connection);
}

Result<void> Database::inTransaction(TransactionKind kind, frost::function_ref<Result<void>()> body) {
    auto transaction = begin(kind);
    if (!transaction) {
        return std::unexpected(std::move(transaction.error()));
    }
    if (auto ran = body(); !ran) {
        transaction->rollback();
        return ran;
    }
    return transaction->commit();
}

Result<CheckpointResult> Database::checkpoint(CheckpointMode mode) {
    FROST_PRE(m_connection != nullptr);
    detail::Connection& connection = *m_connection;
    int sqliteMode = SQLITE_CHECKPOINT_PASSIVE;
    switch (mode) {
    case CheckpointMode::Passive: sqliteMode = SQLITE_CHECKPOINT_PASSIVE; break;
    case CheckpointMode::Full: sqliteMode = SQLITE_CHECKPOINT_FULL; break;
    case CheckpointMode::Restart: sqliteMode = SQLITE_CHECKPOINT_RESTART; break;
    case CheckpointMode::Truncate: sqliteMode = SQLITE_CHECKPOINT_TRUNCATE; break;
    }
    int walFrames = -1;
    int checkpointed = -1;
    const int rc = sqlite3_wal_checkpoint_v2(connection.db, nullptr, sqliteMode, &walFrames, &checkpointed);
    CheckpointResult result{walFrames, checkpointed, walFrames == checkpointed};
    if (rc == SQLITE_BUSY) {
        result.complete = false;
        return result;
    }
    if (rc != SQLITE_OK) {
        return std::unexpected(detail::sqliteError(connection.db, rc, "checkpoint", connection.path));
    }
    return result;
}

namespace {

// Runs an integrity pragma; "ok" means healthy, anything else is Corrupt with the first rows.
Result<void> runCheck(Database& db, std::string_view pragma, std::stop_token stop) {
    if (stop.stop_requested()) {
        return fail(ErrorCode::Cancelled, "integrity check cancelled");
    }
    const InterruptHandle handle = db.interruptHandle();
    std::stop_callback onStop(stop, [handle] { handle.interrupt(); });
    auto statement = db.prepare(pragma);
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<std::string> problems;
    while (true) {
        auto row = statement->step();
        if (!row) {
            if (stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "integrity check cancelled");
            }
            return std::unexpected(std::move(row.error()));
        }
        if (!*row) {
            break;
        }
        std::string text(statement->columnText(0));
        if (text != "ok" && problems.size() < 10) {
            problems.push_back(std::move(text));
        }
    }
    if (stop.stop_requested()) {
        return fail(ErrorCode::Cancelled, "integrity check cancelled");
    }
    if (!problems.empty()) {
        std::string message = std::format("{} '{}' found problems:", pragma, db.path());
        for (const std::string& problem : problems) {
            message += " [";
            message += problem;
            message += ']';
        }
        return fail(ErrorCode::Corrupt, std::move(message));
    }
    return {};
}

} // namespace

Result<void> Database::quickCheck(std::stop_token stop) {
    FROST_PRE(m_connection != nullptr);
    return runCheck(*this, "PRAGMA quick_check(10)", std::move(stop));
}

Result<void> Database::integrityCheck(std::stop_token stop) {
    FROST_PRE(m_connection != nullptr);
    if (auto checked = runCheck(*this, "PRAGMA integrity_check(10)", stop); !checked) {
        return checked;
    }
    // foreign_key_check returns one row per violation (table, rowid, parent, fkid); none is healthy.
    auto statement = prepare("PRAGMA foreign_key_check");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    auto row = statement->step();
    if (!row) {
        return std::unexpected(std::move(row.error()));
    }
    if (*row) {
        return fail(ErrorCode::Corrupt,
                    std::format("foreign_key_check '{}': row {} of table '{}' references a missing row of '{}'", path(),
                                statement->columnInt64(1), statement->columnText(0), statement->columnText(2)));
    }
    return {};
}

std::int64_t Database::lastInsertRowId() const noexcept {
    FROST_PRE(m_connection != nullptr);
    return sqlite3_last_insert_rowid(m_connection->db);
}

std::int64_t Database::changes() const noexcept {
    FROST_PRE(m_connection != nullptr);
    return sqlite3_changes64(m_connection->db);
}

const std::string& Database::path() const noexcept {
    FROST_PRE(m_connection != nullptr);
    return m_connection->path;
}

InterruptHandle Database::interruptHandle() const noexcept {
    FROST_PRE(m_connection != nullptr);
    return InterruptHandle(m_connection->db);
}

sqlite3* Database::nativeHandle() const noexcept {
    FROST_PRE(m_connection != nullptr);
    return m_connection->db;
}

// ---------------------------------------------------------------------------------------------
// Transaction

Transaction::Transaction(Transaction&& other) noexcept : m_connection(std::exchange(other.m_connection, nullptr)) {}

Transaction& Transaction::operator=(Transaction&& other) noexcept {
    if (this != &other) {
        rollback();
        m_connection = std::exchange(other.m_connection, nullptr);
    }
    return *this;
}

Transaction::~Transaction() { rollback(); }

void Transaction::finish() noexcept {
    --m_connection->openTransactions;
    m_connection = nullptr;
}

Result<void> Transaction::commit() {
    FROST_PRE(m_connection != nullptr);
    auto committed = detail::executeSql(*m_connection, "COMMIT");
    if (committed) {
        finish();
        return {};
    }
    // Busy keeps the transaction open; any other failure may have rolled it back already.
    if (sqlite3_get_autocommit(m_connection->db) != 0) {
        finish();
    }
    return committed;
}

void Transaction::rollback() noexcept {
    if (m_connection == nullptr) {
        return;
    }
    if (sqlite3_get_autocommit(m_connection->db) == 0) {
        // ROLLBACK fails only when no transaction is open, which autocommit just ruled out.
        static_cast<void>(detail::executeSql(*m_connection, "ROLLBACK"));
    }
    finish();
}

} // namespace frost::storage
