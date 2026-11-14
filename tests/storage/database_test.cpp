// SPDX-License-Identifier: Apache-2.0
//
// Database: the open sequence (paths, modes, pragmas, read-only files, long paths), the WAL
// hook, checkpoints, close behaviour, integrity checks (and their cancellation), SQL text with a
// NUL byte, and moves.

#include "frost/io/path.hpp"
#include "frost/storage/database.hpp"

#include "support/fault_vfs.hpp"
#include "support/hook_guard.hpp"
#include "support/scratch_dir.hpp"
#include "support/storage_test_support.hpp"
#include <gtest/gtest.h>

#include <stop_token>
#include <string>
#include <string_view>

#include <sqlite3.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>

#include <unistd.h>
#endif

namespace {

using frost::ErrorCode;
using frost::storage::CheckpointMode;
using frost::storage::Database;
using frost::storage::OpenMode;
using frost::storage::OpenOptions;
using frost::storage::Synchronous;
using frost::storage::TransactionKind;
using frost::storage::test::fastOptions;
using frost::storage::test::fileExists;
using frost::storage::test::fileSize;
using frost::test::ScratchDir;

std::string pragmaText(Database& db, std::string_view pragma) {
    auto statement = db.prepare(std::string("PRAGMA ") + std::string(pragma));
    EXPECT_TRUE(statement.has_value());
    auto row = statement->step();
    EXPECT_TRUE(row.has_value() && *row);
    return std::string(statement->columnText(0));
}

// Inserts `rows` rows of `bytes` random bytes each, one transaction per row.
void fill(Database& db, int rows, std::size_t bytes) {
    for (int i = 0; i < rows; ++i) {
        auto insert = db.prepare("INSERT INTO t (b) VALUES (?1)");
        ASSERT_TRUE(insert.has_value());
        FROST_STORAGE_ASSERT_OK(
            insert->bindBlob(1, frost::storage::test::randomBytes(bytes, static_cast<std::uint64_t>(i))));
        FROST_STORAGE_ASSERT_OK(insert->run());
    }
}

TEST(Database, OpenAppliesPragmas) {
    ScratchDir dir("storage-db");
    OpenOptions options;
    options.cacheSizeKiB = 1234;
    auto db = Database::open(dir.join("p.db"), options);
    ASSERT_TRUE(db.has_value()) << db.error().describe();
    EXPECT_EQ(pragmaText(*db, "journal_mode"), "wal");
    EXPECT_EQ(pragmaText(*db, "synchronous"), "2"); // FULL
    EXPECT_EQ(pragmaText(*db, "foreign_keys"), "1");
    EXPECT_EQ(pragmaText(*db, "trusted_schema"), "0");
    EXPECT_EQ(pragmaText(*db, "temp_store"), "2"); // MEMORY
    EXPECT_EQ(pragmaText(*db, "cache_size"), "-1234");
    EXPECT_EQ(pragmaText(*db, "cell_size_check"), "1");
    EXPECT_EQ(pragmaText(*db, "journal_size_limit"), std::to_string(std::int64_t{64} << 20));
    int defensive = -1;
    sqlite3_db_config(db->nativeHandle(), SQLITE_DBCONFIG_DEFENSIVE, -1, &defensive);
    EXPECT_EQ(defensive, 1);
    int extensions = -1;
    sqlite3_db_config(db->nativeHandle(), SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, -1, &extensions);
    EXPECT_EQ(extensions, 0);
    EXPECT_EQ(db->path(), dir.join("p.db"));
    auto normal = Database::open(dir.join("n.db"), fastOptions());
    ASSERT_TRUE(normal.has_value());
    EXPECT_EQ(pragmaText(*normal, "synchronous"), "1"); // NORMAL
}

TEST(Database, WalHookBoundsWal) {
    ScratchDir dir("storage-db");
    for (const std::uint32_t threshold : {std::uint32_t{0}, std::uint32_t{50}}) {
        OpenOptions options = fastOptions();
        options.walAutoCheckpointPages = threshold;
        auto db = Database::open(dir.join(std::format("w{}.db", threshold)), options);
        ASSERT_TRUE(db.has_value());
        FROST_STORAGE_ASSERT_OK(db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, b BLOB) STRICT"));
        fill(*db, 200, 3000); // About one page of overflow per row.
        auto state = db->checkpoint(CheckpointMode::Passive);
        ASSERT_TRUE(state.has_value());
        if (threshold == 0) {
            EXPECT_GT(state->walFrames, 200) << "without the hook the WAL keeps growing";
        } else {
            // The threshold plus the last transaction's frames (a handful of pages).
            EXPECT_LT(state->walFrames, static_cast<int>(threshold) + 10);
        }
    }
}

TEST(Database, RelativeAndNulPathsRejected) {
    auto relative = Database::open("relative.db");
    ASSERT_FALSE(relative.has_value());
    EXPECT_EQ(relative.error().code(), ErrorCode::InvalidArgument);
    ScratchDir dir("storage-db");
    auto nul = Database::open(dir.join("a") + std::string("\0b.db", 5));
    ASSERT_FALSE(nul.has_value());
    EXPECT_EQ(nul.error().code(), ErrorCode::InvalidArgument);
    OpenOptions unknownVfs;
    unknownVfs.vfs = "no-such-vfs";
    auto vfs = Database::open(dir.join("v.db"), unknownVfs);
    ASSERT_FALSE(vfs.has_value());
    EXPECT_EQ(vfs.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(fileExists(dir.join("v.db")));
}

TEST(Database, MissingParentIsNotFound) {
    ScratchDir dir("storage-db");
    auto db = Database::open(dir.join("missing/child.db"));
    ASSERT_FALSE(db.has_value());
    EXPECT_EQ(db.error().code(), ErrorCode::NotFound);
}

TEST(Database, DirectoryIsInvalidArgument) {
    ScratchDir dir("storage-db");
    auto db = Database::open(dir.path());
    ASSERT_FALSE(db.has_value());
    EXPECT_EQ(db.error().code(), ErrorCode::InvalidArgument);
}

TEST(Database, ReadOnlyFileIsPermissionDenied) {
    ScratchDir dir("storage-db");
    const std::string path = dir.join("ro.db");
    {
        auto db = Database::open(path, fastOptions());
        ASSERT_TRUE(db.has_value());
        FROST_STORAGE_ASSERT_OK(db->execute("CREATE TABLE t (v INTEGER)"));
        FROST_STORAGE_ASSERT_OK(db->close());
    }
#ifdef _WIN32
    auto wide = frost::io::toWin32Path(path);
    ASSERT_TRUE(wide.has_value());
    ASSERT_NE(SetFileAttributesW(wide->c_str(), FILE_ATTRIBUTE_READONLY), 0);
#else
    if (geteuid() == 0) {
        GTEST_SKIP() << "root ignores file modes";
    }
    ASSERT_EQ(chmod(path.c_str(), 0444), 0);
#endif
    OpenOptions options = fastOptions();
    options.mode = OpenMode::ReadWrite;
    auto db = Database::open(path, options);
    EXPECT_FALSE(db.has_value());
    if (!db) {
        EXPECT_EQ(db.error().code(), ErrorCode::PermissionDenied) << db.error().describe();
    }
    db = std::unexpected(frost::Error(ErrorCode::Unknown)); // Close before the attributes change.
    options.mode = OpenMode::ReadOnly;
    auto reader = Database::open(path, options);
    // A read-only WAL file without its -shm cannot be read; a rollback-mode one can. Either way no write happened.
    if (reader) {
        reader = std::unexpected(frost::Error(ErrorCode::Unknown));
    }
#ifdef _WIN32
    ASSERT_NE(SetFileAttributesW(wide->c_str(), FILE_ATTRIBUTE_NORMAL), 0);
#else
    ASSERT_EQ(chmod(path.c_str(), 0644), 0);
#endif
}

TEST(Database, OpenModeReadWriteRequiresFile) {
    ScratchDir dir("storage-db");
    for (const OpenMode mode : {OpenMode::ReadWrite, OpenMode::ReadOnly}) {
        OpenOptions options;
        options.mode = mode;
        auto db = Database::open(dir.join("absent.db"), options);
        ASSERT_FALSE(db.has_value());
        EXPECT_EQ(db.error().code(), ErrorCode::NotFound);
        EXPECT_FALSE(fileExists(dir.join("absent.db")));
    }
}

TEST(Database, AllowFreshFalseIsNotFoundAndLeavesFileEmpty) {
    ScratchDir dir("storage-db");
    const std::string path = dir.join("empty.db");
    frost::storage::test::writeBytes(path, "");
    OpenOptions options;
    options.allowFresh = false;
    options.mode = OpenMode::ReadWrite;
    auto db = Database::open(path, options);
    ASSERT_FALSE(db.has_value());
    EXPECT_EQ(db.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fileSize(path), 0u);
    EXPECT_FALSE(fileExists(path + "-wal"));
    EXPECT_FALSE(fileExists(path + "-journal"));
    // A missing file with allowFresh=false is not created either.
    options.mode = OpenMode::ReadWriteCreate;
    auto created = Database::open(dir.join("new.db"), options);
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().code(), ErrorCode::NotFound);
}

TEST(Database, CheckpointModesAndTruncateEmptiesWal) {
    ScratchDir dir("storage-db");
    const std::string path = dir.join("c.db");
    OpenOptions options = fastOptions();
    options.walAutoCheckpointPages = 0;
    auto db = Database::open(path, options);
    ASSERT_TRUE(db.has_value());
    FROST_STORAGE_ASSERT_OK(db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, b BLOB) STRICT"));
    fill(*db, 20, 2000);
    for (const CheckpointMode mode : {CheckpointMode::Passive, CheckpointMode::Full, CheckpointMode::Restart}) {
        auto result = db->checkpoint(mode);
        ASSERT_TRUE(result.has_value()) << result.error().describe();
        EXPECT_TRUE(result->complete);
        EXPECT_EQ(result->walFrames, result->checkpointedFrames);
        fill(*db, 2, 2000);
    }
    EXPECT_GT(fileSize(path + "-wal"), 0u);
    auto truncated = db->checkpoint(CheckpointMode::Truncate);
    ASSERT_TRUE(truncated.has_value());
    EXPECT_TRUE(truncated->complete);
    EXPECT_EQ(fileSize(path + "-wal"), 0u);
    // In memory there is no WAL: -1 frames, complete.
    auto memory = Database::openInMemory();
    ASSERT_TRUE(memory.has_value());
    auto none = memory->checkpoint();
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none->walFrames, -1);
}

