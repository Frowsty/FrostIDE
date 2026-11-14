// SPDX-License-Identifier: Apache-2.0
//
// Statement: binding and reading every type, the empty-value and embedded-NUL pitfalls, prepare
// rules, constraint and limit errors, reuse after reset, and interruption from another thread.

#include "frost/storage/database.hpp"
#include "frost/storage/statement.hpp"

#include "support/storage_test_support.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <thread>

#include <sqlite3.h>

namespace {

using frost::ErrorCode;
using frost::storage::Database;
using frost::storage::Fingerprint128;
using frost::storage::test::fastOptions;

Database memoryDb() {
    auto db = Database::openInMemory(fastOptions());
    EXPECT_TRUE(db.has_value()) << db.error().describe();
    return std::move(*db);
}

TEST(Statement, EveryTypeRoundTrips) {
    Database db = memoryDb();
    FROST_STORAGE_ASSERT_OK(db.execute("CREATE TABLE t (i INTEGER, d REAL, s TEXT, b BLOB, n ANY) STRICT"));
    const double specials[] = {0.0,
                               -0.0,
                               1.5,
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::max()};
    const std::int64_t ints[] = {0, -1, std::numeric_limits<std::int64_t>::min(),
                                 std::numeric_limits<std::int64_t>::max()};
    for (const std::int64_t value : ints) {
        for (const double real : specials) {
            auto insert = db.prepare("INSERT INTO t VALUES (?1, ?2, ?3, ?4, ?5)");
            ASSERT_TRUE(insert.has_value());
            FROST_STORAGE_ASSERT_OK(insert->bindInt64(1, value));
            FROST_STORAGE_ASSERT_OK(insert->bindDouble(2, real));
            FROST_STORAGE_ASSERT_OK(insert->bindText(3, "h\xC3\xA9llo"));
            FROST_STORAGE_ASSERT_OK(insert->bindBlob(4, std::string_view("\x00\x01\xFF", 3)));
            FROST_STORAGE_ASSERT_OK(insert->bindNull(5));
            FROST_STORAGE_ASSERT_OK(insert->run());
            auto select = db.prepare("SELECT i, d, s, b, n FROM t WHERE rowid = last_insert_rowid()");
            ASSERT_TRUE(select.has_value());
            auto row = select->step();
            ASSERT_TRUE(row.has_value() && *row);
            EXPECT_EQ(select->columnCount(), 5);
            EXPECT_EQ(select->columnInt64(0), value);
            EXPECT_EQ(select->columnDouble(1), real);
            // SQLite's record format stores an integral REAL as an integer, so -0.0 comes back as
            // +0.0 (equal, but without its sign); every other value keeps its sign.
            if (real != 0.0) {
                EXPECT_EQ(std::signbit(select->columnDouble(1)), std::signbit(real));
            }
            EXPECT_EQ(select->columnText(2), "h\xC3\xA9llo");
            EXPECT_EQ(select->columnBlob(3), std::string_view("\x00\x01\xFF", 3));
            EXPECT_TRUE(select->columnIsNull(4));
            EXPECT_FALSE(select->columnIsNull(0));
        }
    }
    // NaN: SQLite stores NaN as NULL (sqlite3_bind_double documents it); it never reads back as a number.
    auto nan = db.prepare("SELECT ?1 IS NULL");
    ASSERT_TRUE(nan.has_value());
    FROST_STORAGE_ASSERT_OK(nan->bindDouble(1, std::numeric_limits<double>::quiet_NaN()));
    auto row = nan->step();
    ASSERT_TRUE(row.has_value() && *row);
    EXPECT_EQ(nan->columnInt64(0), 1);
    // Fingerprints round-trip through their canonical BLOB.
    const Fingerprint128 fingerprint{0x1122334455667788ull, 0x99AABBCCDDEEFF00ull};
    auto fp = db.prepare("SELECT ?1, length(?1)");
    ASSERT_TRUE(fp.has_value());
    FROST_STORAGE_ASSERT_OK(fp->bindFingerprint(1, fingerprint));
    row = fp->step();
    ASSERT_TRUE(row.has_value() && *row);
    auto read = fp->columnFingerprint(0);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, fingerprint);
    EXPECT_EQ(fp->columnInt64(1), 16);
}

