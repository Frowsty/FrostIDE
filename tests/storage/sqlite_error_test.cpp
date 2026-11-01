// SPDX-License-Identifier: Apache-2.0
//
// The SQLite result-code mapping of sqlite_error.cpp. The expected table is written from the
// list of result codes in SQLite's documentation (sqlite.org/rescode.html) and ADR 0014's table:
// a consistency check of the two, not an independent oracle.

#include "sqlite_error.hpp"
#include <gtest/gtest.h>

#include <string>

#include <sqlite3.h>

namespace {

using frost::ErrorCode;
using frost::storage::detail::errorCodeFromSqlite;
using frost::storage::detail::makeSqliteError;
using frost::storage::detail::sqliteCodeName;

struct Row {
    int code;
    ErrorCode expected;
};

TEST(SqliteError, MapsEveryPrimaryCode) {
    const Row rows[] = {
        {SQLITE_ERROR, ErrorCode::InvalidArgument},
        {SQLITE_INTERNAL, ErrorCode::Internal},
        {SQLITE_PERM, ErrorCode::PermissionDenied},
        {SQLITE_ABORT, ErrorCode::Cancelled},
        {SQLITE_BUSY, ErrorCode::Busy},
        {SQLITE_LOCKED, ErrorCode::Busy},
        {SQLITE_NOMEM, ErrorCode::IoError},
        {SQLITE_READONLY, ErrorCode::PermissionDenied},
        {SQLITE_INTERRUPT, ErrorCode::Cancelled},
        {SQLITE_IOERR, ErrorCode::IoError},
        {SQLITE_CORRUPT, ErrorCode::Corrupt},
        {SQLITE_NOTFOUND, ErrorCode::Internal},
        {SQLITE_FULL, ErrorCode::IoError},
        {SQLITE_CANTOPEN, ErrorCode::IoError},
        {SQLITE_PROTOCOL, ErrorCode::Busy},
        {SQLITE_EMPTY, ErrorCode::Internal},
        {SQLITE_SCHEMA, ErrorCode::Busy},
        {SQLITE_TOOBIG, ErrorCode::OutOfRange},
        {SQLITE_CONSTRAINT, ErrorCode::InvalidArgument},
        {SQLITE_MISMATCH, ErrorCode::InvalidArgument},
        {SQLITE_MISUSE, ErrorCode::Internal},
        {SQLITE_NOLFS, ErrorCode::Unsupported},
        {SQLITE_AUTH, ErrorCode::PermissionDenied},
        {SQLITE_FORMAT, ErrorCode::Corrupt},
        {SQLITE_RANGE, ErrorCode::Internal},
        {SQLITE_NOTADB, ErrorCode::Corrupt},
        {SQLITE_NOTICE, ErrorCode::Unknown},
        {SQLITE_WARNING, ErrorCode::Unknown},
    };
    for (const Row& row : rows) {
        EXPECT_EQ(errorCodeFromSqlite(row.code, 0), row.expected) << sqliteCodeName(row.code);
        EXPECT_NE(sqliteCodeName(row.code), "SQLITE_UNKNOWN") << row.code;
    }
}

TEST(SqliteError, MapsRefinedExtendedCodes) {
    const Row rows[] = {
        {SQLITE_READONLY_DBMOVED, ErrorCode::IoError},
        {SQLITE_READONLY_CANTLOCK, ErrorCode::PermissionDenied},
        {SQLITE_IOERR_CORRUPTFS, ErrorCode::Corrupt},
        {SQLITE_IOERR_DATA, ErrorCode::Corrupt},
        {SQLITE_IOERR_WRITE, ErrorCode::IoError},
        {SQLITE_IOERR_FSYNC, ErrorCode::IoError},
        {SQLITE_CANTOPEN_ISDIR, ErrorCode::InvalidArgument},
        {SQLITE_CANTOPEN_FULLPATH, ErrorCode::InvalidArgument},
        {SQLITE_ABORT_ROLLBACK, ErrorCode::Internal},
        {SQLITE_BUSY_SNAPSHOT, ErrorCode::Busy},
        {SQLITE_BUSY_TIMEOUT, ErrorCode::Busy},
        {SQLITE_BUSY_RECOVERY, ErrorCode::Busy},
        {SQLITE_LOCKED_SHAREDCACHE, ErrorCode::Busy},
        {SQLITE_CORRUPT_INDEX, ErrorCode::Corrupt},
        {SQLITE_CORRUPT_SEQUENCE, ErrorCode::Corrupt},
        {SQLITE_CONSTRAINT_UNIQUE, ErrorCode::AlreadyExists},
        {SQLITE_CONSTRAINT_PRIMARYKEY, ErrorCode::AlreadyExists},
        {SQLITE_CONSTRAINT_CHECK, ErrorCode::InvalidArgument},
        {SQLITE_CONSTRAINT_FOREIGNKEY, ErrorCode::InvalidArgument},
        {SQLITE_CONSTRAINT_NOTNULL, ErrorCode::InvalidArgument},
        {SQLITE_CONSTRAINT_DATATYPE, ErrorCode::InvalidArgument},
        {SQLITE_ERROR_RETRY, ErrorCode::InvalidArgument},
    };
    for (const Row& row : rows) {
        EXPECT_EQ(errorCodeFromSqlite(row.code, 0), row.expected) << sqliteCodeName(row.code);
        EXPECT_NE(sqliteCodeName(row.code), "SQLITE_UNKNOWN") << row.code;
    }
    EXPECT_EQ(sqliteCodeName(0x7FFF), "SQLITE_UNKNOWN");
    EXPECT_EQ(errorCodeFromSqlite(0xFE, 0), ErrorCode::Unknown);
}

TEST(SqliteError, RefinesCantOpenByOsError) {
#ifdef _WIN32
    constexpr int kAccessDenied = 5; // ERROR_ACCESS_DENIED
    constexpr int kFileNotFound = 2; // ERROR_FILE_NOT_FOUND
#else
    constexpr int kAccessDenied = 13; // EACCES
    constexpr int kFileNotFound = 2;  // ENOENT
#endif
    EXPECT_EQ(errorCodeFromSqlite(SQLITE_CANTOPEN, kAccessDenied), ErrorCode::PermissionDenied);
    EXPECT_EQ(errorCodeFromSqlite(SQLITE_CANTOPEN, kFileNotFound), ErrorCode::NotFound);
    EXPECT_EQ(errorCodeFromSqlite(SQLITE_CANTOPEN, 0), ErrorCode::IoError);
    // ISDIR and FULLPATH keep their own meaning whatever the OS said.
    EXPECT_EQ(errorCodeFromSqlite(SQLITE_CANTOPEN_ISDIR, kAccessDenied), ErrorCode::InvalidArgument);
}

TEST(SqliteError, MessageNamesOperationPathCode) {
    const frost::Error error =
        makeSqliteError(SQLITE_CANTOPEN, "unable to open database file", 5, "open database", "/x/y.db");
    EXPECT_EQ(error.code(), errorCodeFromSqlite(SQLITE_CANTOPEN, 5));
    const std::string& message = error.message();
    EXPECT_NE(message.find("open database '/x/y.db'"), std::string::npos) << message;
    EXPECT_NE(message.find("unable to open database file"), std::string::npos) << message;
    EXPECT_NE(message.find("sqlite 14 SQLITE_CANTOPEN"), std::string::npos) << message;
#ifdef _WIN32
    EXPECT_NE(message.find("win32 error 5"), std::string::npos) << message;
#else
    EXPECT_NE(message.find("errno 5"), std::string::npos) << message;
#endif
    // SQL is quoted, cut to 120 bytes; the system error is shown only for CANTOPEN and IOERR.
    const std::string longSql(300, 'x');
    const frost::Error stepError =
        makeSqliteError(SQLITE_CONSTRAINT_UNIQUE, "UNIQUE constraint failed", 5, "step", "/x/y.db", longSql);
    EXPECT_EQ(stepError.code(), ErrorCode::AlreadyExists);
    EXPECT_NE(stepError.message().find("[sql: " + std::string(120, 'x') + "...]"), std::string::npos);
    EXPECT_EQ(stepError.message().find("error 5"), std::string::npos);
    EXPECT_NE(stepError.message().find("SQLITE_CONSTRAINT_UNIQUE"), std::string::npos);
}

} // namespace
