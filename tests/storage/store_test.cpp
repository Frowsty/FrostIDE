// SPDX-License-Identifier: Apache-2.0
//
// Store files: corrupt-file quarantine (byte-identical copies, streaming, Busy while open), the
// Fail policy, truncated and damaged files, removeDatabaseFiles, and the WAL guarantees SQLite
// gives (a torn tail, stale frames after a restart, a stale -wal beside a missing database).

#include "frost/io/path.hpp"
#include "frost/storage/database.hpp"
#include "frost/storage/schema.hpp"
#include "frost/storage/store.hpp"

#include "support/hook_guard.hpp"
#include "support/scratch_dir.hpp"
#include "support/storage_test_support.hpp"
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <sqlite3.h>

namespace {

using frost::ErrorCode;
using frost::storage::CheckpointMode;
using frost::storage::CorruptPolicy;
using frost::storage::Database;
using frost::storage::Migration;
using frost::storage::OpenOptions;
using frost::storage::openStore;
using frost::storage::removeDatabaseFiles;
using frost::storage::SchemaSpec;
using frost::storage::TransactionKind;
using frost::storage::Verify;
using frost::storage::test::fastOptions;
using frost::storage::test::fileExists;
using frost::storage::test::fileSize;
using frost::storage::test::HookGuard;
using frost::storage::test::randomBytes;
using frost::storage::test::RawDb;
using frost::storage::test::readBytes;
using frost::storage::test::writeBytes;
using frost::test::ScratchDir;

constexpr std::uint32_t kStoreId = 0x46725354; // 'FrST'
constexpr Migration kMigrations[] = {
    {1, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER NOT NULL, b BLOB) STRICT;", nullptr}};
constexpr SchemaSpec kSpec{"test store", kStoreId, {}, kMigrations};
constexpr Migration kMigrationsV2[] = {kMigrations[0], {2, "CREATE TABLE u (x INTEGER) STRICT;", nullptr}};
constexpr SchemaSpec kSpecV2{"test store", kStoreId, {}, kMigrationsV2};

OpenOptions storeOptions() {
    OpenOptions options = fastOptions();
    options.schema = &kSpec;
    return options;
}

// The names in `dir` that start with `prefix`.
std::vector<std::string> namesStartingWith(const std::string& dir, std::string_view prefix) {
    std::vector<std::string> out;
    auto entries = frost::io::realFileSystem().listDirectory(dir);
    if (entries) {
        for (const auto& entry : *entries) {
            if (entry.name.starts_with(prefix)) {
                out.push_back(entry.name);
            }
        }
    }
    return out;
}

void insertRows(Database& db, int count, std::size_t blobBytes, int firstValue = 0) {
    for (int i = 0; i < count; ++i) {
        auto insert = db.prepare("INSERT INTO t (v, b) VALUES (?1, ?2)");
        ASSERT_TRUE(insert.has_value());
        FROST_STORAGE_ASSERT_OK(insert->bindInt64(1, firstValue + i));
        FROST_STORAGE_ASSERT_OK(
            insert->bindBlob(2, randomBytes(blobBytes, static_cast<std::uint64_t>(firstValue + i))));
        FROST_STORAGE_ASSERT_OK(insert->run());
    }
}

void expectQuarantinedCopy(const std::string& dir, const std::string& path, std::size_t size) {
    const std::string main = randomBytes(size, size);
    const std::string wal = randomBytes(size + 7, size + 1);
    writeBytes(path, main);
    writeBytes(path + "-wal", wal);
    auto outcome = openStore(path, storeOptions(), CorruptPolicy::Quarantine);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().describe();
    ASSERT_TRUE(outcome->quarantined.has_value());
    EXPECT_EQ(outcome->quarantined->reason.code(), ErrorCode::Corrupt);
    const std::string& movedTo = outcome->quarantined->movedTo;
    EXPECT_TRUE(movedTo.starts_with(path + ".corrupt-")) << movedTo;
    // The store in its place is fresh and usable.
    FROST_STORAGE_ASSERT_OK(outcome->database.execute("INSERT INTO t (v) VALUES (1)"));
    FROST_STORAGE_ASSERT_OK(outcome->database.close());
    EXPECT_EQ(readBytes(movedTo), main);
    EXPECT_EQ(readBytes(movedTo + "-wal"), wal);
    static_cast<void>(dir);
}

TEST(Store, GarbageFileQuarantinedByteIdentical) {
    ScratchDir dir("storage-store");
    for (const std::size_t size : {std::size_t{1}, std::size_t{100}, std::size_t{4096}, std::size_t{10000}}) {
        SCOPED_TRACE(size);
        expectQuarantinedCopy(dir.path(), dir.join(std::format("g{}.db", size)), size);
    }
}

TEST(Store, QuarantineNamesNeverCollide) {
    ScratchDir dir("storage-store");
    const std::string path = dir.join("q.db");
    frost::ManualClock clock; // The same stamp every time: the -n suffix must count up.
    OpenOptions options = storeOptions();
    options.clock = &clock;
    std::vector<std::string> moved;
    for (int i = 0; i < 3; ++i) {
        writeBytes(path, randomBytes(300, static_cast<std::uint64_t>(i)));
        auto outcome = openStore(path, options, CorruptPolicy::Quarantine);
        ASSERT_TRUE(outcome.has_value()) << outcome.error().describe();
        ASSERT_TRUE(outcome->quarantined.has_value());
        moved.push_back(outcome->quarantined->movedTo);
        FROST_STORAGE_ASSERT_OK(outcome->database.close());
        ASSERT_TRUE(removeDatabaseFiles(path).has_value());
    }
    EXPECT_EQ(moved[0], path + ".corrupt-19700101T000000Z");
    EXPECT_EQ(moved[1], path + ".corrupt-19700101T000000Z-1");
    EXPECT_EQ(moved[2], path + ".corrupt-19700101T000000Z-2");
}

TEST(Store, QuarantineStreamsLargeFile) {
    ScratchDir dir("storage-store");
    const std::string path = dir.join("large.db");
    const std::string main = randomBytes(std::size_t{32} << 20, 1);
    const std::string wal = randomBytes(std::size_t{4} << 20, 2);
    writeBytes(path, main);
    writeBytes(path + "-wal", wal);
    auto outcome = openStore(path, storeOptions(), CorruptPolicy::Quarantine);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().describe();
    ASSERT_TRUE(outcome->quarantined.has_value());
    const std::string movedTo = outcome->quarantined->movedTo;
    FROST_STORAGE_ASSERT_OK(outcome->database.close());
    EXPECT_TRUE(readBytes(movedTo) == main);
    EXPECT_TRUE(readBytes(movedTo + "-wal") == wal);
}

// Creates a closed store with `rows` rows of 1000-byte blobs; returns its page size.
std::int64_t makeStore(const std::string& path, int rows) {
    auto db = Database::open(path, storeOptions());
    EXPECT_TRUE(db.has_value()) << db.error().describe();
    insertRows(*db, rows, 1000);
    auto size = db->prepare("PRAGMA page_size");
    EXPECT_TRUE(size.has_value() && size->step().value_or(false));
    const std::int64_t pageSize = size->columnInt64(0);
    size = std::unexpected(frost::Error(ErrorCode::Unknown));
    EXPECT_TRUE(db->close().has_value());
    return pageSize;
}

// Reads every row; returns the error, or the rows' (v, blob) concatenation.
frost::Result<std::string> readAll(Database& db) {
    auto statement = db.prepare("SELECT v, b FROM t ORDER BY id");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    std::string out;
    while (true) {
        auto row = statement->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            break;
        }
        out += std::to_string(statement->columnInt64(0));
        out += statement->columnBlob(1);
    }
    return out;
}

TEST(Store, TruncatedFileIsCorruptAtOpenOrFirstRead) {
    ScratchDir dir("storage-store");
    const std::string original = dir.join("original.db");
    const std::int64_t pageSize = makeStore(original, 200);
    std::string expected;
    {
        auto db = Database::open(original, storeOptions());
        ASSERT_TRUE(db.has_value());
        auto all = readAll(*db);
        ASSERT_TRUE(all.has_value());
        expected = *all;
    }
    const std::string bytes = readBytes(original);
    const auto pages = static_cast<std::int64_t>(bytes.size()) / pageSize;
    ASSERT_GT(pages, 10);
    const std::size_t cuts[] = {
        static_cast<std::size_t>((pages - 3) * pageSize),                // at a page boundary
        static_cast<std::size_t>((pages - 3) * pageSize + pageSize / 2), // mid-page
        static_cast<std::size_t>(pageSize * 2 + 100),                    // early, mid-page
    };
    for (const std::size_t cut : cuts) {
        SCOPED_TRACE(cut);
        const std::string path = dir.join(std::format("cut{}.db", cut));
        writeBytes(path, std::string_view(bytes).substr(0, cut));
        OpenOptions quick = storeOptions();
        quick.verify = Verify::QuickCheck;
        {
            auto db = Database::open(path, quick);
            ASSERT_FALSE(db.has_value()) << "quick_check accepted a truncated file";
            EXPECT_EQ(db.error().code(), ErrorCode::Corrupt) << db.error().describe();
        }
        auto db = Database::open(path, storeOptions());
        if (!db) {
            EXPECT_EQ(db.error().code(), ErrorCode::Corrupt) << db.error().describe();
            continue;
        }
        auto all = readAll(*db);
        ASSERT_FALSE(all.has_value()) << "a truncated file read back as complete";
        EXPECT_EQ(all.error().code(), ErrorCode::Corrupt) << all.error().describe();
    }
}

TEST(Store, DamagedBtreeHeaderCaughtByQuickCheck) {
    ScratchDir dir("storage-store");
    const std::string path = dir.join("damaged.db");
    const std::int64_t pageSize = makeStore(path, 50);
    std::int64_t rootPage = 0;
    {
        RawDb raw(path);
        rootPage = raw.queryInt("SELECT rootpage FROM sqlite_schema WHERE name = 't'");
    }
    ASSERT_GT(rootPage, 1);
    const std::string before = readBytes(path);
    frost::storage::test::patchBytes(path, static_cast<std::size_t>((rootPage - 1) * pageSize), "\xFF");
    const std::string damaged = readBytes(path);
    OpenOptions options = storeOptions();
    options.verify = Verify::QuickCheck;
    auto outcome = openStore(path, options, CorruptPolicy::Quarantine);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().describe();
    ASSERT_TRUE(outcome->quarantined.has_value());
    EXPECT_EQ(outcome->quarantined->reason.code(), ErrorCode::Corrupt);
    const std::string movedTo = outcome->quarantined->movedTo;
    FROST_STORAGE_ASSERT_OK(outcome->database.close());
    EXPECT_EQ(readBytes(movedTo), damaged);
    EXPECT_NE(readBytes(path), before);
}

/*
 * Another process quarantines the corrupt file and creates a fresh store between our verdict and
 * our lock (the hook plays that process). The copy judged under our lock is not corrupt, so the
 * new store and its row stay, we open it, and only the other process's copy exists.
 */
TEST(Store, QuarantineSparesAStoreThatReplacedTheCorruptFile) {
    ScratchDir dir("storage-store");
    const std::string path = dir.join("s.db");
    writeBytes(path, randomBytes(4096, 7));
    struct Race {
        std::string path;
        bool ran = false;
        std::string error;
    } race{path, false, {}};
    {
        HookGuard guard(
            frost::storage::detail::g_openStoreBeforeQuarantine,
            [](void* context) {
                auto* state = static_cast<Race*>(context);
                if (state->ran) {
                    return; // The other process's own openStore passes this hook too.
                }
                state->ran = true;
                auto other = openStore(state->path, storeOptions(), CorruptPolicy::Quarantine);
                if (!other) {
                    state->error = other.error().describe();
                    return;
                }
                if (auto inserted = other->database.execute("INSERT INTO t (v) VALUES (42)"); !inserted) {
                    state->error = inserted.error().describe();
                }
            },
            &race);
        auto outcome = openStore(path, storeOptions(), CorruptPolicy::Quarantine);
        ASSERT_TRUE(outcome.has_value()) << outcome.error().describe();
        ASSERT_TRUE(race.ran);
        ASSERT_TRUE(race.error.empty()) << race.error;
        EXPECT_FALSE(outcome->quarantined.has_value());
        auto row = outcome->database.prepare("SELECT v FROM t");
        ASSERT_TRUE(row.has_value()) << row.error().describe();
        auto stepped = row->step();
        ASSERT_TRUE(stepped.has_value() && *stepped);
        EXPECT_EQ(row->columnInt64(0), 42);
    }
    EXPECT_EQ(namesStartingWith(dir.path(), "s.db.corrupt-").size(), 1u);
}

TEST(Store, FailPolicyLeavesFileInPlace) {
    ScratchDir dir("storage-store");
    const std::string path = dir.join("fail.db");
    const std::string garbage = randomBytes(5000, 5);
    writeBytes(path, garbage);
    auto outcome = openStore(path, storeOptions(), CorruptPolicy::Fail);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code(), ErrorCode::Corrupt);
    EXPECT_EQ(readBytes(path), garbage);
    EXPECT_TRUE(namesStartingWith(dir.path(), "fail.db.corrupt-").empty());
    // A read-only open never quarantines either.
    OpenOptions readOnly = storeOptions();
    readOnly.mode = frost::storage::OpenMode::ReadOnly;
    auto refused = openStore(path, readOnly, CorruptPolicy::Quarantine);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::Corrupt);
    EXPECT_EQ(readBytes(path), garbage);
}