TEST(Database, CloseCheckpointsAndRemovesWalShm) {
    ScratchDir dir("storage-db");
    const std::string path = dir.join("close.db");
    auto db = Database::open(path, fastOptions());
    ASSERT_TRUE(db.has_value());
    FROST_STORAGE_ASSERT_OK(db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, b BLOB) STRICT"));
    fill(*db, 5, 100);
    EXPECT_TRUE(fileExists(path + "-wal"));
    EXPECT_TRUE(fileExists(path + "-shm"));
    FROST_STORAGE_ASSERT_OK(db->close());
    EXPECT_FALSE(db->isOpen());
    EXPECT_FALSE(fileExists(path + "-wal"));
    EXPECT_FALSE(fileExists(path + "-shm"));
    frost::storage::test::RawDb raw(path);
    EXPECT_EQ(raw.queryInt("SELECT count(*) FROM t"), 5);
}

TEST(Database, NoCheckpointOnCloseKeepsWal) {
    ScratchDir dir("storage-db");
    const std::string path = dir.join("keep.db");
    OpenOptions options = fastOptions();
    options.checkpointOnClose = false;
    options.walAutoCheckpointPages = 0;
    auto db = Database::open(path, options);
    ASSERT_TRUE(db.has_value());
    FROST_STORAGE_ASSERT_OK(db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, b BLOB) STRICT"));
    fill(*db, 5, 100);
    FROST_STORAGE_ASSERT_OK(db->close());
    EXPECT_GT(fileSize(path + "-wal"), 0u);
    // The data is in the WAL and is read back from it.
    auto reopened = Database::open(path, fastOptions());
    ASSERT_TRUE(reopened.has_value());
    auto statement = reopened->prepare("SELECT count(*) FROM t");
    ASSERT_TRUE(statement.has_value());
    auto row = statement->step();
    ASSERT_TRUE(row.has_value() && *row);
    EXPECT_EQ(statement->columnInt64(0), 5);
}

