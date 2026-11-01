// SPDX-License-Identifier: Apache-2.0
//
// frost::storage::Statement (see frost/storage/statement.hpp).

#include "frost/storage/statement.hpp"

#include "frost/core/assert.hpp"

#include "connection.hpp"
#include "sqlite_error.hpp"

#include <array>
#include <format>
#include <utility>

#include <sqlite3.h>

namespace frost::storage {

namespace {

// A non-null pointer for empty TEXT and BLOB values: SQLite binds NULL for a null pointer.
constexpr char kEmpty[1] = {'\0'};

} // namespace

Statement::Statement(detail::Connection* connection, sqlite3_stmt* stmt) noexcept
    : m_connection(connection), m_stmt(stmt) {
    ++m_connection->liveStatements;
}

Statement::Statement(Statement&& other) noexcept
    : m_connection(std::exchange(other.m_connection, nullptr)), m_stmt(std::exchange(other.m_stmt, nullptr)),
      m_hasRow(std::exchange(other.m_hasRow, false)), m_stepped(std::exchange(other.m_stepped, false)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        release();
        m_connection = std::exchange(other.m_connection, nullptr);
        m_stmt = std::exchange(other.m_stmt, nullptr);
        m_hasRow = std::exchange(other.m_hasRow, false);
        m_stepped = std::exchange(other.m_stepped, false);
    }
    return *this;
}

Statement::~Statement() { release(); }

void Statement::release() noexcept {
    if (m_stmt != nullptr) {
        // sqlite3_finalize repeats the statement's last error, which step() already reported.
        static_cast<void>(sqlite3_finalize(m_stmt));
        m_stmt = nullptr;
        --m_connection->liveStatements;
        m_connection = nullptr;
    }
}

Result<void> Statement::checkBind(int rc, int index) {
    if (rc == SQLITE_OK) {
        return {};
    }
    return std::unexpected(
        detail::sqliteError(m_connection->db, rc, std::format("bind parameter {}", index), m_connection->path, sql()));
}

Result<void> Statement::bindInt64(int index, std::int64_t value) {
    FROST_PRE(m_stmt != nullptr && !m_stepped);
    return checkBind(sqlite3_bind_int64(m_stmt, index, value), index);
}

Result<void> Statement::bindDouble(int index, double value) {
    FROST_PRE(m_stmt != nullptr && !m_stepped);
    return checkBind(sqlite3_bind_double(m_stmt, index, value), index);
}

Result<void> Statement::bindText(int index, std::string_view utf8) {
    FROST_PRE(m_stmt != nullptr && !m_stepped);
    const char* data = utf8.empty() ? kEmpty : utf8.data();
    return checkBind(sqlite3_bind_text64(m_stmt, index, data, utf8.size(), SQLITE_TRANSIENT, SQLITE_UTF8), index);
}

Result<void> Statement::bindBlob(int index, std::string_view bytes) {
    FROST_PRE(m_stmt != nullptr && !m_stepped);
    const char* data = bytes.empty() ? kEmpty : bytes.data();
    return checkBind(sqlite3_bind_blob64(m_stmt, index, data, bytes.size(), SQLITE_TRANSIENT), index);
}

Result<void> Statement::bindBlobNoCopy(int index, std::string_view bytes) {
    FROST_PRE(m_stmt != nullptr && !m_stepped);
    const char* data = bytes.empty() ? kEmpty : bytes.data();
    return checkBind(sqlite3_bind_blob64(m_stmt, index, data, bytes.size(), SQLITE_STATIC), index);
}

Result<void> Statement::bindFingerprint(int index, const Fingerprint128& value) {
    FROST_PRE(m_stmt != nullptr && !m_stepped);
    const std::array<std::byte, 16> bytes = value.canonicalBytes();
    return checkBind(sqlite3_bind_blob64(m_stmt, index, bytes.data(), bytes.size(), SQLITE_TRANSIENT), index);
}

Result<void> Statement::bindNull(int index) {
    FROST_PRE(m_stmt != nullptr && !m_stepped);
    return checkBind(sqlite3_bind_null(m_stmt, index), index);
}

