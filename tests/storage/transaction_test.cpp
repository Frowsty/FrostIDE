// SPDX-License-Identifier: Apache-2.0
//
// Transaction: commit, rollback by destructor, nesting, inTransaction, read-only refusals and
// SQLite's automatic rollbacks.

#include "frost/storage/database.hpp"

#include "support/scratch_dir.hpp"
#include "support/storage_test_support.hpp"
#include <gtest/gtest.h>

#include <sqlite3.h>

namespace {

using frost::ErrorCode;
using frost::Result;
using frost::storage::Database;
using frost::storage::OpenMode;
using frost::storage::TransactionKind;
using frost::storage::test::fastOptions;

std::int64_t count(Database& db) {
    auto statement = db.prepare("SELECT count(*) FROM t");
    EXPECT_TRUE(statement.has_value());
    auto row = statement->step();
    EXPECT_TRUE(row.has_value() && *row);
    return statement->columnInt64(0);
}

class TransactionTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto db = Database::open(m_dir.join("t.db"), fastOptions());
        ASSERT_TRUE(db.has_value()) << db.error().describe();
        m_db = std::make_unique<Database>(std::move(*db));
        FROST_STORAGE_ASSERT_OK(m_db->execute("CREATE TABLE t (v INTEGER) STRICT"));
    }
    frost::test::ScratchDir m_dir{"storage-txn"};
    std::unique_ptr<Database> m_db;
};

TEST_F(TransactionTest, CommitPersists) {
    {
        auto transaction = m_db->begin(TransactionKind::Immediate);
        ASSERT_TRUE(transaction.has_value());
        EXPECT_TRUE(transaction->active());
        FROST_STORAGE_ASSERT_OK(m_db->execute("INSERT INTO t VALUES (1), (2)"));
        FROST_STORAGE_ASSERT_OK(transaction->commit());
        EXPECT_FALSE(transaction->active());
    }
    m_db.reset();
    auto reopened = Database::open(m_dir.join("t.db"), fastOptions());
    ASSERT_TRUE(reopened.has_value());
    EXPECT_EQ(count(*reopened), 2);
}

TEST_F(TransactionTest, DestructorRollsBack) {
    {
        auto transaction = m_db->begin(TransactionKind::Immediate);
        ASSERT_TRUE(transaction.has_value());
        FROST_STORAGE_ASSERT_OK(m_db->execute("INSERT INTO t VALUES (1)"));
        EXPECT_EQ(count(*m_db), 1);
    }
    EXPECT_EQ(count(*m_db), 0);
    EXPECT_NE(sqlite3_get_autocommit(m_db->nativeHandle()), 0);
    // Move assignment rolls back the overwritten transaction.
    auto first = m_db->begin(TransactionKind::Deferred);
    ASSERT_TRUE(first.has_value());
    FROST_STORAGE_ASSERT_OK(m_db->execute("INSERT INTO t VALUES (1)"));
    frost::storage::Transaction moved = std::move(*first);
    EXPECT_FALSE(first->active());
    moved.rollback();
    moved.rollback(); // Idempotent.
    EXPECT_EQ(count(*m_db), 0);
}

TEST_F(TransactionTest, NestedBeginIsInvalidArgument) {
    auto outer = m_db->begin(TransactionKind::Deferred);
    ASSERT_TRUE(outer.has_value());
    auto inner = m_db->begin(TransactionKind::Immediate);
    ASSERT_FALSE(inner.has_value());
    EXPECT_EQ(inner.error().code(), ErrorCode::InvalidArgument);
    // A transaction opened with raw SQL counts too.
    outer->rollback();
    FROST_STORAGE_ASSERT_OK(m_db->execute("BEGIN"));
    auto overRaw = m_db->begin(TransactionKind::Deferred);
    ASSERT_FALSE(overRaw.has_value());
    EXPECT_EQ(overRaw.error().code(), ErrorCode::InvalidArgument);
    FROST_STORAGE_ASSERT_OK(m_db->execute("ROLLBACK"));
}

TEST_F(TransactionTest, InTransactionRollsBackOnBodyError) {
    auto failed = m_db->inTransaction(TransactionKind::Immediate, [&]() -> Result<void> {
        if (auto inserted = m_db->execute("INSERT INTO t VALUES (1)"); !inserted) {
            return inserted;
        }
        return frost::fail(ErrorCode::NotFound, "body failed");
    });
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(count(*m_db), 0);
    auto succeeded = m_db->inTransaction(TransactionKind::Immediate,
                                         [&]() -> Result<void> { return m_db->execute("INSERT INTO t VALUES (2)"); });
    ASSERT_TRUE(succeeded.has_value());
    EXPECT_EQ(count(*m_db), 1);
}

TEST_F(TransactionTest, ImmediateOnReadOnlyIsPermissionDenied) {
    auto options = fastOptions();
    options.mode = OpenMode::ReadOnly;
    auto reader = Database::open(m_dir.join("t.db"), options);
    ASSERT_TRUE(reader.has_value()) << reader.error().describe();
    EXPECT_EQ(count(*reader), 0);
    auto write = reader->execute("INSERT INTO t VALUES (1)");
    ASSERT_FALSE(write.has_value());
    EXPECT_EQ(write.error().code(), ErrorCode::PermissionDenied);
    auto transaction = reader->begin(TransactionKind::Immediate);
    if (transaction) {
        // Some SQLite versions grant BEGIN IMMEDIATE on a read-only connection; the write still fails.
        auto inside = reader->execute("INSERT INTO t VALUES (1)");
        ASSERT_FALSE(inside.has_value());
        EXPECT_EQ(inside.error().code(), ErrorCode::PermissionDenied);
    } else {
        EXPECT_EQ(transaction.error().code(), ErrorCode::PermissionDenied);
    }
}

TEST_F(TransactionTest, RollbackAfterAutomaticRollbackIsHarmless) {
    FROST_STORAGE_ASSERT_OK(m_db->execute("CREATE TABLE u (k INTEGER PRIMARY KEY ON CONFLICT ROLLBACK) STRICT"));
    auto transaction = m_db->begin(TransactionKind::Immediate);
    ASSERT_TRUE(transaction.has_value());
    FROST_STORAGE_ASSERT_OK(m_db->execute("INSERT INTO t VALUES (1); INSERT INTO u VALUES (1)"));
    // ON CONFLICT ROLLBACK ends the transaction inside SQLite.
    auto conflict = m_db->execute("INSERT INTO u VALUES (1)");
    ASSERT_FALSE(conflict.has_value());
    EXPECT_EQ(conflict.error().code(), ErrorCode::AlreadyExists);
    EXPECT_NE(sqlite3_get_autocommit(m_db->nativeHandle()), 0);
    transaction->rollback(); // No transaction left: a no-op, not an error.
    EXPECT_FALSE(transaction->active());
    EXPECT_EQ(count(*m_db), 0);
    // A commit after SQLite rolled back fails, and the Transaction is finished.
    auto second = m_db->begin(TransactionKind::Immediate);
    ASSERT_TRUE(second.has_value());
    FROST_STORAGE_ASSERT_OK(m_db->execute("INSERT INTO u VALUES (2)"));
    ASSERT_FALSE(m_db->execute("INSERT INTO u VALUES (2)").has_value());
    auto committed = second->commit();
    EXPECT_FALSE(committed.has_value());
    EXPECT_FALSE(second->active());
    auto third = m_db->begin(TransactionKind::Immediate);
    EXPECT_TRUE(third.has_value());
}

} // namespace
