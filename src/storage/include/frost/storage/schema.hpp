// SPDX-License-Identifier: Apache-2.0
//
// Schema versioning for frost::storage databases: a SchemaSpec names the file's owner
// (PRAGMA application_id) and lists its migrations (PRAGMA user_version counts them), and
// describeSchema() renders a canonical dump for golden tests (ADR 0014).
//
/*
 * Database::open with OpenOptions::schema runs these rules, in this order, so a file that is
 * refused is never modified:
 *   1. Read application_id, user_version, page_count and the number of sqlite_schema rows.
 *      A file is fresh when it has no pages, or pages but application_id 0, user_version 0 and
 *      no schema rows (another tool or a crash inside SQLite's own first write can leave one).
 *   2. Refuse: a non-zero foreign application_id, or application_id 0 on a non-fresh file, is
 *      Corrupt ("not a <name> database"), and so is our application_id with a user_version of 0
 *      or below (creation sets both at once); a user_version above the newest migration is
 *      Unsupported (a newer FrostIDE's file); ReadOnly below the newest is Unsupported ("needs
 *      migration"); a fresh file with allowFresh=false is NotFound. The connection then closes
 *      without a checkpoint, so neither the file nor a -wal beside it is rewritten.
 *   3. With Verify::QuickCheck, a non-fresh file that fails quick_check is Corrupt, before any
 *      write, so a damaged file is never migrated.
 *   4. A fresh file runs freshPragmas (an empty-schema file also runs VACUUM, because
 *      auto_vacuum cannot change once page 1 exists), then one BEGIN IMMEDIATE transaction that
 *      re-reads user_version, runs every missing migration in order and sets application_id and
 *      user_version. Creation is atomic: a crash leaves an empty file (fresh again) or a complete
 *      newest-version file.
 *   5. An existing older file migrates one step per BEGIN IMMEDIATE transaction; each re-reads
 *      user_version and application_id first and skips a step another connection already
 *      applied. A failed step leaves the previous version. Concurrent opens migrate exactly once.
 */
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace frost::storage {

class Database;

// One schema step. Aggregate, constant-initialisable (no global constructors).
struct Migration {
    std::uint32_t version; // migrations[i].version == i + 1; version 1 creates the schema.
    std::string_view sql;  // Run with Database::execute inside the step's transaction.
    // Optional data step, run after `sql` in the same transaction; its error rolls the step back.
    Result<void> (*apply)(Database&) = nullptr;
};

/*
 * A store's schema. The spec and the memory its views name must outlive every open that uses
 * it (in practice they are constexpr globals). A malformed spec (application id 0, no
 * migrations, versions not 1..n) is a precondition violation (FROST_PRE in open).
 */
struct SchemaSpec {
    std::string_view name;                 // "hot-exit journal", used in messages.
    std::uint32_t applicationId;           // PRAGMA application_id; must be non-zero.
    std::string_view freshPragmas;         // Run only on a fresh file, before its first write.
    std::span<const Migration> migrations; // Non-empty.
};

/*
 * A canonical text dump for goldens and diagnostics: application_id, user_version, auto_vacuum
 * and journal_mode, then every sqlite_schema row (type, name, tbl_name, sql) sorted by type and
 * then name, without SQLite's internal sqlite_% objects. Errors: as Database::execute.
 */
[[nodiscard]] Result<std::string> describeSchema(Database& db);

} // namespace frost::storage
