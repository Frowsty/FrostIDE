// SPDX-License-Identifier: Apache-2.0
//
// Fault injection through the FaultVfs shim (support/fault_vfs.hpp): every write of a journal put
// and of a store's creation fails in turn, checkpoint writes fail under commits, and syncs fail.
// After each fault the files are reopened through the real VFS: the integrity check passes and
// the data is the old state or the new one, never a mixture (the storage half of PLAN's "disk
// full" injection).

#include "frost/storage/database.hpp"
#include "frost/storage/hot_exit_journal.hpp"
#include "frost/storage/schema.hpp"

#include "database_internal.hpp"
#include "support/fault_vfs.hpp"
#include "support/scratch_dir.hpp"
#include "support/storage_test_support.hpp"
#include <gtest/gtest.h>

#include <format>
#include <string>

#include <sqlite3.h>

namespace {

using frost::ErrorCode;
using frost::storage::CheckpointMode;
using frost::storage::Database;
using frost::storage::DocumentState;
using frost::storage::findRecoverableJournals;
using frost::storage::HotExitJournal;
using frost::storage::JournalOptions;
using frost::storage::Migration;
using frost::storage::OpenOptions;
using frost::storage::PutOutcome;
using frost::storage::RecoveredJournal;
using frost::storage::SchemaSpec;
using frost::storage::Synchronous;
using frost::storage::test::fastOptions;
using frost::storage::test::FaultVfs;
using frost::storage::test::randomBytes;
using frost::test::ScratchDir;

// Enough faults for any of these operations; reaching it means the loop never succeeded.
constexpr std::int64_t kMaxFaults = 2000;

std::string contentFor(std::int64_t generation) { return randomBytes(20000, static_cast<std::uint64_t>(generation)); }

DocumentState stateFor(std::int64_t generation) {
    DocumentState state;
    state.documentKey = "doc";
    state.path = "/work/doc";
    state.generation = generation;
    state.encoding = "utf-8";
    state.lineEnding = "lf";
    return state;
}

frost::Result<PutOutcome> putGeneration(HotExitJournal& journal, std::int64_t generation) {
    const std::string content = contentFor(generation);
    const std::string_view pieces[] = {content};
    return journal.put(stateFor(generation), pieces);
}

// Recovers the only journal in `dir` through the real VFS; checks it and returns the generation of
// its entry (0 if none), then discards it.
std::int64_t recoverAndCheck(const std::string& dir) {
    auto scan = findRecoverableJournals(dir, std::chrono::seconds(5));
    EXPECT_TRUE(scan.has_value()) << scan.error().describe();
    if (!scan || scan->journals.size() != 1) {
        ADD_FAILURE() << "expected one recoverable journal";
        return -1;
    }
    EXPECT_TRUE(scan->unreadable.empty());
    RecoveredJournal recovered = std::move(scan->journals.front());
    EXPECT_FALSE(recovered.damaged());
    EXPECT_TRUE(recovered.database().integrityCheck().has_value());
    auto entries = recovered.entries();
    EXPECT_TRUE(entries.has_value());
    std::int64_t generation = 0;
    if (entries && !entries->empty()) {
        EXPECT_EQ(entries->size(), 1u);
        generation = (*entries)[0].state.generation;
        auto content = recovered.readContent((*entries)[0].id);
        EXPECT_TRUE(content.has_value()) << content.error().describe();
        if (content) {
            EXPECT_TRUE(*content == contentFor(generation)) << "generation " << generation;
        }
    }
    EXPECT_TRUE(std::move(recovered).discard().has_value());
    return generation;
}

JournalOptions shimJournal() {
    JournalOptions options;
    options.vfs = FaultVfs::kName;
    options.chunkBytes = 4096;
    return options;
}

TEST(FaultVfs, ShimDelegatesAndRecordsNames) {
    FaultVfs& vfs = FaultVfs::instance();
    vfs.reset();
    ScratchDir dir("storage-fault");
    OpenOptions options = fastOptions();
    options.vfs = FaultVfs::kName;
    {
        auto db = Database::open(dir.join("shim.db"), options);
        ASSERT_TRUE(db.has_value()) << db.error().describe();
        FROST_STORAGE_ASSERT_OK(db->execute("CREATE TABLE t (v INTEGER); INSERT INTO t VALUES (1);"));
        FROST_STORAGE_ASSERT_OK(db->close());
    }
    EXPECT_GT(vfs.count(FaultVfs::Op::Write), 0);
    bool sawMain = false;
    bool sawWal = false;
    for (const std::string& name : vfs.openedNames()) {
        sawMain = sawMain || name.ends_with("shim.db");
        sawWal = sawWal || name.ends_with("shim.db-wal");
    }
    EXPECT_TRUE(sawMain);
    EXPECT_TRUE(sawWal);
    // A planned fault fires once.
    vfs.reset();
    vfs.failAt(FaultVfs::Op::Write, 0, SQLITE_FULL);
    {
        auto db = Database::open(dir.join("shim.db"), options);
        ASSERT_TRUE(db.has_value()) << db.error().describe();
        auto failed = db->execute("INSERT INTO t VALUES (2)");
        ASSERT_FALSE(failed.has_value());
        EXPECT_EQ(failed.error().code(), ErrorCode::IoError) << failed.error().describe();
        EXPECT_TRUE(vfs.fired());
        FROST_STORAGE_ASSERT_OK(db->execute("INSERT INTO t VALUES (3)"));
    }
    vfs.reset();
}

TEST(FaultVfs, JournalPutSurvivesEveryWriteFailure) {
    FaultVfs& vfs = FaultVfs::instance();
    ScratchDir dir("storage-fault");
    std::int64_t index = 0;
    for (; index < kMaxFaults; ++index) {
        SCOPED_TRACE(std::format("write {}", index));
        vfs.reset();
        bool succeeded = false;
        {
            auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), shimJournal());
            ASSERT_TRUE(journal.has_value()) << journal.error().describe();
            ASSERT_TRUE(putGeneration(*journal, 1).has_value());
            vfs.reset();
            vfs.failAt(FaultVfs::Op::Write, index, index % 2 == 0 ? SQLITE_FULL : SQLITE_IOERR_WRITE);
            auto put = putGeneration(*journal, 2);
            if (put) {
                succeeded = true;
            } else {
                EXPECT_EQ(put.error().code(), ErrorCode::IoError) << put.error().describe();
            }
        } // A crash: no clean shutdown.
        const bool fired = vfs.fired();
        vfs.reset();
        const std::int64_t generation = recoverAndCheck(dir.path());
        EXPECT_TRUE(generation == 1 || generation == 2) << generation;
        if (succeeded) {
            EXPECT_EQ(generation, 2);
        }
        if (!fired) {
            EXPECT_TRUE(succeeded);
            break;
        }
    }
    EXPECT_LT(index, kMaxFaults);
    EXPECT_GT(index, 3); // The put wrote several pages.
}