TEST(Database, NonAsciiPathWithSpaces) {
    ScratchDir dir("storage-db");
    const std::string folder = dir.join("ü ñ 日本 \xF0\x9F\x98\x80");
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(folder).has_value());
    const std::string path = folder + "/d b \xC3\xA9.db";
    auto db = Database::open(path, fastOptions());
    ASSERT_TRUE(db.has_value()) << db.error().describe();
    FROST_STORAGE_ASSERT_OK(db->execute("CREATE TABLE t (v INTEGER); INSERT INTO t VALUES (42);"));
    FROST_STORAGE_ASSERT_OK(db->close());
    EXPECT_TRUE(fileExists(path));
    auto reopened = Database::open(path, fastOptions());
    ASSERT_TRUE(reopened.has_value());
    auto statement = reopened->prepare("SELECT v FROM t");
    ASSERT_TRUE(statement.has_value());
    auto row = statement->step();
    ASSERT_TRUE(row.has_value() && *row);
    EXPECT_EQ(statement->columnInt64(0), 42);
}

#ifdef _WIN32
TEST(Database, LongPathReachesVfsVerbatim) {
    ScratchDir dir("storage-db");
    std::string folder = dir.path();
    while (folder.size() < 300) {
        folder += "/" + std::string(60, 'd');
    }
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(folder).has_value());
    const std::string path = folder + "/long.db";
    auto& vfs = frost::storage::test::FaultVfs::instance();
    vfs.reset();
    OpenOptions options = fastOptions();
    options.vfs = frost::storage::test::FaultVfs::kName;
    auto db = Database::open(path, options);
    ASSERT_TRUE(db.has_value()) << db.error().describe();
    FROST_STORAGE_ASSERT_OK(db->execute("CREATE TABLE t (v INTEGER); INSERT INTO t VALUES (1);"));
    const auto names = vfs.openedNames();
    ASSERT_FALSE(names.empty());
    for (const std::string& name : names) {
        EXPECT_TRUE(name.starts_with("\\\\?\\")) << name;
    }
    EXPECT_EQ(db->path(), *frost::io::normalizePath(path));
    FROST_STORAGE_ASSERT_OK(db->close());
    EXPECT_TRUE(fileExists(path));
}
#else
TEST(Database, PathLongerThanVfsLimitIsInvalidArgument) {
    ScratchDir dir("storage-db");
    sqlite3_vfs* unix = sqlite3_vfs_find(nullptr);
    ASSERT_NE(unix, nullptr);
    const std::size_t limit = static_cast<std::size_t>(unix->mxPathname);
    // Directories of up to 200 bytes, so no component exceeds NAME_MAX.
    std::string folder = dir.path();
    while (folder.size() + 210 < limit - 20) {
        folder += "/" + std::string(200, 'd');
    }
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(folder).has_value());
    // The name plus "-journal" must stay below mxPathname.
    const std::size_t fits = limit - 8 - 1 - folder.size() - 1;
    const std::string ok = folder + "/" + std::string(fits - 3, 'a') + ".db";
    ASSERT_EQ(ok.size() + 8, limit - 1);
    auto opened = Database::open(ok, fastOptions());
    ASSERT_TRUE(opened.has_value()) << opened.error().describe();
    const std::string tooLong = folder + "/" + std::string(fits - 2, 'a') + ".db";
    auto refused = Database::open(tooLong, fastOptions());
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(fileExists(tooLong));
}
#endif

