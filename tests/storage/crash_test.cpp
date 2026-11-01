// SPDX-License-Identifier: Apache-2.0
//
// Crash consistency: storage_test_child (started through io::realProcessLauncher()) is killed
// with IProcess::terminate(), which is TerminateProcess on its job or SIGKILL on its group (kill
// -9), at random points after a reported commit, put or record. The survivors are checked through
// the raw sqlite3 API (support/crash_check.hpp): every reported commit survives a process crash in
// both synchronous modes, every transaction present is complete, and journals recover the old or
// the new generation byte for byte.

#include "frost/storage/database.hpp"
#include "frost/storage/hot_exit_journal.hpp"
#include "frost/storage/store.hpp"

#include "support/child_process.hpp"
#include "support/crash_check.hpp"
#include "support/crash_protocol.hpp"
#include "support/scratch_dir.hpp"
#include "support/storage_test_support.hpp"
#include <gtest/gtest.h>

#include <random>
#include <set>
#include <string>

namespace {

using frost::ErrorCode;
using frost::storage::findRecoverableJournals;
using frost::storage::RecoveredJournal;
using frost::storage::test::fileExists;
using frost::storage::test::RawDb;
using frost::storage::test::StorageChild;
using frost::test::ScratchDir;

constexpr std::uint64_t kSeed = 0xC0FFEE;

TEST(StorageCrash, KillMidTransactionKeepsCommitted) {
    ScratchDir dir("storage-crash");
    const std::string path = dir.join("crash.db");
    auto child = StorageChild::start({"hold-mid", path, std::to_string(kSeed)});
    ASSERT_NE(child, nullptr);
    ASSERT_TRUE(child->waitFor("mid").has_value());
    child->kill();
    const std::set<std::int64_t> printed = frost::storage::test::printedCommits(child->lines());
    EXPECT_EQ(printed, (std::set<std::int64_t>{1, 2, 3, 4, 5}));
    EXPECT_EQ(frost::storage::test::verifyCrashStore(path, kSeed, printed), 5);
    RawDb raw(path);
    EXPECT_EQ(raw.queryInt("SELECT count(*) FROM crash_data WHERE txn = 6"), 0); // The half transaction.
}

class StorageCrashBySync : public ::testing::TestWithParam<const char*> {};

TEST_P(StorageCrashBySync, RandomKillsAtomicAndDurable) {
    ScratchDir dir("storage-crash");
    const std::string path = dir.join("crash.db");
    std::mt19937_64 rng(std::string_view(GetParam()) == "full" ? 11 : 12);
    std::set<std::int64_t> printed;
    for (int kill = 0; kill < 20; ++kill) {
        SCOPED_TRACE(std::format("kill {}", kill));
        frost::storage::test::killTxnLoopOnce(path, GetParam(), kSeed, rng, printed);
        if (HasFatalFailure()) {
            return;
        }
    }
    EXPECT_GE(printed.size(), 20u);
}

INSTANTIATE_TEST_SUITE_P(Sync, StorageCrashBySync, ::testing::Values("normal", "full"),
                         [](const ::testing::TestParamInfo<const char*>& info) { return std::string(info.param); });

TEST(StorageCrash, KillDuringCheckpoints) {
    ScratchDir dir("storage-crash");
    const std::string path = dir.join("checkpoints.db");
    std::mt19937_64 rng(13);
    std::set<std::int64_t> printed;
    for (int kill = 0; kill < 10; ++kill) {
        SCOPED_TRACE(std::format("kill {}", kill));
        frost::storage::test::killTxnLoopOnce(path, "normal", kSeed, rng, printed, "checkpoint-loop");
        if (HasFatalFailure()) {
            return;
        }
    }
}

TEST(StorageCrash, QuarantineBusyWhileChildHoldsFile) {
    ScratchDir dir("storage-crash");
    const std::string path = dir.join("bank.db");
    auto child = StorageChild::start({"bank-writer", path, "5"});
    ASSERT_NE(child, nullptr);
    ASSERT_TRUE(child->waitFor("ready").has_value());
    // To a store with another application_id the file is foreign (Corrupt): quarantine needs the
    // exclusive lock, which the child's connection prevents.
    static constexpr frost::storage::Migration kOther[] = {{1, "CREATE TABLE other (v INTEGER) STRICT;", nullptr}};
    static constexpr frost::storage::SchemaSpec kOtherSpec{"other store", 0x4F544852, {}, kOther};
    frost::storage::OpenOptions options;
    options.schema = &kOtherSpec;
    options.busyTimeout = std::chrono::milliseconds(300);
    auto outcome = frost::storage::openStore(path, options, frost::storage::CorruptPolicy::Quarantine);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code(), ErrorCode::Busy) << outcome.error().describe();
    EXPECT_TRUE(fileExists(path));
    auto listing = frost::io::realFileSystem().listDirectory(dir.path());
    ASSERT_TRUE(listing.has_value());
    for (const auto& entry : *listing) {
        EXPECT_EQ(entry.name.find(".corrupt-"), std::string::npos) << entry.name;
    }
    ASSERT_TRUE(child->waitFor("transfer ", 2).has_value()); // The child kept working.
    child->kill();
    RawDb raw(path);
    sqlite3_busy_timeout(raw.get(), 5000);
    EXPECT_EQ(raw.queryInt("SELECT sum(balance) FROM bank"), frost::storage::test::kBankTotal);
    EXPECT_TRUE(raw.integrityOk());
}

TEST(HotExitJournalCrash, KilledPutLeavesOldOrNew) {
    ScratchDir dir("storage-journal-crash");
    std::mt19937_64 rng(21);
    for (int kill = 0; kill < 10; ++kill) {
        SCOPED_TRACE(std::format("kill {}", kill));
        frost::storage::test::killJournalPutOnce(dir.join(std::format("k{}", kill)), 300 * 1024,
                                                 kSeed + static_cast<std::uint64_t>(kill), rng);
        if (HasFatalFailure()) {
            return;
        }
    }
}

