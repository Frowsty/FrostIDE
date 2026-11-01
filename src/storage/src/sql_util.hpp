// SPDX-License-Identifier: Apache-2.0
//
// Private: small helpers for the stores' SQL: binding a parameter list in one call, preparing
// a statement with its parameters bound, and running a statement for its effect or its first
// integer. Parameters are numbered from 1 in argument order.
#pragma once

#include "frost/core/error.hpp"
#include "frost/storage/database.hpp"
#include "frost/storage/fingerprint.hpp"
#include "frost/storage/statement.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace frost::storage::detail {

// A BLOB parameter (copied when bound); a plain string_view binds TEXT.
struct Blob {
    std::string_view bytes;
};
// An SQL NULL parameter.
struct Null {};

inline Result<void> bindOne(Statement& statement, int index, std::int64_t value) {
    return statement.bindInt64(index, value);
}
inline Result<void> bindOne(Statement& statement, int index, std::string_view value) {
    return statement.bindText(index, value);
}
inline Result<void> bindOne(Statement& statement, int index, Blob value) {
    return statement.bindBlob(index, value.bytes);
}
inline Result<void> bindOne(Statement& statement, int index, const Fingerprint128& value) {
    return statement.bindFingerprint(index, value);
}
inline Result<void> bindOne(Statement& statement, int index, Null) { return statement.bindNull(index); }
template<class T>
Result<void> bindOne(Statement& statement, int index, const std::optional<T>& value) {
    return value ? bindOne(statement, index, *value) : statement.bindNull(index);
}

// Binds args to parameters 1..n, stopping at the first error.
template<class... Args>
[[nodiscard]] Result<void> bindAll(Statement& statement, const Args&... args) {
    Result<void> result;
    int index = 0;
    static_cast<void>(((result = bindOne(statement, ++index, args), result.has_value()) && ...));
    return result;
}

// db.prepare(sql) with args bound.
template<class... Args>
[[nodiscard]] Result<Statement> prepareBound(Database& db, std::string_view sql, const Args&... args) {
    auto statement = db.prepare(sql);
    if (!statement) {
        return statement;
    }
    if (auto bound = bindAll(*statement, args...); !bound) {
        return std::unexpected(std::move(bound.error()));
    }
    return statement;
}

// Prepares, binds and runs a statement to completion, discarding every row it returns (none, or
// a RETURNING clause the caller does not need). Returns changes().
template<class... Args>
[[nodiscard]] Result<std::int64_t> runSql(Database& db, std::string_view sql, const Args&... args) {
    auto statement = prepareBound(db, sql, args...);
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    while (true) {
        auto row = statement->step();
        if (!row) {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row) {
            break;
        }
    }
    return db.changes();
}

// The first column of the first row as an integer, or nullopt when there is no row (or it is NULL).
template<class... Args>
[[nodiscard]] Result<std::optional<std::int64_t>> queryOptionalInt(Database& db, std::string_view sql,
                                                                   const Args&... args) {
    auto statement = prepareBound(db, sql, args...);
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    auto row = statement->step();
    if (!row) {
        return std::unexpected(std::move(row.error()));
    }
    if (!*row || statement->columnIsNull(0)) {
        return std::optional<std::int64_t>();
    }
    return std::optional<std::int64_t>(statement->columnInt64(0));
}

} // namespace frost::storage::detail