Result<bool> Statement::step() {
    FROST_PRE(m_stmt != nullptr);
    m_stepped = true;
    const int rc = sqlite3_step(m_stmt);
    if (rc == SQLITE_ROW) {
        m_hasRow = true;
        return true;
    }
    m_hasRow = false;
    if (rc == SQLITE_DONE) {
        return false;
    }
    sqlite3* db = m_connection->db;
    // An interrupt shows as SQLITE_INTERRUPT, or as BUSY when it cut a busy wait short; the
    // flag must be read before a reset, which may let SQLite clear it.
    Error error = detail::sqliteError(db, rc, "step", m_connection->path, sql());
    if (error.code() == ErrorCode::Busy && sqlite3_is_interrupted(db) != 0) {
        return fail(ErrorCode::Cancelled, error.message());
    }
    return std::unexpected(std::move(error));
}

Result<void> Statement::run() {
    auto stepped = step();
    reset();
    if (!stepped) {
        return std::unexpected(std::move(stepped.error()));
    }
    if (*stepped) {
        return fail(ErrorCode::InvalidArgument, std::format("statement returned a row [sql: {}]", sql()));
    }
    return {};
}

void Statement::reset() noexcept {
    FROST_PRE(m_stmt != nullptr);
    // The return value repeats the last step error, which step() already reported.
    static_cast<void>(sqlite3_reset(m_stmt));
    m_hasRow = false;
    m_stepped = false;
}

void Statement::clearBindings() noexcept {
    FROST_PRE(m_stmt != nullptr);
    static_cast<void>(sqlite3_clear_bindings(m_stmt));
}

int Statement::columnCount() const noexcept {
    FROST_PRE(m_stmt != nullptr);
    return sqlite3_column_count(m_stmt);
}

bool Statement::columnIsNull(int col) const noexcept {
    FROST_PRE(m_hasRow && col >= 0 && col < sqlite3_column_count(m_stmt));
    return sqlite3_column_type(m_stmt, col) == SQLITE_NULL;
}

std::int64_t Statement::columnInt64(int col) const noexcept {
    FROST_PRE(m_hasRow && col >= 0 && col < sqlite3_column_count(m_stmt));
    return sqlite3_column_int64(m_stmt, col);
}

double Statement::columnDouble(int col) const noexcept {
    FROST_PRE(m_hasRow && col >= 0 && col < sqlite3_column_count(m_stmt));
    return sqlite3_column_double(m_stmt, col);
}

std::string_view Statement::columnText(int col) const noexcept {
    FROST_PRE(m_hasRow && col >= 0 && col < sqlite3_column_count(m_stmt));
    // Pointer first, then the byte count (the count of the converted value), as SQLite requires.
    const unsigned char* text = sqlite3_column_text(m_stmt, col);
    const int bytes = sqlite3_column_bytes(m_stmt, col);
    if (text == nullptr || bytes <= 0) {
        return {};
    }
    return {reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes)};
}

std::string_view Statement::columnBlob(int col) const noexcept {
    FROST_PRE(m_hasRow && col >= 0 && col < sqlite3_column_count(m_stmt));
    const void* blob = sqlite3_column_blob(m_stmt, col);
    const int bytes = sqlite3_column_bytes(m_stmt, col);
    if (blob == nullptr || bytes <= 0) {
        return {};
    }
    return {static_cast<const char*>(blob), static_cast<std::size_t>(bytes)};
}

Result<Fingerprint128> Statement::columnFingerprint(int col) const {
    const std::string_view bytes = columnBlob(col);
    if (bytes.size() != 16) {
        return fail(ErrorCode::Corrupt,
                    std::format("fingerprint column {} holds {} bytes, not 16 [sql: {}]", col, bytes.size(), sql()));
    }
    return Fingerprint128::fromCanonicalBytes(bytes);
}

std::string_view Statement::sql() const noexcept {
    if (m_stmt == nullptr) {
        return {};
    }
    const char* text = sqlite3_sql(m_stmt);
    return text != nullptr ? std::string_view(text) : std::string_view();
}

} // namespace frost::storage
