// SPDX-License-Identifier: Apache-2.0
//
// Schema versioning: fresh creation, step-by-step migration, refusals that leave the file
// byte-identical, fresh detection, concurrent first opens and upgrades, and the regression
// goldens of the two stores' DDL (FROST_STORAGE_REGENERATE_GOLDENS=1 rewrites them).

#include "frost/storage/database.hpp"
#include "frost/storage/hot_exit_journal.hpp"
#include "frost/storage/local_history.hpp"
#include "frost/storage/schema.hpp"

#include "support/scratch_dir.hpp"
#include "support/storage_test_support.hpp"
#include <gtest/gtest.h>

#include <cstddef>
#include <format>
#include <fstream>
#include <latch>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sqlite3.h>

namespace {

using frost::ErrorCode;
using frost::Result;
using frost::storage::Database;
using frost::storage::Migration;
using frost::storage::OpenMode;
using frost::storage::OpenOptions;
using frost::storage::SchemaSpec;
using frost::storage::Verify;
using frost::storage::test::fastOptions;
using frost::storage::test::fileExists;
using frost::storage::test::RawDb;
using frost::storage::test::readBytes;
using frost::test::ScratchDir;

constexpr std::uint32_t kTestId = 0x46725453; // 'FrTS'

// Version 3's data step: counts its runs, so the tests can see it ran exactly once.
Result<void> countRun(Database& db) { return db.execute("INSERT INTO runs (step) VALUES (3)"); }
Result<void> failStep(Database&) { return frost::fail(ErrorCode::IoError, "migration step failed on purpose"); }

constexpr Migration kV1[] = {
    {1, "CREATE TABLE a (x INTEGER) STRICT; CREATE TABLE runs (step INTEGER NOT NULL) STRICT;", nullptr},
};
constexpr Migration kV2[] = {
    kV1[0],
    {2, "ALTER TABLE a ADD COLUMN y TEXT;", nullptr},
};
constexpr Migration kV3[] = {
    kV1[0],
    kV2[1],
    {3, "CREATE TABLE b (z INTEGER) STRICT;", countRun},
};
constexpr Migration kV2Failing[] = {
    kV1[0],
    {2, "ALTER TABLE a ADD COLUMN y TEXT;", failStep},
};

constexpr SchemaSpec kSpecV1{"test store", kTestId, {}, kV1};
constexpr SchemaSpec kSpecV2{"test store", kTestId, {}, kV2};
constexpr SchemaSpec kSpecV3{"test store", kTestId, {}, kV3};
constexpr SchemaSpec kSpecV2Failing{"test store", kTestId, {}, kV2Failing};
constexpr SchemaSpec kSpecV3Vacuum{"test store", kTestId, "PRAGMA auto_vacuum=INCREMENTAL;", kV3};

OpenOptions withSpec(const SchemaSpec& spec) {
    OpenOptions options = fastOptions();
    options.schema = &spec;
    return options;
}

// Creates a closed file at `spec`'s newest version.
void createAt(const std::string& path, const SchemaSpec& spec) {
    auto db = Database::open(path, withSpec(spec));
    ASSERT_TRUE(db.has_value()) << db.error().describe();
    FROST_STORAGE_ASSERT_OK(db->execute("INSERT INTO a (x) VALUES (1), (2), (3)"));
    FROST_STORAGE_ASSERT_OK(db->close());
}

std::int64_t rawInt(const std::string& path, std::string_view sql) {
    RawDb raw(path);
    return raw.queryInt(sql);
}

TEST(Schema, FreshFileGetsNewestVersion) {
    ScratchDir dir("storage-schema");
    const std::string path = dir.join("s.db");
    {
        auto db = Database::open(path, withSpec(kSpecV3));
        ASSERT_TRUE(db.has_value()) << db.error().describe();
        FROST_STORAGE_ASSERT_OK(db->execute("INSERT INTO b (z) VALUES (1)"));
    }
    EXPECT_EQ(rawInt(path, "PRAGMA user_version"), 3);
    EXPECT_EQ(rawInt(path, "PRAGMA application_id"), kTestId);
    EXPECT_EQ(rawInt(path, "SELECT count(*) FROM runs"), 1);
    EXPECT_EQ(rawInt(path, "SELECT count(*) FROM pragma_table_info('a') WHERE name = 'y'"), 1);
    // Reopening applies nothing.
    auto again = Database::open(path, withSpec(kSpecV3));
    ASSERT_TRUE(again.has_value());
    again = std::unexpected(frost::Error(ErrorCode::Unknown));
    EXPECT_EQ(rawInt(path, "SELECT count(*) FROM runs"), 1);
}

TEST(Schema, MigratesStepByStep) {
    ScratchDir dir("storage-schema");
    const std::string path = dir.join("s.db");
    createAt(path, kSpecV1);
    EXPECT_EQ(rawInt(path, "PRAGMA user_version"), 1);
    {
        auto db = Database::open(path, withSpec(kSpecV2));
        ASSERT_TRUE(db.has_value()) << db.error().describe();
    }
    EXPECT_EQ(rawInt(path, "PRAGMA user_version"), 2);
    EXPECT_EQ(rawInt(path, "SELECT count(*) FROM runs"), 0);
    {
        auto db = Database::open(path, withSpec(kSpecV3));
        ASSERT_TRUE(db.has_value()) << db.error().describe();
    }
    EXPECT_EQ(rawInt(path, "PRAGMA user_version"), 3);
    EXPECT_EQ(rawInt(path, "SELECT count(*) FROM runs"), 1);
    EXPECT_EQ(rawInt(path, "SELECT count(*) FROM a"), 3); // Data survives migrations.
}

TEST(Schema, FailedStepKeepsPreviousVersion) {
    ScratchDir dir("storage-schema");
    const std::string path = dir.join("s.db");
    createAt(path, kSpecV1);
    auto db = Database::open(path, withSpec(kSpecV2Failing));
    ASSERT_FALSE(db.has_value());
    EXPECT_EQ(db.error().code(), ErrorCode::IoError);
    EXPECT_EQ(rawInt(path, "PRAGMA user_version"), 1);
    EXPECT_EQ(rawInt(path, "SELECT count(*) FROM pragma_table_info('a') WHERE name = 'y'"), 0);
}

// Opens `path` with `spec`, expects `code`, and checks the file and its -wal are unchanged.
void expectRefusedUntouched(const std::string& path, const SchemaSpec& spec, ErrorCode code,
                            OpenMode mode = OpenMode::ReadWriteCreate) {
    const bool hadWal = fileExists(path + "-wal");
    const std::string before = readBytes(path);
    const std::string walBefore = hadWal ? readBytes(path + "-wal") : std::string();
    OpenOptions options = withSpec(spec);
    options.mode = mode;
    {
        auto db = Database::open(path, options);
        ASSERT_FALSE(db.has_value());
        EXPECT_EQ(db.error().code(), code) << db.error().describe();
    }
    EXPECT_EQ(readBytes(path), before);
    if (mode == OpenMode::ReadOnly) {
        // A read-only reader of a WAL file creates an empty -wal and -shm it cannot remove (SQLite).
        return;
    }
    EXPECT_EQ(fileExists(path + "-wal"), hadWal);
    if (hadWal) {
        EXPECT_EQ(readBytes(path + "-wal"), walBefore);
    }
    EXPECT_FALSE(fileExists(path + "-journal"));
}

TEST(Schema, NewerVersionIsUnsupportedAndFileUntouched) {
    ScratchDir dir("storage-schema");
    const std::string path = dir.join("s.db");
    createAt(path, kSpecV3);
    expectRefusedUntouched(path, kSpecV1, ErrorCode::Unsupported);
}

TEST(Schema, ForeignApplicationIdIsCorruptAndUntouched) {
    ScratchDir dir("storage-schema");
    const std::string foreign = dir.join("foreign.db");
    {
        RawDb raw(foreign, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        raw.exec("PRAGMA application_id=305419896; CREATE TABLE other (v INTEGER); INSERT INTO other VALUES (1);");
    }
    expectRefusedUntouched(foreign, kSpecV1, ErrorCode::Corrupt);
    // application_id 0 on a file with tables is foreign too.
    const std::string anonymous = dir.join("anonymous.db");
    {
        RawDb raw(anonymous, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        raw.exec("CREATE TABLE other (v INTEGER);");
    }
    expectRefusedUntouched(anonymous, kSpecV1, ErrorCode::Corrupt);
}

TEST(Schema, OurIdWithoutVersionIsCorruptAndUntouched) {
    ScratchDir dir("storage-schema");
    const std::string path = dir.join("s.db");
    createAt(path, kSpecV1);
    {
        RawDb raw(path);
        raw.exec("PRAGMA user_version=0;");
    }
    expectRefusedUntouched(path, kSpecV3, ErrorCode::Corrupt);
    {
        RawDb raw(path);
        raw.exec("PRAGMA user_version=-4;");
    }
    expectRefusedUntouched(path, kSpecV3, ErrorCode::Corrupt);
}

TEST(Schema, NotADatabaseIsCorruptAndUntouched) {
    ScratchDir dir("storage-schema");
    const std::string path = dir.join("garbage.db");
    frost::storage::test::writeBytes(path, frost::storage::test::randomBytes(4096, 9));
    expectRefusedUntouched(path, kSpecV1, ErrorCode::Corrupt);
    // Text that starts like a database header is still refused.
    std::string fake = "SQLite format 3";
    fake.push_back('\0');
    fake += std::string(200, 'x');
    frost::storage::test::writeBytes(path, fake);
    expectRefusedUntouched(path, kSpecV1, ErrorCode::Corrupt);
}

TEST(Schema, RefusedFileWithWalUntouched) {
    ScratchDir dir("storage-schema");
    const std::string path = dir.join("s.db");
    {
        RawDb raw(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        raw.exec("PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;");
        raw.exec(std::format("PRAGMA application_id={}; PRAGMA user_version=9; CREATE TABLE a (x INTEGER);", kTestId));
        raw.exec("INSERT INTO a VALUES (1), (2);");
        sqlite3_db_config(raw.get(), SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, nullptr);
    }
    ASSERT_TRUE(fileExists(path + "-wal"));
    ASSERT_GT(frost::storage::test::fileSize(path + "-wal"), 0u);
    expectRefusedUntouched(path, kSpecV3, ErrorCode::Unsupported);
}

TEST(Schema, ZeroLengthFileIsFresh) {
    ScratchDir dir("storage-schema");
    const std::string path = dir.join("zero.db");
    frost::storage::test::writeBytes(path, "");
    {
        auto db = Database::open(path, withSpec(kSpecV3));
        ASSERT_TRUE(db.has_value()) << db.error().describe();
    }
    EXPECT_EQ(rawInt(path, "PRAGMA user_version"), 3);
}

TEST(Schema, EmptySchemaFileIsFresh) {
    ScratchDir dir("storage-schema");
    const std::string path = dir.join("empty.db");
    {
        RawDb raw(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        raw.exec("PRAGMA journal_mode=WAL;");
    }
    ASSERT_GT(frost::storage::test::fileSize(path), 0u);
    {
        auto db = Database::open(path, withSpec(kSpecV3Vacuum));
        ASSERT_TRUE(db.has_value()) << db.error().describe();
    }
    EXPECT_EQ(rawInt(path, "PRAGMA user_version"), 3);
    EXPECT_EQ(rawInt(path, "PRAGMA auto_vacuum"), 2); // INCREMENTAL
}

TEST(Schema, ReadOnlyOnOlderVersionIsUnsupported) {
    ScratchDir dir("storage-schema");
    const std::string path = dir.join("s.db");
    createAt(path, kSpecV1);
    expectRefusedUntouched(path, kSpecV3, ErrorCode::Unsupported, OpenMode::ReadOnly);
    // At the newest version a read-only open works.
    OpenOptions options = withSpec(kSpecV1);
    options.mode = OpenMode::ReadOnly;
    auto reader = Database::open(path, options);
    EXPECT_TRUE(reader.has_value()) << reader.error().describe();
}

TEST(Schema, QuickCheckRunsBeforeMigration) {
    ScratchDir dir("storage-schema");
    const std::string path = dir.join("s.db");
    std::int64_t rootPage = 0;
    std::int64_t pageSize = 0;
    {
        auto db = Database::open(path, withSpec(kSpecV1));
        ASSERT_TRUE(db.has_value());
        FROST_STORAGE_ASSERT_OK(db->execute("INSERT INTO a (x) VALUES (1), (2), (3)"));
        auto root = db->prepare("SELECT rootpage FROM sqlite_schema WHERE name = 'a'");
        ASSERT_TRUE(root.has_value());
        ASSERT_TRUE(root->step().value_or(false));
        rootPage = root->columnInt64(0);
        auto size = db->prepare("PRAGMA page_size");
        ASSERT_TRUE(size.has_value());
        ASSERT_TRUE(size->step().value_or(false));
        pageSize = size->columnInt64(0);
    }
    ASSERT_GT(rootPage, 1);
    // The b-tree page type byte of table a's root page.
    frost::storage::test::patchBytes(path, static_cast<std::size_t>((rootPage - 1) * pageSize), "\xFF");
    OpenOptions options = withSpec(kSpecV3);
    options.verify = Verify::QuickCheck;
    const std::string before = readBytes(path);
    {
        auto db = Database::open(path, options);
        ASSERT_FALSE(db.has_value());
        EXPECT_EQ(db.error().code(), ErrorCode::Corrupt) << db.error().describe();
    }
    EXPECT_EQ(readBytes(path), before);
    EXPECT_FALSE(fileExists(path + "-wal"));
}

/*
 * Eight threads open `path` at the same moment (a latch releases them). Every open must succeed
 * and the migrations must run once. Races found this way: the header read in separate
 * transactions (a creation committed between them looked like a foreign file), and the switch to
 * WAL, for which SQLite does not call the busy handler.
 */
void openConcurrently(const std::string& path, const SchemaSpec& spec) {
    constexpr std::size_t kThreads = 8;
    std::vector<std::string> failures(kThreads);
    std::latch start(static_cast<std::ptrdiff_t>(kThreads));
    {
        std::vector<std::jthread> threads;
        for (std::size_t i = 0; i < kThreads; ++i) {
            threads.emplace_back([&, i] {
                OpenOptions options = withSpec(spec);
                options.busyTimeout = std::chrono::seconds(20);
                start.arrive_and_wait();
                auto db = Database::open(path, options);
                if (!db) {
                    failures[i] = db.error().describe();
                }
            });
        }
    }
    for (const std::string& failure : failures) {
        EXPECT_TRUE(failure.empty()) << failure;
    }
    EXPECT_EQ(rawInt(path, "PRAGMA user_version"), 3);
    EXPECT_EQ(rawInt(path, "SELECT count(*) FROM runs"), 1);
}

// Rounds per concurrency test: the races are timing-dependent, so each test tries several files.
constexpr int kConcurrentRounds = 10;

TEST(Schema, ConcurrentFirstOpenMigratesOnce) {
    ScratchDir dir("storage-schema");
    for (int round = 0; round < kConcurrentRounds; ++round) {
        SCOPED_TRACE(round);
        openConcurrently(dir.join(std::format("s{}.db", round)), kSpecV3);
    }
}

TEST(Schema, ConcurrentUpgradeMigratesOnce) {
    ScratchDir dir("storage-schema");
    for (int round = 0; round < kConcurrentRounds; ++round) {
        SCOPED_TRACE(round);
        const std::string path = dir.join(std::format("s{}.db", round));
        createAt(path, kSpecV1);
        openConcurrently(path, kSpecV3);
    }
}

// A file with page 1 but no schema (fresh, so it gets the fresh pragmas and a VACUUM), raced by
// eight first opens: every open succeeds, the schema exists once and auto_vacuum took effect.
TEST(Schema, ConcurrentFirstOpenOfPagedFreshFile) {
    ScratchDir dir("storage-schema");
    for (int round = 0; round < kConcurrentRounds; ++round) {
        SCOPED_TRACE(round);
        const std::string path = dir.join(std::format("paged{}.db", round));
        {
            RawDb raw(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
            raw.exec("PRAGMA journal_mode=WAL;");
        }
        ASSERT_GT(frost::storage::test::fileSize(path), 0u);
        openConcurrently(path, kSpecV3Vacuum);
        EXPECT_EQ(rawInt(path, "PRAGMA auto_vacuum"), 2); // INCREMENTAL
    }
}

void expectGolden(const std::string& name, const std::string& actual) {
    const std::string path = std::string(FROST_STORAGE_GOLDEN_DIR) + "/" + name;
    if (frost::storage::test::environment("FROST_STORAGE_REGENERATE_GOLDENS") == "1") {
        std::ofstream(path, std::ios::binary) << actual;
    }
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in) << "missing golden " << path;
    std::stringstream buffer;
    buffer << in.rdbuf();
    EXPECT_EQ(buffer.str(), actual) << "the DDL changed: add a migration instead of editing version 1";
}

TEST(Schema, JournalSchemaMatchesGolden) {
    ScratchDir dir("storage-schema");
    auto journal = frost::storage::HotExitJournal::create(dir.path());
    ASSERT_TRUE(journal.has_value()) << journal.error().describe();
    auto described = frost::storage::describeSchema(journal->database());
    ASSERT_TRUE(described.has_value());
    expectGolden("hot_exit_journal.v1.schema.txt", *described);
}

TEST(Schema, LocalHistorySchemaMatchesGolden) {
    ScratchDir dir("storage-schema");
    auto history = frost::storage::LocalHistory::open(dir.join("history.db"));
    ASSERT_TRUE(history.has_value()) << history.error().describe();
    auto described = frost::storage::describeSchema(history->database());
    ASSERT_TRUE(described.has_value());
    expectGolden("local_history.v1.schema.txt", *described);
}

TEST(Schema, FreshPragmasSetIncrementalAutoVacuum) {
    ScratchDir dir("storage-schema");
    const std::string path = dir.join("history.db");
    {
        auto history = frost::storage::LocalHistory::open(path);
        ASSERT_TRUE(history.has_value());
    }
    EXPECT_EQ(rawInt(path, "PRAGMA auto_vacuum"), 2);
    EXPECT_EQ(rawInt(path, "PRAGMA application_id"), 0x46724C48);
    // A plain database keeps SQLite's default (none).
    const std::string plain = dir.join("plain.db");
    createAt(plain, kSpecV3);
    EXPECT_EQ(rawInt(plain, "PRAGMA auto_vacuum"), 0);
}

} // namespace
