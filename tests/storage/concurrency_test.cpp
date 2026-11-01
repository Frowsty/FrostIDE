// SPDX-License-Identifier: Apache-2.0
//
// Concurrency: readers see consistent snapshots while one writer commits and a checkpointer runs
// (in threads, and across processes with storage_test_child), a second writer waits for the
// lock up to busyTimeout and then gets Busy, a Truncate checkpoint reports incomplete while a
// reader holds a snapshot, and local-history readers run while another connection records.

#include "frost/storage/database.hpp"
#include "frost/storage/local_history.hpp"

#include "support/bank.hpp"
#include "support/child_process.hpp"
#include "support/crash_protocol.hpp"
#include "support/scratch_dir.hpp"
#include "support/storage_test_support.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <latch>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::storage::CheckpointMode;
using frost::storage::Database;
using frost::storage::OpenOptions;
using frost::storage::TransactionKind;
using frost::storage::test::bankOptions;
using frost::storage::test::Failures;
using frost::storage::test::fastOptions;
using frost::test::ScratchDir;

TEST(StorageConcurrency, ReadersSeeConsistentSnapshots) {
    ScratchDir dir("storage-concurrency");
    Failures failures;
    const std::uint64_t reads = frost::storage::test::runBank(dir.join("bank.db"), 2000, 0ms, 4, failures);
    failures.report();
    EXPECT_EQ(failures.count(), 0u);
    EXPECT_GT(reads, 0u);
}

OpenOptions lockOptions(std::chrono::milliseconds timeout) {
    OpenOptions options = fastOptions();
    options.busyTimeout = timeout;
    return options;
}

TEST(StorageConcurrency, SecondWriterWaitsThenSucceeds) {
    ScratchDir dir("storage-concurrency");
    const std::string path = dir.join("lock.db");
    {
        auto db = Database::open(path, lockOptions(2s));
        ASSERT_TRUE(db.has_value());
        FROST_STORAGE_ASSERT_OK(db->execute("CREATE TABLE t (v INTEGER) STRICT"));
    }
    std::latch locked(1);
    std::atomic<bool> holding{false};
    std::jthread holder([&] {
        auto db = Database::open(path, lockOptions(2s));
        auto transaction = db ? db->begin(TransactionKind::Immediate)
                              : frost::Result<frost::storage::Transaction>(std::unexpected(db.error()));
        const bool inserted = transaction && db->execute("INSERT INTO t VALUES (1)").has_value();
        holding.store(inserted);
        locked.count_down(); // Also on failure, so the test never hangs.
        if (inserted) {
            std::this_thread::sleep_for(300ms);
            EXPECT_TRUE(transaction->commit().has_value());
        }
    });
    locked.wait();
    ASSERT_TRUE(holding.load()) << "the holder could not take the write lock";
    auto db = Database::open(path, lockOptions(2s));
    ASSERT_TRUE(db.has_value());
    const auto started = std::chrono::steady_clock::now();
    auto transaction = db->begin(TransactionKind::Immediate);
    const auto waited = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(transaction.has_value()) << transaction.error().describe();
    EXPECT_GE(waited, 250ms);
    FROST_STORAGE_ASSERT_OK(db->execute("INSERT INTO t VALUES (2)"));
    FROST_STORAGE_ASSERT_OK(transaction->commit());
    holder.join();
    auto count = db->prepare("SELECT count(*) FROM t");
    ASSERT_TRUE(count.has_value() && count->step().value_or(false));
    EXPECT_EQ(count->columnInt64(0), 2);
}

TEST(StorageConcurrency, BusyTimeoutExpires) {
    ScratchDir dir("storage-concurrency");
    const std::string path = dir.join("lock.db");
    auto holder = Database::open(path, lockOptions(2s));
    ASSERT_TRUE(holder.has_value());
    FROST_STORAGE_ASSERT_OK(holder->execute("CREATE TABLE t (v INTEGER) STRICT"));
    auto held = holder->begin(TransactionKind::Immediate);
    ASSERT_TRUE(held.has_value());
    auto other = Database::open(path, lockOptions(50ms));
    ASSERT_TRUE(other.has_value());
    const auto started = std::chrono::steady_clock::now();
    auto transaction = other->begin(TransactionKind::Immediate);
    const auto waited = std::chrono::steady_clock::now() - started;
    ASSERT_FALSE(transaction.has_value());
    EXPECT_EQ(transaction.error().code(), ErrorCode::Busy);
    EXPECT_LT(waited, 1s);
    EXPECT_GE(waited, 40ms);
    // Writes in autocommit mode give up the same way.
    auto write = other->execute("INSERT INTO t VALUES (1)");
    ASSERT_FALSE(write.has_value());
    EXPECT_EQ(write.error().code(), ErrorCode::Busy);
}

