// SPDX-License-Identifier: Apache-2.0
//
// Schema validation, atomic creation, migrations and describeSchema (see
// frost/storage/schema.hpp for the rules).

#include "frost/storage/schema.hpp"

#include "frost/core/assert.hpp"
#include "frost/storage/database.hpp"

#include "database_internal.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace frost::storage {

namespace detail {

void checkSchemaSpec(const SchemaSpec& spec) noexcept {
    // PRAGMA application_id is a signed 32-bit value; positive ids read back unchanged.
    FROST_PRE(spec.applicationId != 0 && spec.applicationId <= 0x7FFFFFFFu);
    FROST_PRE(!spec.migrations.empty());
    for (std::size_t i = 0; i < spec.migrations.size(); ++i) {
        FROST_PRE(spec.migrations[i].version == i + 1);
    }
}

/*
 * The four values come from one read transaction (one WAL snapshot; in rollback mode the SHARED
 * lock keeps writers out until COMMIT). Read one by one, a concurrent first open could commit its
 * creation between them, and application_id 0 beside the new tables would look like a foreign file.
 */
Result<HeaderInfo> readHeader(Connection& connection) {
    HeaderInfo header;
    struct Query {
        std::int64_t* out;
        std::string_view sql;
    };
    const Query queries[] = {
        {&header.applicationId, "PRAGMA application_id"},
        {&header.userVersion, "PRAGMA user_version"},
        {&header.pageCount, "PRAGMA page_count"},
        {&header.schemaRows, "SELECT count(*) FROM sqlite_schema"},
    };
    if (auto begun = executeSql(connection, "BEGIN DEFERRED"); !begun) {
        return std::unexpected(std::move(begun.error()));
    }
    for (const Query& query : queries) {
        auto value = queryInt64(connection, query.sql);
        if (!value) {
            static_cast<void>(executeSql(connection, "ROLLBACK"));
            return std::unexpected(std::move(value.error()));
        }
        *query.out = *value;
    }
    if (auto ended = executeSql(connection, "COMMIT"); !ended) {
        static_cast<void>(executeSql(connection, "ROLLBACK"));
        return std::unexpected(std::move(ended.error()));
    }
    header.fresh =
        header.pageCount == 0 || (header.applicationId == 0 && header.userVersion == 0 && header.schemaRows == 0);
    return header;
}

Result<void> checkHeaderAgainstSpec(const HeaderInfo& header, const SchemaSpec& spec, OpenMode mode, bool allowFresh,
                                    std::string_view path) {
    const auto newest = static_cast<std::int64_t>(spec.migrations.size());
    if (header.fresh) {
        if (!allowFresh) {
            return fail(ErrorCode::NotFound, std::format("open {} '{}': empty database", spec.name, path));
        }
        if (mode == OpenMode::ReadOnly) {
            return fail(
                ErrorCode::Unsupported,
                std::format("open {} '{}': empty database needs creation; open read-write first", spec.name, path));
        }
        return {};
    }
    if (header.applicationId != static_cast<std::int64_t>(spec.applicationId)) {
        return fail(ErrorCode::Corrupt,
                    std::format("open {} '{}': not a {} database (application_id {:#010x})", spec.name, path, spec.name,
                                static_cast<std::uint32_t>(header.applicationId)));
    }
    if (header.userVersion > newest) {
        return fail(ErrorCode::Unsupported,
                    std::format("open {} '{}': schema version {} is newer than this FrostIDE's {}", spec.name, path,
                                header.userVersion, newest));
    }
    // Creation sets application_id and user_version in one transaction, so a non-fresh file with
    // our id and version 0 (or a negative one) was damaged, not left half-made.
    if (header.userVersion <= 0) {
        return fail(ErrorCode::Corrupt, std::format("open {} '{}': application_id without a valid schema version ({})",
                                                    spec.name, path, header.userVersion));
    }
    if (mode == OpenMode::ReadOnly && header.userVersion < newest) {
        return fail(ErrorCode::Unsupported,
                    std::format("open {} '{}': schema version {} needs migration to {}; open read-write first",
                                spec.name, path, header.userVersion, newest));
    }
    return {};
}

namespace {

// Runs migrations (from, newest] inside the caller's transaction: sql, then apply.
Result<void> runMigrations(Database& db, const SchemaSpec& spec, std::int64_t from, std::int64_t to) {
    for (std::int64_t version = from + 1; version <= to; ++version) {
        const Migration& migration = spec.migrations[static_cast<std::size_t>(version - 1)];
        if (!migration.sql.empty()) {
            if (auto ran = db.execute(migration.sql); !ran) {
                return std::unexpected(
                    std::move(ran.error()).withContext(std::format("{} migration {}", spec.name, version)));
            }
        }
        if (migration.apply != nullptr) {
            if (auto applied = migration.apply(db); !applied) {
                return std::unexpected(
                    std::move(applied.error()).withContext(std::format("{} migration {}", spec.name, version)));
            }
        }
    }
    return {};
}

// Re-reads the header inside a write transaction and checks it is still ours and not newer.
Result<std::int64_t> recheckVersion(Database& db, const SchemaSpec& spec) {
    Connection& connection = DatabaseAccess::connection(db);
    auto version = queryInt64(connection, "PRAGMA user_version");
    if (!version) {
        return std::unexpected(std::move(version.error()));
    }
    auto applicationId = queryInt64(connection, "PRAGMA application_id");
    if (!applicationId) {
        return std::unexpected(std::move(applicationId.error()));
    }
    const auto newest = static_cast<std::int64_t>(spec.migrations.size());
    if (*version > 0 && *applicationId != static_cast<std::int64_t>(spec.applicationId)) {
        return fail(ErrorCode::Corrupt,
                    std::format("open {} '{}': not a {} database (application_id {:#010x})", spec.name, db.path(),
                                spec.name, static_cast<std::uint32_t>(*applicationId)));
    }
    if (*version > newest) {
        return fail(ErrorCode::Unsupported,
                    std::format("open {} '{}': schema version {} is newer than this FrostIDE's {}", spec.name,
                                db.path(), *version, newest));
    }
    return *version;
}

} // namespace

Result<void> createFreshSchema(Database& db, const SchemaSpec& spec) {
    if (!spec.freshPragmas.empty()) {
        /*
         * A racing first open may have created the store since open() read the header, so it is
         * read again: only a file that is still fresh gets the pragmas and the VACUUM, which would
         * otherwise rewrite a populated store for nothing. Losing the race after this re-read costs
         * at most that rewrite (VACUUM keeps the content); the transaction below re-checks anyway.
         */
        auto current = readHeader(DatabaseAccess::connection(db));
        if (!current) {
            return std::unexpected(std::move(current.error()));
        }
        if (current->fresh) {
            if (auto applied = db.execute(spec.freshPragmas); !applied) {
                return applied;
            }
        }
        // auto_vacuum and page_size cannot change once page 1 exists, except through VACUUM,
        // which is instant on a file without tables.
        if (current->fresh && current->pageCount > 0) {
            if (auto vacuumed = db.execute("VACUUM"); !vacuumed) {
                return vacuumed;
            }
        }
    }
    const auto newest = static_cast<std::int64_t>(spec.migrations.size());
    return db.inTransaction(TransactionKind::Immediate, [&]() -> Result<void> {
        // Another connection may have created (part of) the schema since the header was read.
        auto version = recheckVersion(db, spec);
        if (!version) {
            return std::unexpected(std::move(version.error()));
        }
        if (*version == newest) {
            return {};
        }
        if (auto migrated = runMigrations(db, spec, *version, newest); !migrated) {
            return migrated;
        }
        return db.execute(std::format("PRAGMA application_id={}; PRAGMA user_version={};",
                                      static_cast<std::int32_t>(spec.applicationId), newest));
    });
}

Result<void> migrateExisting(Database& db, const SchemaSpec& spec, std::int64_t fromVersion) {
    const auto newest = static_cast<std::int64_t>(spec.migrations.size());
    for (std::int64_t target = fromVersion + 1; target <= newest; ++target) {
        auto stepped = db.inTransaction(TransactionKind::Immediate, [&]() -> Result<void> {
            auto version = recheckVersion(db, spec);
            if (!version) {
                return std::unexpected(std::move(version.error()));
            }
            if (*version >= target) {
                return {}; // Another connection applied this step already.
            }
            if (auto migrated = runMigrations(db, spec, target - 1, target); !migrated) {
                return migrated;
            }
            return db.execute(std::format("PRAGMA user_version={};", target));
        });
        if (!stepped) {
            return stepped;
        }
    }
    return {};
}

} // namespace detail