TEST(Database, MovedDatabaseKeepsStatementsValid) {
    auto opened = Database::openInMemory(fastOptions());
    ASSERT_TRUE(opened.has_value());
    Database first = std::move(*opened);
    FROST_STORAGE_ASSERT_OK(first.execute("CREATE TABLE t (v INTEGER); INSERT INTO t VALUES (1), (2);"));
    auto statement = first.prepare("SELECT sum(v) FROM t");
    ASSERT_TRUE(statement.has_value());
    auto transaction = first.begin(TransactionKind::Deferred);
    ASSERT_TRUE(transaction.has_value());
    const frost::storage::InterruptHandle handle = first.interruptHandle();
    Database second = std::move(first);
    // NOLINTNEXTLINE(bugprone-use-after-move): Assert the moved-from closed state.
    EXPECT_FALSE(first.isOpen());
    auto row = statement->step();
    ASSERT_TRUE(row.has_value() && *row);
    EXPECT_EQ(statement->columnInt64(0), 3);
    statement->reset();
    FROST_STORAGE_ASSERT_OK(transaction->commit());
    handle.interrupt(); // Still the same connection; nothing runs, so a no-op.
    Database third = std::move(second);
    statement = std::unexpected(frost::Error(ErrorCode::Unknown));
    FROST_STORAGE_ASSERT_OK(third.close());
}

TEST(Database, QuickAndIntegrityCheckHealthyAndCancellable) {
    ScratchDir dir("storage-db");
    auto db = Database::open(dir.join("i.db"), fastOptions());
    ASSERT_TRUE(db.has_value());
    FROST_STORAGE_ASSERT_OK(
        db->execute("CREATE TABLE p (id INTEGER PRIMARY KEY); CREATE TABLE c (p INTEGER REFERENCES p(id));"
                    "INSERT INTO p VALUES (1); INSERT INTO c VALUES (1);"));
    EXPECT_TRUE(db->quickCheck().has_value());
    EXPECT_TRUE(db->integrityCheck().has_value());
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = db->quickCheck(stop.get_token());
    ASSERT_FALSE(cancelled.has_value());
    EXPECT_EQ(cancelled.error().code(), ErrorCode::Cancelled);
    // A foreign-key violation (inserted with the checks off) is found by integrityCheck only.
    FROST_STORAGE_ASSERT_OK(db->execute("PRAGMA foreign_keys=OFF; INSERT INTO c VALUES (99); PRAGMA foreign_keys=ON;"));
    EXPECT_TRUE(db->quickCheck().has_value());
    auto violation = db->integrityCheck();
    ASSERT_FALSE(violation.has_value());
    EXPECT_EQ(violation.error().code(), ErrorCode::Corrupt);
}