TEST(FaultVfs, JournalPutSurvivesPersistentDiskFull) {
    FaultVfs& vfs = FaultVfs::instance();
    ScratchDir dir("storage-fault");
    for (const std::int64_t index : {0, 1, 3, 7}) {
        SCOPED_TRACE(std::format("full from write {}", index));
        vfs.reset();
        {
            auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), shimJournal());
            ASSERT_TRUE(journal.has_value());
            ASSERT_TRUE(putGeneration(*journal, 1).has_value());
            vfs.reset();
            vfs.failAt(FaultVfs::Op::Write, index, SQLITE_FULL, true);
            auto put = putGeneration(*journal, 2);
            ASSERT_FALSE(put.has_value());
            EXPECT_EQ(put.error().code(), ErrorCode::IoError);
            // Still full: a retry fails the same way, and the closing checkpoint cannot write either.
            EXPECT_FALSE(putGeneration(*journal, 2).has_value());
        }
        vfs.reset();
        EXPECT_EQ(recoverAndCheck(dir.path()), 1);
    }
}

constexpr std::uint32_t kFaultId = 0x46724654; // 'FrFT'
constexpr Migration kFaultMigrations[] = {
    {1, "CREATE TABLE a (x INTEGER PRIMARY KEY, y BLOB) STRICT; CREATE INDEX a_y ON a (y);", nullptr},
    {2, "CREATE TABLE b (z TEXT NOT NULL) STRICT; INSERT INTO b VALUES ('migrated');", nullptr},
};
constexpr SchemaSpec kFaultSpec{"fault store", kFaultId, "PRAGMA auto_vacuum=INCREMENTAL;", kFaultMigrations};