TEST(Store, UnsupportedNeverQuarantined) {
    ScratchDir dir("storage-store");
    const std::string path = dir.join("newer.db");
    {
        OpenOptions options = fastOptions();
        options.schema = &kSpecV2;
        auto db = Database::open(path, options);
        ASSERT_TRUE(db.has_value());
    }
    const std::string before = readBytes(path);
    auto outcome = openStore(path, storeOptions(), CorruptPolicy::Quarantine);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(readBytes(path), before);
    EXPECT_TRUE(namesStartingWith(dir.path(), "newer.db.corrupt-").empty());
}

TEST(Store, QuarantineIsBusyWhileOpenElsewhere) {
    ScratchDir dir("storage-store");
    const std::string path = dir.join("held.db");
    RawDb holder(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    holder.exec(
        "PRAGMA journal_mode=WAL; PRAGMA application_id=1; CREATE TABLE x (v INTEGER); INSERT INTO x VALUES (1);");
    EXPECT_EQ(holder.queryInt("SELECT count(*) FROM x"), 1); // Holds SHARED from here on (WAL).
    OpenOptions options = storeOptions();
    options.busyTimeout = std::chrono::milliseconds(200);
    auto outcome = openStore(path, options, CorruptPolicy::Quarantine);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code(), ErrorCode::Busy) << outcome.error().describe();
    EXPECT_TRUE(namesStartingWith(dir.path(), "held.db.corrupt-").empty());
    EXPECT_EQ(holder.queryInt("SELECT count(*) FROM x"), 1);
    EXPECT_EQ(holder.queryInt("PRAGMA application_id"), 1);
}