TEST(Statement, EmptyBlobAndTextAreNotNull) {
    Database db = memoryDb();
    auto statement = db.prepare("SELECT ?1 IS NULL, ?2 IS NULL, ?3 IS NULL, typeof(?1), typeof(?2), typeof(?3)");
    ASSERT_TRUE(statement.has_value());
    FROST_STORAGE_ASSERT_OK(statement->bindText(1, std::string_view()));
    FROST_STORAGE_ASSERT_OK(statement->bindBlob(2, std::string_view()));
    FROST_STORAGE_ASSERT_OK(statement->bindBlobNoCopy(3, std::string_view()));
    auto row = statement->step();
    ASSERT_TRUE(row.has_value() && *row);
    EXPECT_EQ(statement->columnInt64(0), 0);
    EXPECT_EQ(statement->columnInt64(1), 0);
    EXPECT_EQ(statement->columnInt64(2), 0);
    EXPECT_EQ(statement->columnText(3), "text");
    EXPECT_EQ(statement->columnText(4), "blob");
    EXPECT_EQ(statement->columnText(5), "blob");
}

TEST(Statement, EmbeddedNulsAndInvalidUtf8BytesSurvive) {
    Database db = memoryDb();
    FROST_STORAGE_ASSERT_OK(db.execute("CREATE TABLE t (b BLOB NOT NULL) STRICT"));
    std::string bytes;
    for (int i = 0; i < 256; ++i) {
        bytes.push_back(static_cast<char>(i));
    }
    bytes += std::string("\0\0\xC3\x28\xFF\xFE", 6); // NULs and invalid UTF-8.
    auto insert = db.prepare("INSERT INTO t VALUES (?1)");
    ASSERT_TRUE(insert.has_value());
    FROST_STORAGE_ASSERT_OK(insert->bindBlob(1, bytes));
    FROST_STORAGE_ASSERT_OK(insert->run());
    auto select = db.prepare("SELECT b, length(b) FROM t");
    ASSERT_TRUE(select.has_value());
    auto row = select->step();
    ASSERT_TRUE(row.has_value() && *row);
    EXPECT_EQ(select->columnBlob(0), bytes);
    EXPECT_EQ(select->columnInt64(1), static_cast<std::int64_t>(bytes.size()));
}

TEST(Statement, PrepareAcceptsTrailingCommentsRejectsTrailingStatement) {
    Database db = memoryDb();
    EXPECT_TRUE(db.prepare("SELECT 1;  -- comment\n /* block */ ").has_value());
    auto two = db.prepare("SELECT 1; SELECT 2");
    ASSERT_FALSE(two.has_value());
    EXPECT_EQ(two.error().code(), ErrorCode::InvalidArgument);
    auto none = db.prepare("  -- only a comment");
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().code(), ErrorCode::InvalidArgument);
}

TEST(Statement, SyntaxErrorIsInvalidArgument) {
    Database db = memoryDb();
    auto bad = db.prepare("SELEKT 1");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(bad.error().message().find("SELEKT"), std::string::npos) << bad.error().message();
    // Double-quoted string literals are refused (DQS off), so a typo in a column name is caught.
    FROST_STORAGE_ASSERT_OK(db.execute("CREATE TABLE t (a INTEGER)"));
    auto dqs = db.prepare("SELECT \"nosuchcolumn\" FROM t");
    ASSERT_FALSE(dqs.has_value());
    EXPECT_EQ(dqs.error().code(), ErrorCode::InvalidArgument);
}

TEST(Statement, UniqueViolationIsAlreadyExists) {
    Database db = memoryDb();
    FROST_STORAGE_ASSERT_OK(db.execute("CREATE TABLE t (k TEXT PRIMARY KEY, u INTEGER UNIQUE) STRICT"));
    FROST_STORAGE_ASSERT_OK(db.execute("INSERT INTO t VALUES ('a', 1)"));
    auto duplicateKey = db.execute("INSERT INTO t VALUES ('a', 2)");
    ASSERT_FALSE(duplicateKey.has_value());
    EXPECT_EQ(duplicateKey.error().code(), ErrorCode::AlreadyExists);
    auto duplicateUnique = db.execute("INSERT INTO t VALUES ('b', 1)");
    ASSERT_FALSE(duplicateUnique.has_value());
    EXPECT_EQ(duplicateUnique.error().code(), ErrorCode::AlreadyExists);
}