TEST(HotExitJournalCrash, KilledCreateIsFreshOrComplete) {
    ScratchDir dir("storage-journal-crash");
    std::mt19937_64 rng(22);
    for (int kill = 0; kill < 5; ++kill) {
        SCOPED_TRACE(std::format("kill {}", kill));
        const std::string journals = dir.join(std::format("k{}", kill));
        auto child = StorageChild::start({"journal-create-loop", journals});
        ASSERT_NE(child, nullptr);
        ASSERT_TRUE(child->waitFor("created ", 1 + rng() % 4).has_value());
        frost::storage::test::randomKillDelay(rng);
        child->kill();
        auto scan = findRecoverableJournals(journals, std::chrono::seconds(5));
        ASSERT_TRUE(scan.has_value()) << scan.error().describe();
        EXPECT_TRUE(scan->unreadable.empty());
        EXPECT_TRUE(scan->skipped.empty());
        EXPECT_EQ(scan->inUse, 0u);
        EXPECT_LE(scan->abandoned, 1u);
        std::size_t created = 0;
        for (const std::string& text : child->lines()) {
            created += text.starts_with("created ") ? 1 : 0;
        }
        EXPECT_GE(scan->journals.size(), created);
        EXPECT_LE(scan->journals.size(), created + 1);
        for (RecoveredJournal& journal : scan->journals) {
            EXPECT_FALSE(journal.damaged());
            auto entries = journal.entries();
            ASSERT_TRUE(entries.has_value()) << entries.error().describe();
            EXPECT_LE(entries->size(), 1u);
            for (const auto& entry : *entries) {
                auto content = journal.readContent(entry.id);
                ASSERT_TRUE(content.has_value()) << content.error().describe();
                EXPECT_TRUE(*content == frost::storage::test::journalContent(7, 1, 5000));
            }
            ASSERT_TRUE(std::move(journal).discard().has_value());
        }
    }
}

TEST(HotExitJournalCrash, LiveJournalIsSkipped) {
    ScratchDir dir("storage-journal-crash");
    auto child = StorageChild::start({"hold-journal", dir.path()});
    ASSERT_NE(child, nullptr);
    ASSERT_TRUE(child->waitFor("ready ").has_value());
    auto scan = findRecoverableJournals(dir.path(), std::chrono::milliseconds(200));
    ASSERT_TRUE(scan.has_value()) << scan.error().describe();
    EXPECT_EQ(scan->inUse, 1u);
    EXPECT_TRUE(scan->journals.empty());
    EXPECT_TRUE(scan->unreadable.empty());
    child->kill();
}

TEST(HotExitJournalCrash, CrashedJournalIsRecoverable) {
    ScratchDir dir("storage-journal-crash");
    auto child = StorageChild::start({"hold-journal", dir.path()});
    ASSERT_NE(child, nullptr);
    const auto ready = child->waitFor("ready ");
    ASSERT_TRUE(ready.has_value());
    const std::string path = ready->substr(6);
    child->kill();
    auto scan = findRecoverableJournals(dir.path(), std::chrono::seconds(5));
    ASSERT_TRUE(scan.has_value()) << scan.error().describe();
    ASSERT_EQ(scan->journals.size(), 1u);
    RecoveredJournal recovered = std::move(scan->journals.front());
    EXPECT_EQ(recovered.path(), *frost::io::normalizePath(path));
    EXPECT_FALSE(recovered.endedCleanly());
    auto entries = recovered.entries();
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 1u);
    EXPECT_TRUE(recovered.readContent((*entries)[0].id).value() == frost::storage::test::journalContent(7, 1, 5000));
    ASSERT_TRUE(std::move(recovered).discard().has_value());
    for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
        EXPECT_FALSE(fileExists(path + suffix)) << suffix;
    }
}

TEST(HotExitJournalCrash, CleanExitIsHotExit) {
    ScratchDir dir("storage-journal-crash");
    auto child = StorageChild::start({"exit-clean", dir.path()});
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->waitForExit(), 0) << child->output();
    auto scan = findRecoverableJournals(dir.path(), std::chrono::seconds(5));
    ASSERT_TRUE(scan.has_value());
    ASSERT_EQ(scan->journals.size(), 1u) << child->output();
    RecoveredJournal recovered = std::move(scan->journals.front());
    EXPECT_TRUE(recovered.endedCleanly());
    EXPECT_EQ(recovered.entries().value().size(), 1u);
    ASSERT_TRUE(std::move(recovered).discard().has_value());
}

TEST(LocalHistoryCrash, KilledRecordKeepsBlobsConsistent) {
    ScratchDir dir("storage-history-crash");
    const std::string path = dir.join("history.db");
    std::mt19937_64 rng(31);
    std::uint64_t printedTotal = 0;
    constexpr std::uint64_t kKills = 6;
    for (std::uint64_t kill = 0; kill < kKills; ++kill) {
        SCOPED_TRACE(std::format("kill {}", kill));
        auto child = StorageChild::start({"history-record-loop", path, std::to_string(kSeed + kill)});
        ASSERT_NE(child, nullptr);
        ASSERT_TRUE(child->waitFor("record ", 1 + rng() % 8).has_value());
        frost::storage::test::randomKillDelay(rng);
        child->kill();
        printedTotal += static_cast<std::uint64_t>(std::max<std::int64_t>(child->lastNumber("record "), 0));
        frost::storage::test::verifyHistoryStore(path, printedTotal, kill + 1);
        if (HasFatalFailure()) {
            return;
        }
    }
}

} // namespace