// A stop requested after integrity_check passed cancels the foreign-key pass too.
TEST(Database, IntegrityCheckCancellableInTheForeignKeyPass) {
    auto db = Database::openInMemory(fastOptions());
    ASSERT_TRUE(db.has_value());
    FROST_STORAGE_ASSERT_OK(
        db->execute("CREATE TABLE p (id INTEGER PRIMARY KEY); CREATE TABLE c (p INTEGER REFERENCES p(id));"
                    "INSERT INTO p VALUES (1); INSERT INTO c VALUES (1);"));
    std::stop_source stop;
    frost::storage::test::HookGuard guard(
        frost::storage::detail::g_integrityBeforeForeignKeys,
        [](void* context) { static_cast<std::stop_source*>(context)->request_stop(); }, &stop);
    auto cancelled = db->integrityCheck(stop.get_token());
    ASSERT_TRUE(stop.stop_requested()) << "the hook did not run";
    ASSERT_FALSE(cancelled.has_value()) << "the foreign-key pass ignored the stop request";
    EXPECT_EQ(cancelled.error().code(), ErrorCode::Cancelled) << cancelled.error().describe();
}

// SQLite stops at a NUL byte: prepare would miss SQL after it, and execute could not advance.
TEST(Database, SqlWithNulIsInvalidArgument) {
    auto db = Database::openInMemory(fastOptions());
    ASSERT_TRUE(db.has_value());
    constexpr std::string_view kTwo("SELECT 1;\0SELECT 2;", 19);
    auto prepared = db->prepare(kTwo);
    ASSERT_FALSE(prepared.has_value()) << "prepare accepted SQL after a NUL";
    EXPECT_EQ(prepared.error().code(), ErrorCode::InvalidArgument) << prepared.error().describe();
    auto executed = db->execute(kTwo); // Looped forever before the check.
    ASSERT_FALSE(executed.has_value());
    EXPECT_EQ(executed.error().code(), ErrorCode::InvalidArgument) << executed.error().describe();
    auto leading = db->execute(std::string_view("\0CREATE TABLE t (v INTEGER)", 27));
    ASSERT_FALSE(leading.has_value());
    EXPECT_EQ(leading.error().code(), ErrorCode::InvalidArgument);
    // Nothing ran: the table does not exist.
    auto missing = db->prepare("SELECT count(*) FROM t");
    EXPECT_FALSE(missing.has_value());
}

// close() reports Busy, rather than success, when a statement prepared through nativeHandle()
// keeps the connection alive; finalizing it completes the deferred close.
TEST(Database, CloseReportsAStatementOutsideTheWrapper) {
    ScratchDir dir("storage-db");
    auto db = Database::open(dir.join("n.db"), fastOptions());
    ASSERT_TRUE(db.has_value()) << db.error().describe();
    sqlite3_stmt* foreign = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db->nativeHandle(), "SELECT 1", -1, &foreign, nullptr), SQLITE_OK);
    auto closed = db->close();
    EXPECT_FALSE(db->isOpen());
    ASSERT_FALSE(closed.has_value()) << "close() reported success with a statement still alive";
    EXPECT_EQ(closed.error().code(), ErrorCode::Busy) << closed.error().describe();
    EXPECT_EQ(sqlite3_finalize(foreign), SQLITE_OK); // Frees the zombie connection.
}

TEST(Database, ExclusiveLockLeavesNoShmOrJournal) {
    ScratchDir dir("storage-db");
    const std::string path = dir.join("x.db");
    OpenOptions options = fastOptions();
    options.exclusiveLock = true;
    auto db = Database::open(path, options);
    ASSERT_TRUE(db.has_value()) << db.error().describe();
    FROST_STORAGE_ASSERT_OK(db->execute("CREATE TABLE t (v INTEGER); INSERT INTO t VALUES (1);"));
    EXPECT_FALSE(fileExists(path + "-shm"));
    EXPECT_FALSE(fileExists(path + "-journal"));
    // Another connection cannot even read it.
    OpenOptions second = fastOptions();
    second.busyTimeout = std::chrono::milliseconds(50);
    second.mode = OpenMode::ReadWrite;
    auto other = Database::open(path, second);
    ASSERT_FALSE(other.has_value());
    EXPECT_EQ(other.error().code(), ErrorCode::Busy);
}

} // namespace