TEST(Store, RemoveDatabaseFilesRemovesAllThree) {
    ScratchDir dir("storage-store");
    const std::string path = dir.join("r.db");
    {
        OpenOptions options = fastOptions();
        options.checkpointOnClose = false;
        auto db = Database::open(path, options);
        ASSERT_TRUE(db.has_value());
        FROST_STORAGE_ASSERT_OK(db->execute("CREATE TABLE t (v INTEGER); INSERT INTO t VALUES (1);"));
    }
    writeBytes(path + "-journal", "stale");
    ASSERT_TRUE(fileExists(path));
    ASSERT_TRUE(fileExists(path + "-wal"));
    ASSERT_TRUE(removeDatabaseFiles(path).has_value());
    for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
        EXPECT_FALSE(fileExists(path + suffix)) << suffix;
    }
    // Nothing there is fine; stale companions of a missing file go too.
    EXPECT_TRUE(removeDatabaseFiles(path).has_value());
    writeBytes(path + "-wal", "stale");
    EXPECT_TRUE(removeDatabaseFiles(path).has_value());
    EXPECT_FALSE(fileExists(path + "-wal"));
    auto relative = removeDatabaseFiles("relative.db");
    ASSERT_FALSE(relative.has_value());
    EXPECT_EQ(relative.error().code(), ErrorCode::InvalidArgument);
}