Result<std::string> describeSchema(Database& db) {
    detail::Connection& connection = DatabaseAccess::connection(db);
    std::string out;
    for (const std::string_view pragma : {"application_id", "user_version", "auto_vacuum", "journal_mode"}) {
        auto value = detail::queryText(connection, std::format("PRAGMA {}", pragma));
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        out += std::format("{}={}\n", pragma, *value);
    }
    auto statement =
        db.prepare("SELECT type, name, tbl_name, sql FROM sqlite_schema WHERE name NOT LIKE 'sqlite\\_%' ESCAPE '\\'");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<std::tuple<std::string, std::string, std::string, std::string>> rows;
    while (true) {
        auto row = statement->step();
        if (!row) {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row) {
            break;
        }
        rows.emplace_back(statement->columnText(0), statement->columnText(1), statement->columnText(2),
                          statement->columnIsNull(3) ? std::string("NULL") : std::string(statement->columnText(3)));
    }
    std::ranges::sort(rows, [](const auto& a, const auto& b) {
        return std::tie(std::get<0>(a), std::get<1>(a)) < std::tie(std::get<0>(b), std::get<1>(b));
    });
    for (const auto& [type, name, table, sql] : rows) {
        out += std::format("\n{} {} on {}\n{}\n", type, name, table, sql);
    }
    return out;
}

} // namespace frost::storage