TEST(Statement, StrictTypeViolationIsInvalidArgument) {
    Database db = memoryDb();
    FROST_STORAGE_ASSERT_OK(db.execute("CREATE TABLE t (i INTEGER NOT NULL) STRICT"));
    auto insert = db.prepare("INSERT INTO t VALUES (?1)");
    ASSERT_TRUE(insert.has_value());
    FROST_STORAGE_ASSERT_OK(insert->bindText(1, "not a number"));
    auto ran = insert->run();
    ASSERT_FALSE(ran.has_value());
    EXPECT_EQ(ran.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(ran.error().message().find("SQLITE_CONSTRAINT_DATATYPE"), std::string::npos) << ran.error().message();
}

TEST(Statement, TooBigIsOutOfRange) {
    Database db = memoryDb();
    sqlite3_limit(db.nativeHandle(), SQLITE_LIMIT_LENGTH, 1000);
    auto statement = db.prepare("SELECT length(?1)");
    ASSERT_TRUE(statement.has_value());
    auto bound = statement->bindBlob(1, std::string(2000, 'x'));
    ASSERT_FALSE(bound.has_value());
    EXPECT_EQ(bound.error().code(), ErrorCode::OutOfRange);
    auto boundText = statement->bindText(1, std::string(2000, 'x'));
    ASSERT_FALSE(boundText.has_value());
    EXPECT_EQ(boundText.error().code(), ErrorCode::OutOfRange);
    // A bad index is a programming error: Internal.
    auto badIndex = statement->bindInt64(7, 1);
    ASSERT_FALSE(badIndex.has_value());
    EXPECT_EQ(badIndex.error().code(), ErrorCode::Internal);
}

TEST(Statement, ResetAndRebindReuses) {
    Database db = memoryDb();
    FROST_STORAGE_ASSERT_OK(db.execute("CREATE TABLE t (v INTEGER) STRICT"));
    auto insert = db.prepare("INSERT INTO t VALUES (?1)");
    ASSERT_TRUE(insert.has_value());
    for (int i = 0; i < 100; ++i) {
        FROST_STORAGE_ASSERT_OK(insert->bindInt64(1, i));
        FROST_STORAGE_ASSERT_OK(insert->run());
    }
    auto sum = db.prepare("SELECT sum(v), count(*) FROM t");
    ASSERT_TRUE(sum.has_value());
    for (int pass = 0; pass < 3; ++pass) {
        auto row = sum->step();
        ASSERT_TRUE(row.has_value() && *row);
        EXPECT_EQ(sum->columnInt64(0), 4950);
        EXPECT_EQ(sum->columnInt64(1), 100);
        sum->reset();
    }
}

TEST(Statement, NoCopyBindingSurvivesReset) {
    // SQLite keeps bindings across reset: a SQLITE_STATIC buffer is read again by the next step,
    // so it must outlive the binding, not just the step (the rule bindBlobNoCopy documents).
    Database db = memoryDb();
    std::string buffer = "first";
    auto statement = db.prepare("SELECT ?1");
    ASSERT_TRUE(statement.has_value());
    FROST_STORAGE_ASSERT_OK(statement->bindBlobNoCopy(1, buffer));
    auto row = statement->step();
    ASSERT_TRUE(row.has_value() && *row);
    EXPECT_EQ(statement->columnBlob(0), "first");
    statement->reset();
    buffer.replace(0, 5, "other"); // Same storage, new bytes.
    row = statement->step();
    ASSERT_TRUE(row.has_value() && *row);
    EXPECT_EQ(statement->columnBlob(0), "other");
    statement->reset();
    statement->clearBindings();
    row = statement->step();
    ASSERT_TRUE(row.has_value() && *row);
    EXPECT_TRUE(statement->columnIsNull(0));
}

TEST(Statement, RunRejectsRows) {
    Database db = memoryDb();
    auto statement = db.prepare("SELECT 1");
    ASSERT_TRUE(statement.has_value());
    auto ran = statement->run();
    ASSERT_FALSE(ran.has_value());
    EXPECT_EQ(ran.error().code(), ErrorCode::InvalidArgument);
    // run() reset the statement, so it steps again from the start.
    auto row = statement->step();
    ASSERT_TRUE(row.has_value() && *row);
}

TEST(Statement, InterruptHandleFromAnotherThreadCancels) {
    Database db = memoryDb();
    auto statement =
        db.prepare("WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) SELECT count(*) FROM c");
    ASSERT_TRUE(statement.has_value());
    const frost::storage::InterruptHandle handle = db.interruptHandle();
    std::jthread interrupter([handle] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        handle.interrupt();
    });
    const auto started = std::chrono::steady_clock::now();
    auto row = statement->step();
    ASSERT_FALSE(row.has_value());
    EXPECT_EQ(row.error().code(), ErrorCode::Cancelled);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(30));
    statement->reset();
    // The connection works again afterwards.
    auto again = db.prepare("SELECT 7");
    ASSERT_TRUE(again.has_value());
    auto next = again->step();
    ASSERT_TRUE(next.has_value() && *next);
    EXPECT_EQ(again->columnInt64(0), 7);
    // A default handle is a no-op.
    frost::storage::InterruptHandle().interrupt();
}

} // namespace