TEST(Store, RemoveDatabaseFilesBusyWhileOpen) {
    ScratchDir dir("storage-store");
    const std::string path = dir.join("open.db");
    auto db = Database::open(path, fastOptions());
    ASSERT_TRUE(db.has_value());
    FROST_STORAGE_ASSERT_OK(db->execute("CREATE TABLE t (v INTEGER); INSERT INTO t VALUES (1);"));
    auto removed = removeDatabaseFiles(path);
    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().code(), ErrorCode::Busy) << removed.error().describe();
    EXPECT_TRUE(fileExists(path));
    EXPECT_TRUE(fileExists(path + "-wal"));
    // The connection still works (the lock attempt did not disturb it).
    FROST_STORAGE_ASSERT_OK(db->execute("INSERT INTO t VALUES (2)"));
    FROST_STORAGE_ASSERT_OK(db->close());
    EXPECT_TRUE(removeDatabaseFiles(path).has_value());
    EXPECT_FALSE(fileExists(path));
}

// WAL guarantees. Closing with NO_CKPT_ON_CLOSE (checkpointOnClose=false) leaves the WAL as a
// crash would.
OpenOptions noCheckpoint() {
    OpenOptions options = storeOptions();
    options.checkpointOnClose = false;
    options.walAutoCheckpointPages = 0;
    return options;
}