TEST(FaultVfs, CreationSurvivesEveryWriteFailure) {
    FaultVfs& vfs = FaultVfs::instance();
    ScratchDir dir("storage-fault");
    std::int64_t index = 0;
    for (; index < kMaxFaults; ++index) {
        SCOPED_TRACE(std::format("write {}", index));
        const std::string path = dir.join(std::format("create{}.db", index));
        vfs.reset();
        vfs.failAt(FaultVfs::Op::Write, index, SQLITE_FULL);
        OpenOptions options = fastOptions();
        options.vfs = FaultVfs::kName;
        options.schema = &kFaultSpec;
        bool succeeded = false;
        {
            auto db = Database::open(path, options);
            succeeded = db.has_value();
            if (!db) {
                EXPECT_EQ(db.error().code(), ErrorCode::IoError) << db.error().describe();
            }
        }
        const bool fired = vfs.fired();
        vfs.reset();
        // Through the real VFS: fresh (then created now) or complete, never Corrupt.
        OpenOptions real = fastOptions();
        real.schema = &kFaultSpec;
        {
            auto db = Database::open(path, real);
            ASSERT_TRUE(db.has_value()) << db.error().describe();
            FROST_STORAGE_ASSERT_OK(db->integrityCheck());
            auto migrated = db->prepare("SELECT count(*) FROM b");
            ASSERT_TRUE(migrated.has_value() && migrated->step().value_or(false));
            EXPECT_EQ(migrated->columnInt64(0), 1);
        }
        {
            frost::storage::test::RawDb raw(path);
            EXPECT_EQ(raw.queryInt("PRAGMA user_version"), 2);
            EXPECT_EQ(raw.queryInt("PRAGMA auto_vacuum"), 2);
        }
        if (!fired) {
            EXPECT_TRUE(succeeded);
            break;
        }
    }
    EXPECT_LT(index, kMaxFaults);
    EXPECT_GT(index, 2);
}

TEST(FaultVfs, CheckpointWriteFailureDoesNotFailCommit) {
    FaultVfs& vfs = FaultVfs::instance();
    vfs.reset();
    ScratchDir dir("storage-fault");
    const std::string path = dir.join("ckpt.db");
    OpenOptions options = fastOptions();
    options.vfs = FaultVfs::kName;
    options.walAutoCheckpointPages = 4;
    {
        auto db = Database::open(path, options);
        ASSERT_TRUE(db.has_value()) << db.error().describe();
        FROST_STORAGE_ASSERT_OK(db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, b BLOB) STRICT"));
        // Only the main file fails: commits write the WAL, the hook's checkpoints the main file.
        vfs.reset();
        vfs.setTypeFilter(SQLITE_OPEN_MAIN_DB);
        vfs.failAt(FaultVfs::Op::Write, 0, SQLITE_IOERR_WRITE, true);
        for (int i = 0; i < 20; ++i) {
            auto insert = db->prepare("INSERT INTO t (b) VALUES (?1)");
            ASSERT_TRUE(insert.has_value());
            FROST_STORAGE_ASSERT_OK(insert->bindBlob(1, randomBytes(3000, static_cast<std::uint64_t>(i))));
            FROST_STORAGE_ASSERT_OK(insert->run()); // Autocommit: each run is a commit.
        }
        EXPECT_TRUE(vfs.fired());
        EXPECT_NE(frost::storage::DatabaseAccess::connection(*db).lastCheckpointError, SQLITE_OK);
        // A checkpoint asked for explicitly reports the failure.
        auto checkpointed = db->checkpoint(CheckpointMode::Truncate);
        ASSERT_FALSE(checkpointed.has_value());
        EXPECT_EQ(checkpointed.error().code(), ErrorCode::IoError);
        // Space is back: the next checkpoint succeeds.
        vfs.reset();
        auto retried = db->checkpoint(CheckpointMode::Truncate);
        ASSERT_TRUE(retried.has_value()) << retried.error().describe();
        EXPECT_TRUE(retried->complete);
    }
    frost::storage::test::RawDb raw(path);
    EXPECT_EQ(raw.queryInt("SELECT count(*) FROM t"), 20);
    EXPECT_TRUE(raw.integrityOk());
}

TEST(FaultVfs, CommitsDurableWhenEveryCheckpointFails) {
    FaultVfs& vfs = FaultVfs::instance();
    vfs.reset();
    ScratchDir dir("storage-fault");
    const std::string path = dir.join("durable.db");
    OpenOptions options;
    options.synchronous = Synchronous::Full;
    options.vfs = FaultVfs::kName;
    options.walAutoCheckpointPages = 2;
    {
        auto db = Database::open(path, options);
        ASSERT_TRUE(db.has_value()) << db.error().describe();
        FROST_STORAGE_ASSERT_OK(db->execute("CREATE TABLE t (v INTEGER) STRICT"));
        vfs.reset();
        vfs.setTypeFilter(SQLITE_OPEN_MAIN_DB);
        vfs.failAt(FaultVfs::Op::Write, 0, SQLITE_FULL, true);
        for (int i = 0; i < 50; ++i) {
            FROST_STORAGE_ASSERT_OK(db->execute(std::format("INSERT INTO t VALUES ({})", i)));
        }
        // The closing checkpoint fails too; the WAL keeps every commit.
    }
    vfs.reset();
    frost::storage::test::RawDb raw(path);
    EXPECT_EQ(raw.queryInt("SELECT count(*) FROM t"), 50);
    EXPECT_TRUE(raw.integrityOk());
}

