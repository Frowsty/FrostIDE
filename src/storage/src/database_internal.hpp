// SPDX-License-Identifier: Apache-2.0
//
// Private: helpers shared by the open sequence (database.cpp), the schema rules (schema.cpp)
// and the stores: raw SQL execution on a Connection, single-value queries, and access to a
// Database's connection.
#pragma once

#include "frost/core/error.hpp"
#include "frost/storage/database.hpp"
#include "frost/storage/schema.hpp"

#include "connection.hpp"

#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace frost::storage {

// Friend of Database: builds one around a Connection and reaches its connection.
struct DatabaseAccess {
    [[nodiscard]] static Database make(std::unique_ptr<detail::Connection> connection) noexcept {
        return Database(std::move(connection));
    }
    // Precondition: `db` is open.
    [[nodiscard]] static detail::Connection& connection(Database& db) noexcept { return *db.m_connection; }
    /*
     * Closes `db` after a refusal. With `keepWal`, NO_CKPT_ON_CLOSE is set first, so neither the
     * main file nor the WAL is checkpointed or rewritten; without it SQLite's normal close runs,
     * which only removes the empty -wal and -shm a read of a WAL-mode file creates (nothing was
     * written, so the checkpoint has no frames to copy).
     */
    static void abandon(Database& db, bool keepWal) noexcept;
};

namespace detail {

// Runs every statement in `sql` to completion, discarding rows. Errors as Statement::step
// (Busy on an interrupted connection becomes Cancelled).
[[nodiscard]] Result<void> executeSql(Connection& connection, std::string_view sql);

// The first column of the first row of `sql` as an integer (0 when there is no row).
[[nodiscard]] Result<std::int64_t> queryInt64(Connection& connection, std::string_view sql);

// The first column of the first row of `sql` as text ("" when there is no row).
[[nodiscard]] Result<std::string> queryText(Connection& connection, std::string_view sql);

// What open() learned from a file's header (schema.cpp).
struct HeaderInfo {
    std::int64_t applicationId = 0;
    std::int64_t userVersion = 0;
    std::int64_t pageCount = 0;
    std::int64_t schemaRows = 0;
    bool fresh = false;
};

// Reads application_id, user_version, page_count and the sqlite_schema row count in one read
// transaction (one snapshot). Errors: those of the queries (Corrupt for NOTADB, Busy).
[[nodiscard]] Result<HeaderInfo> readHeader(Connection& connection);

// FROST_PRE checks of a SchemaSpec (non-zero id, migrations numbered 1..n).
void checkSchemaSpec(const SchemaSpec& spec) noexcept;

// The refusal rules of schema.hpp (step 2); returns the error to refuse with, if any.
[[nodiscard]] Result<void> checkHeaderAgainstSpec(const HeaderInfo& header, const SchemaSpec& spec, OpenMode mode,
                                                  bool allowFresh, std::string_view path);

// Creates the schema of a fresh file atomically (schema.hpp, step 4).
[[nodiscard]] Result<void> createFreshSchema(Database& db, const SchemaSpec& spec);

// Applies pending migrations to an existing file, one transaction per step (schema.hpp, step 5).
[[nodiscard]] Result<void> migrateExisting(Database& db, const SchemaSpec& spec, std::int64_t fromVersion);

} // namespace detail
} // namespace frost::storage