TEST(StorageConcurrency, TruncateBusyWhileReaderHoldsSnapshot) {
    ScratchDir dir("storage-concurrency");
    const std::string path = dir.join("snapshot.db");
    OpenOptions options = lockOptions(100ms);
    options.walAutoCheckpointPages = 0;
    auto writer = Database::open(path, options);
    ASSERT_TRUE(writer.has_value());
    FROST_STORAGE_ASSERT_OK(writer->execute("CREATE TABLE t (b BLOB) STRICT"));
    auto reader = Database::open(path, options);
    ASSERT_TRUE(reader.has_value());
    {
        auto snapshot = reader->begin(TransactionKind::Deferred);
        ASSERT_TRUE(snapshot.has_value());
        auto count = reader->prepare("SELECT count(*) FROM t");
        ASSERT_TRUE(count.has_value() && count->step().value_or(false));
        EXPECT_EQ(count->columnInt64(0), 0);
        for (int i = 0; i < 20; ++i) {
            FROST_STORAGE_ASSERT_OK(writer->execute("INSERT INTO t VALUES (randomblob(3000))"));
        }
        auto blocked = writer->checkpoint(CheckpointMode::Truncate);
        ASSERT_TRUE(blocked.has_value()) << blocked.error().describe();
        EXPECT_FALSE(blocked->complete);
        EXPECT_GT(frost::storage::test::fileSize(path + "-wal"), 0u);
        count = std::unexpected(frost::Error(ErrorCode::Unknown));
    }
    auto truncated = writer->checkpoint(CheckpointMode::Truncate);
    ASSERT_TRUE(truncated.has_value()) << truncated.error().describe();
    EXPECT_TRUE(truncated->complete);
    EXPECT_EQ(frost::storage::test::fileSize(path + "-wal"), 0u);
}

TEST(StorageConcurrency, LocalHistoryReadersWhileRecording) {
    ScratchDir dir("storage-concurrency");
    const std::string path = dir.join("history.db");
    frost::storage::HistoryOptions options;
    options.synchronous = frost::storage::Synchronous::Normal;
    options.busyTimeout = 10s;
    {
        auto created = frost::storage::LocalHistory::open(path, frost::systemClock(), options);
        ASSERT_TRUE(created.has_value()) << created.error().describe();
    }
    Failures failures;
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> reads{0};
    std::vector<std::jthread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&, r] {
            auto history = frost::storage::LocalHistory::open(path, frost::systemClock(), options);
            if (!history) {
                failures.add(std::format("reader {} open: {}", r, history.error().describe()));
                return;
            }
            while (!done.load()) {
                auto revisions = history->revisions("file.cpp", 1);
                if (!revisions) {
                    failures.add(std::format("reader {}: {}", r, revisions.error().describe()));
                    return;
                }
                if (revisions->empty()) {
                    continue;
                }
                auto content = history->content((*revisions)[0].id);
                if (!content) {
                    failures.add(std::format("reader {} content: {}", r, content.error().describe()));
                    return;
                }
                if (content->size() != (*revisions)[0].size) {
                    failures.add(
                        std::format("reader {} read {} bytes of {}", r, content->size(), (*revisions)[0].size));
                    return;
                }
                reads.fetch_add(1);
            }
        });
    }
    {
        auto history = frost::storage::LocalHistory::open(path, frost::systemClock(), options);
        ASSERT_TRUE(history.has_value());
        for (int i = 0; i < 200; ++i) {
            const std::string content = frost::storage::test::randomBytes(20000 + static_cast<std::size_t>(i) * 100,
                                                                          static_cast<std::uint64_t>(i));
            const std::string_view pieces[] = {content};
            auto recorded = history->record("file.cpp", "/file.cpp", frost::storage::RevisionKind::Save, {}, pieces);
            if (!recorded) {
                failures.add("record: " + recorded.error().describe());
                break;
            }
        }
    }
    done.store(true);
    readers.clear();
    failures.report();
    EXPECT_EQ(failures.count(), 0u);
    EXPECT_GT(reads.load(), 0u);
}

TEST(StorageConcurrency, CrossProcessWriterAndReaders) {
    ScratchDir dir("storage-concurrency");
    const std::string path = dir.join("bank.db");
    auto child = frost::storage::test::StorageChild::start({"bank-writer", path, "77"});
    ASSERT_NE(child, nullptr);
    ASSERT_TRUE(child->waitFor("ready").has_value());
    Failures failures;
    {
        std::atomic<bool> done{false};
        std::vector<std::jthread> readers;
        for (int r = 0; r < 2; ++r) {
            readers.emplace_back([&, r] {
                auto db = Database::open(path, bankOptions(frost::storage::OpenMode::ReadWrite));
                if (!db) {
                    failures.add(std::format("reader {} open: {}", r, db.error().describe()));
                    return;
                }
                while (!done.load()) {
                    auto total = frost::storage::test::bankTotal(*db);
                    if (!total || *total != frost::storage::test::kBankTotal) {
                        failures.add(
                            std::format("reader {}: {}", r, total ? std::to_string(*total) : total.error().describe()));
                        return;
                    }
                }
            });
        }
        ASSERT_TRUE(child->waitFor("transfer ", 10).has_value());
        done.store(true);
    }
    child->kill();
    failures.report();
    EXPECT_EQ(failures.count(), 0u);
    // The parent takes over as the writer after the crash.
    auto db = Database::open(path, bankOptions(frost::storage::OpenMode::ReadWrite));
    ASSERT_TRUE(db.has_value()) << db.error().describe();
    for (std::uint64_t n = 1; n <= 100; ++n) {
        FROST_STORAGE_ASSERT_OK(frost::storage::test::transfer(*db, n));
    }
    EXPECT_EQ(frost::storage::test::bankTotal(*db).value(), frost::storage::test::kBankTotal);
    FROST_STORAGE_ASSERT_OK(db->integrityCheck());
}

} // namespace