TEST(FaultVfs, SyncFailureIsIoErrorAndRecoverable) {
    FaultVfs& vfs = FaultVfs::instance();
    ScratchDir dir("storage-fault");
    vfs.reset();
    {
        JournalOptions options = shimJournal();
        options.synchronous = Synchronous::Full;
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), options);
        ASSERT_TRUE(journal.has_value()) << journal.error().describe();
        ASSERT_TRUE(putGeneration(*journal, 1).has_value());
        vfs.reset();
        vfs.failAt(FaultVfs::Op::Sync, 0, SQLITE_IOERR_FSYNC);
        auto put = putGeneration(*journal, 2);
        ASSERT_FALSE(put.has_value());
        EXPECT_EQ(put.error().code(), ErrorCode::IoError) << put.error().describe();
        EXPECT_TRUE(vfs.fired());
        // The outcome is unknown; a retry with the same generation is safe, and later puts work.
        auto retried = putGeneration(*journal, 2);
        ASSERT_TRUE(retried.has_value()) << retried.error().describe();
        auto later = putGeneration(*journal, 3);
        ASSERT_TRUE(later.has_value()) << later.error().describe();
        EXPECT_EQ(*later, PutOutcome::Written);
    }
    vfs.reset();
    EXPECT_EQ(recoverAndCheck(dir.path()), 3);
    // Without the retries: generation 1 or 2 after the crash.
    {
        JournalOptions options = shimJournal();
        options.synchronous = Synchronous::Full;
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), options);
        ASSERT_TRUE(journal.has_value());
        ASSERT_TRUE(putGeneration(*journal, 1).has_value());
        vfs.reset();
        vfs.failAt(FaultVfs::Op::Sync, 0, SQLITE_IOERR_FSYNC);
        EXPECT_FALSE(putGeneration(*journal, 2).has_value());
    }
    vfs.reset();
    const std::int64_t generation = recoverAndCheck(dir.path());
    EXPECT_TRUE(generation == 1 || generation == 2) << generation;
}

TEST(FaultVfs, DiskFullDuringCheckpoint) {
    FaultVfs& vfs = FaultVfs::instance();
    vfs.reset();
    ScratchDir dir("storage-fault");
    const std::string path = dir.join("full.db");
    OpenOptions options = fastOptions();
    options.vfs = FaultVfs::kName;
    options.walAutoCheckpointPages = 0; // Only explicit checkpoints.
    {
        auto db = Database::open(path, options);
        ASSERT_TRUE(db.has_value());
        FROST_STORAGE_ASSERT_OK(db->execute("CREATE TABLE t (b BLOB) STRICT"));
        for (int i = 0; i < 30; ++i) {
            auto insert = db->prepare("INSERT INTO t VALUES (?1)");
            ASSERT_TRUE(insert.has_value());
            FROST_STORAGE_ASSERT_OK(insert->bindBlob(1, randomBytes(4000, static_cast<std::uint64_t>(i))));
            FROST_STORAGE_ASSERT_OK(insert->run());
        }
        for (const std::int64_t index : {0, 5, 20}) {
            SCOPED_TRACE(index);
            vfs.reset();
            vfs.setTypeFilter(SQLITE_OPEN_MAIN_DB);
            vfs.failAt(FaultVfs::Op::Write, index, SQLITE_FULL, true);
            auto checkpointed = db->checkpoint(CheckpointMode::Truncate);
            ASSERT_FALSE(checkpointed.has_value());
            EXPECT_EQ(checkpointed.error().code(), ErrorCode::IoError) << checkpointed.error().describe();
            // Reads and writes still work while the main file is full: they use the WAL.
            vfs.reset();
            auto count = db->prepare("SELECT count(*) FROM t");
            ASSERT_TRUE(count.has_value() && count->step().value_or(false));
            EXPECT_EQ(count->columnInt64(0), 30);
        }
        vfs.reset();
        auto checkpointed = db->checkpoint(CheckpointMode::Truncate);
        ASSERT_TRUE(checkpointed.has_value()) << checkpointed.error().describe();
        EXPECT_TRUE(checkpointed->complete);
        FROST_STORAGE_ASSERT_OK(db->integrityCheck());
    }
    frost::storage::test::RawDb raw(path);
    EXPECT_EQ(raw.queryInt("SELECT count(*) FROM t"), 30);
    EXPECT_TRUE(raw.integrityOk());
}

} // namespace