TEST(Store, TornWalTailDropsOnlyTheTornTransaction) {
    ScratchDir dir("storage-store");
    const std::string path = dir.join("torn.db");
    std::uint64_t walBeforeLast = 0;
    std::int64_t pageSize = 0;
    {
        auto db = Database::open(path, noCheckpoint());
        ASSERT_TRUE(db.has_value());
        auto checkpointed = db->checkpoint(CheckpointMode::Truncate);
        ASSERT_TRUE(checkpointed.has_value());
        insertRows(*db, 10, 3000);
        walBeforeLast = fileSize(path + "-wal");
        insertRows(*db, 1, 3000, 100);
        ASSERT_GT(fileSize(path + "-wal"), walBeforeLast);
        auto size = db->prepare("PRAGMA page_size");
        ASSERT_TRUE(size.has_value() && size->step().value_or(false));
        pageSize = size->columnInt64(0);
    }
    // Cut inside the last transaction's first frame (24-byte frame header, then the page).
    const std::string wal = readBytes(path + "-wal");
    writeBytes(path + "-wal",
               std::string_view(wal).substr(0, walBeforeLast + 24 + static_cast<std::uint64_t>(pageSize / 2)));
    auto db = Database::open(path, storeOptions());
    ASSERT_TRUE(db.has_value()) << db.error().describe();
    auto statement = db->prepare("SELECT count(*), sum(v = 100) FROM t");
    ASSERT_TRUE(statement.has_value() && statement->step().value_or(false));
    EXPECT_EQ(statement->columnInt64(0), 10);
    EXPECT_EQ(statement->columnInt64(1), 0);
    statement = std::unexpected(frost::Error(ErrorCode::Unknown));
    EXPECT_TRUE(db->integrityCheck().has_value());
}

TEST(Store, StaleFramesAfterWalRestartIgnored) {
    ScratchDir dir("storage-store");
    const std::string path = dir.join("restart.db");
    {
        auto db = Database::open(path, noCheckpoint());
        ASSERT_TRUE(db.has_value());
        FROST_STORAGE_ASSERT_OK(db->execute("INSERT INTO t (id, v) VALUES (1, 0)"));
        for (int i = 1; i <= 5; ++i) {
            FROST_STORAGE_ASSERT_OK(db->execute(std::format("UPDATE t SET v = {} WHERE id = 1", i)));
            insertRows(*db, 3, 4000, i * 10);
        }
        const std::uint64_t walBefore = fileSize(path + "-wal");
        auto restarted = db->checkpoint(CheckpointMode::Restart);
        ASSERT_TRUE(restarted.has_value());
        ASSERT_TRUE(restarted->complete);
        // A small transaction now overwrites the start of the WAL; the old frames stay behind it.
        FROST_STORAGE_ASSERT_OK(db->execute("UPDATE t SET v = 6 WHERE id = 1"));
        ASSERT_EQ(fileSize(path + "-wal"), walBefore);
    }
    auto db = Database::open(path, storeOptions());
    ASSERT_TRUE(db.has_value()) << db.error().describe();
    auto statement = db->prepare("SELECT v FROM t WHERE id = 1");
    ASSERT_TRUE(statement.has_value() && statement->step().value_or(false));
    EXPECT_EQ(statement->columnInt64(0), 6);
    statement = std::unexpected(frost::Error(ErrorCode::Unknown));
    EXPECT_TRUE(db->integrityCheck().has_value());
}

TEST(Store, StaleWalBesideMissingDatabaseDiscarded) {
    ScratchDir dir("storage-store");
    const std::string donor = dir.join("donor.db");
    {
        auto db = Database::open(donor, noCheckpoint());
        ASSERT_TRUE(db.has_value());
        insertRows(*db, 5, 100);
    }
    const std::string path = dir.join("missing.db");
    writeBytes(path + "-wal", readBytes(donor + "-wal"));
    auto outcome = openStore(path, storeOptions(), CorruptPolicy::Fail);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().describe();
    EXPECT_FALSE(outcome->quarantined.has_value());
    auto statement = outcome->database.prepare("SELECT count(*) FROM t");
    ASSERT_TRUE(statement.has_value() && statement->step().value_or(false));
    EXPECT_EQ(statement->columnInt64(0), 0); // Fresh: the donor's rows were not replayed.
}

} // namespace
