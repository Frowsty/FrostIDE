// SPDX-License-Identifier: Apache-2.0
// CPP-8 existing SQLite storage owns atomic per-command generations, never collapsed by filename.
#include "frost/io/file_system.hpp"
#include "frost/lang/cpp_index.hpp"
#include "frost/storage/schema.hpp"
#include "frost/storage/statement.hpp"

#include <algorithm>
#include <map>
#include <variant>

namespace frost::lang {
namespace {
constexpr storage::Migration migrations[]{{1, R"sql(
CREATE TABLE units(command TEXT PRIMARY KEY, configuration TEXT NOT NULL, source TEXT NOT NULL,
 database_hash TEXT NOT NULL, generation INTEGER NOT NULL, receipt TEXT NOT NULL);
CREATE TABLE symbols(command TEXT NOT NULL, usr TEXT NOT NULL, name TEXT NOT NULL,
 qualified TEXT NOT NULL, kind TEXT NOT NULL, PRIMARY KEY(command,usr));
CREATE INDEX symbols_name ON symbols(name);
CREATE TABLE occurrences(command TEXT NOT NULL, ordinal INTEGER NOT NULL, usr TEXT NOT NULL,
 path TEXT NOT NULL, offset INTEGER NOT NULL, line INTEGER NOT NULL, column INTEGER NOT NULL,
 roles INTEGER NOT NULL, PRIMARY KEY(command,ordinal));
CREATE INDEX occurrences_usr ON occurrences(usr,command);
CREATE TABLE relations(command TEXT NOT NULL, ordinal INTEGER NOT NULL, source TEXT NOT NULL,
 target TEXT NOT NULL, roles INTEGER NOT NULL);
CREATE INDEX relations_source ON relations(source,command);
CREATE INDEX relations_target ON relations(target,command);
)sql"}};
constexpr storage::SchemaSpec schema{"C++ configuration index", 0x46434958, {}, migrations};
using Value = std::variant<std::string_view, std::int64_t>;
Result<void> execute(storage::Database& database, std::string_view sql, std::initializer_list<Value> values) {
    auto statement = database.prepare(sql);
    if (!statement)
        return std::unexpected(statement.error());
    int index = 1;
    for (const auto& value : values) {
        auto bound = std::holds_alternative<std::string_view>(value)
                         ? statement->bindText(index++, std::get<std::string_view>(value))
                         : statement->bindInt64(index++, std::get<std::int64_t>(value));
        if (!bound)
            return bound;
    }
    auto step = statement->step();
    if (!step)
        return std::unexpected(step.error());
    return {};
}
Result<void> erase(storage::Database& database, std::string_view command) {
    for (const auto sql : {"DELETE FROM relations WHERE command=?1", "DELETE FROM occurrences WHERE command=?1",
                           "DELETE FROM symbols WHERE command=?1", "DELETE FROM units WHERE command=?1"}) {
        auto result = execute(database, sql, {command});
        if (!result)
            return result;
    }
    return {};
}
Result<bool> fresh(storage::Database& database, std::string_view command, std::stop_token stop,
                   std::vector<CppIndexUnit>& receipts, std::size_t& receiptBytes) {
    auto statement = database.prepare("SELECT receipt FROM units WHERE command=?1");
    if (!statement)
        return std::unexpected(statement.error());
    if (auto bound = statement->bindText(1, command); !bound)
        return std::unexpected(bound.error());
    auto row = statement->step();
    if (!row)
        return std::unexpected(row.error());
    if (!*row)
        return false;
    const auto bytes = statement->columnText(0);
    if (bytes.size() > 32U * 1024U * 1024U - receiptBytes)
        return fail(ErrorCode::OutOfRange, "Index query dependency receipts exceed32MiB; narrow the query");
    auto unit = parseCppIndexUnit(bytes);
    if (!unit)
        return std::unexpected(unit.error());
    auto valid = validateCppIndexInputs(io::realFileSystem(), *unit, {}, stop);
    if (!valid && valid.error().code() == ErrorCode::Cancelled)
        return std::unexpected(valid.error());
    if (valid) {
        receiptBytes += bytes.size();
        receipts.push_back(std::move(*unit));
    }
    return valid.has_value();
}
std::int64_t integer(const Json& row, std::string_view key) {
    return static_cast<std::int64_t>(*row.find(key)->asUnsigned());
}
std::string likePrefix(std::string_view prefix) {
    std::string result;
    for (const char character : prefix) {
        if (character == '%' || character == '_' || character == '\\')
            result += '\\';
        result += character;
    }
    return result + '%';
}
} // namespace
Result<CppIndexStore> CppIndexStore::open(std::string_view path) {
    storage::OpenOptions options;
    options.schema = &schema;
    auto database = storage::Database::open(path, options);
    if (!database)
        return std::unexpected(database.error());
    return CppIndexStore(std::move(*database));
}
Result<void> CppIndexStore::publish(const CppIndexUnit& unit, std::uint64_t generation, std::stop_token stop) {
    if (generation > INT64_MAX)
        return fail(ErrorCode::OutOfRange, "Index generation exceeds storage range");
    // Persistent generations describe disk bytes. Dirty-buffer analysis can be requested for other
    // consumers, but cannot overwrite the durable index with an unsaved editor-only generation.
    if (auto valid = validateCppIndexInputs(io::realFileSystem(), unit, {}, stop); !valid)
        return valid;
    const auto handle = database.interruptHandle();
    std::stop_callback interrupt(stop, [handle] { handle.interrupt(); });
    return database.inTransaction(storage::TransactionKind::Immediate, [&]() -> Result<void> {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Index publication cancelled");
        if (auto removed = erase(database, unit.commandIdentity); !removed)
            return removed;
        auto receipt = Json(unit.data)
                           .set("symbols", Json::array())
                           .set("occurrences", Json::array())
                           .set("relations", Json::array())
                           .dump();
        if (auto inserted = execute(database, "INSERT INTO units VALUES(?1,?2,?3,?4,?5,?6)",
                                    {unit.commandIdentity, unit.configuration, unit.source, unit.databaseSha256,
                                     static_cast<std::int64_t>(generation), receipt});
            !inserted)
            return inserted;
        for (const auto& symbol : unit.data.find("symbols")->items()) {
            if (stop.stop_requested())
                return fail(ErrorCode::Cancelled, "Index publication cancelled");
            if (auto inserted = execute(database, "INSERT INTO symbols VALUES(?1,?2,?3,?4,?5)",
                                        {unit.commandIdentity, symbol.stringAt("usr"), symbol.stringAt("name"),
                                         symbol.stringAt("qualifiedName"), symbol.stringAt("kind")});
                !inserted)
                return inserted;
        }
        std::int64_t ordinal = 0;
        for (const auto& occurrence : unit.data.find("occurrences")->items()) {
            if (stop.stop_requested())
                return fail(ErrorCode::Cancelled, "Index publication cancelled");
            if (auto inserted =
                    execute(database, "INSERT INTO occurrences VALUES(?1,?2,?3,?4,?5,?6,?7,?8)",
                            {unit.commandIdentity, ordinal++, occurrence.stringAt("usr"), occurrence.stringAt("path"),
                             integer(occurrence, "offset"), integer(occurrence, "line"), integer(occurrence, "column"),
                             integer(occurrence, "roles")});
                !inserted)
                return inserted;
        }
        for (const auto& relation : unit.data.find("relations")->items()) {
            if (stop.stop_requested())
                return fail(ErrorCode::Cancelled, "Index publication cancelled");
            if (auto inserted =
                    execute(database, "INSERT INTO relations VALUES(?1,?2,?3,?4,?5)",
                            {unit.commandIdentity, integer(relation, "occurrence"), relation.stringAt("source"),
                             relation.stringAt("target"), integer(relation, "roles")});
                !inserted)
                return inserted;
        }
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Index publication cancelled before commit");
        return {};
    });
}
Result<void> CppIndexStore::invalidate(std::string_view commandIdentity) {
    return database.inTransaction(storage::TransactionKind::Immediate,
                                  [&] { return erase(database, commandIdentity); });
}
Result<void> CppIndexStore::clear() {
    return database.inTransaction(storage::TransactionKind::Immediate, [&] {
        return database.execute("DELETE FROM relations;DELETE FROM occurrences;DELETE FROM symbols;DELETE FROM units;");
    });
}
Result<CppIndexQuery> CppIndexStore::symbols(std::string_view prefix, std::string_view hash,
                                             std::string_view configuration, std::size_t limit, std::stop_token stop) {
    return query(likePrefix(prefix), hash, configuration, limit, false, stop);
}
Result<std::size_t> CppIndexStore::storedCommands(std::string_view hash) {
    auto statement = database.prepare("SELECT COUNT(*) FROM units WHERE database_hash=?1");
    if (!statement)
        return std::unexpected(statement.error());
    if (auto bound = statement->bindText(1, hash); !bound)
        return std::unexpected(bound.error());
    auto row = statement->step();
    if (!row)
        return std::unexpected(row.error());
    return *row ? static_cast<std::size_t>(statement->columnInt64(0)) : 0;
}
Result<CppIndexQuery> CppIndexStore::occurrences(std::string_view usr, std::string_view hash,
                                                 std::string_view configuration, std::size_t limit,
                                                 std::stop_token stop) {
    return query(usr, hash, configuration, limit, true, stop);
}
Result<CppIndexQuery> CppIndexStore::query(std::string_view value, std::string_view hash,
                                           std::string_view configuration, std::size_t limit, bool usages,
                                           std::stop_token stop) {
    if (hash.size() != 64 || !limit || limit > 1000)
        return fail(ErrorCode::InvalidArgument, "Index query needs current CDB digest and a limit of1..1000");
    const auto sql =
        usages ? "SELECT o.command,u.configuration,o.usr,s.qualified,s.kind,o.path,o.offset,o.line,o.column,o.roles "
                 "FROM occurrences o JOIN units u ON u.command=o.command JOIN symbols s ON s.command=o.command AND "
                 "s.usr=o.usr WHERE o.usr=?1 AND u.database_hash=?2 AND (?3='' OR u.configuration=?3) ORDER BY "
                 "u.configuration,o.path,o.offset LIMIT ?4"
               : "SELECT o.command,u.configuration,o.usr,s.qualified,s.kind,o.path,o.offset,o.line,o.column,o.roles "
                 "FROM symbols s JOIN units u ON u.command=s.command JOIN occurrences o ON o.command=s.command AND "
                 "o.usr=s.usr WHERE s.name LIKE ?1 ESCAPE '\\' AND (o.roles&3)!=0 AND u.database_hash=?2 AND (?3='' OR "
                 "u.configuration=?3) ORDER BY s.name,u.configuration,o.path,o.offset LIMIT ?4";
    const auto handle = database.interruptHandle();
    std::stop_callback interrupt(stop, [handle] { handle.interrupt(); });
    auto statement = database.prepare(sql);
    if (!statement)
        return std::unexpected(statement.error());
    if (auto bound = statement->bindText(1, value); !bound)
        return std::unexpected(bound.error());
    if (auto bound = statement->bindText(2, hash); !bound)
        return std::unexpected(bound.error());
    if (auto bound = statement->bindText(3, configuration); !bound)
        return std::unexpected(bound.error());
    if (auto bound = statement->bindInt64(4, static_cast<std::int64_t>(limit + 1)); !bound)
        return std::unexpected(bound.error());
    CppIndexQuery result;
    std::map<std::string, bool> freshness;
    std::size_t visited = 0, receiptBytes = 0;
    for (;;) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Index query cancelled");
        auto row = statement->step();
        if (!row)
            return std::unexpected(row.error());
        if (!*row)
            break;
        if (++visited > limit) {
            result.truncated = true;
            break;
        }
        const auto command = std::string(statement->columnText(0));
        auto [position, inserted] = freshness.emplace(command, false);
        if (inserted) {
            auto valid = fresh(database, command, stop, result.receipts, receiptBytes);
            if (!valid)
                return std::unexpected(valid.error());
            position->second = *valid;
            if (!*valid)
                ++result.staleCommands;
        }
        if (!position->second)
            continue;
        result.locations.push_back({command, std::string(statement->columnText(1)),
                                    std::string(statement->columnText(2)), std::string(statement->columnText(3)),
                                    std::string(statement->columnText(4)), std::string(statement->columnText(5)),
                                    static_cast<std::uint64_t>(statement->columnInt64(6)),
                                    static_cast<std::uint64_t>(statement->columnInt64(7)),
                                    static_cast<std::uint64_t>(statement->columnInt64(8)),
                                    static_cast<std::uint64_t>(statement->columnInt64(9))});
    }
    return result;
}
Result<Json> CppIndexStore::relations(std::string_view usr, std::string_view hash, std::string_view configuration,
                                      std::size_t limit, std::stop_token stop) {
    if (hash.size() != 64 || !limit || limit > 1000)
        return fail(ErrorCode::InvalidArgument, "Invalid relation query");
    const auto handle = database.interruptHandle();
    std::stop_callback interrupt(stop, [handle] { handle.interrupt(); });
    auto statement =
        database.prepare("SELECT "
                         "r.command,u.configuration,r.source,r.target,r.roles,s.qualified,t.qualified,o.path,o.offset,"
                         "o.line,o.column FROM relations r JOIN units u ON "
                         "u.command=r.command JOIN symbols s ON s.command=r.command AND s.usr=r.source JOIN symbols t "
                         "ON t.command=r.command AND t.usr=r.target JOIN occurrences o ON o.command=r.command AND "
                         "o.ordinal=r.ordinal WHERE (r.source=?1 OR r.target=?1) AND u.database_hash=?2 AND (?3='' OR "
                         "u.configuration=?3) ORDER BY u.configuration,r.source,r.target LIMIT ?4");
    if (!statement)
        return std::unexpected(statement.error());
    if (auto bound = statement->bindText(1, usr); !bound)
        return std::unexpected(bound.error());
    if (auto bound = statement->bindText(2, hash); !bound)
        return std::unexpected(bound.error());
    if (auto bound = statement->bindText(3, configuration); !bound)
        return std::unexpected(bound.error());
    if (auto bound = statement->bindInt64(4, static_cast<std::int64_t>(limit + 1)); !bound)
        return std::unexpected(bound.error());
    std::map<std::string, bool> freshness;
    std::vector<Json> rows;
    std::vector<CppIndexUnit> receipts;
    std::size_t visited = 0, stale = 0, receiptBytes = 0;
    bool truncated = false;
    for (;;) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Relation query cancelled");
        auto row = statement->step();
        if (!row)
            return std::unexpected(row.error());
        if (!*row)
            break;
        if (++visited > limit) {
            truncated = true;
            break;
        }
        const auto command = std::string(statement->columnText(0));
        auto [position, inserted] = freshness.emplace(command, false);
        if (inserted) {
            auto valid = fresh(database, command, stop, receipts, receiptBytes);
            if (!valid)
                return std::unexpected(valid.error());
            position->second = *valid;
            if (!*valid)
                ++stale;
        }
        if (!position->second)
            continue;
        rows.push_back(Json::object()
                           .set("commandIdentity", command)
                           .set("configuration", std::string(statement->columnText(1)))
                           .set("source", std::string(statement->columnText(2)))
                           .set("target", std::string(statement->columnText(3)))
                           .set("roles", statement->columnInt64(4))
                           .set("sourceName", std::string(statement->columnText(5)))
                           .set("targetName", std::string(statement->columnText(6)))
                           .set("path", std::string(statement->columnText(7)))
                           .set("offset", statement->columnInt64(8))
                           .set("line", statement->columnInt64(9))
                           .set("column", statement->columnInt64(10)));
    }
    std::vector<Json> receiptRows;
    for (auto& unit : receipts)
        receiptRows.push_back(std::move(unit.data));
    return Json::object()
        .set("receipts", Json::array(std::move(receiptRows)))
        .set("rows", Json::array(std::move(rows)))
        .set("staleCommands", stale)
        .set("truncated", truncated);
}
} // namespace frost::lang
