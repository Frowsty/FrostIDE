// SPDX-License-Identifier: Apache-2.0
//
// The indicative measurements of ADR 0014 (not a test: it asserts only that the operations
// succeed). Disabled, so ctest never runs it; run it by hand on a Release build:
//   storage_stress_tests --gtest_also_run_disabled_tests --gtest_filter=StorageMeasure.*
// It prints one "measure <what>: <value>" line per item.

#include "frost/storage/database.hpp"
#include "frost/storage/fingerprint.hpp"
#include "frost/storage/hot_exit_journal.hpp"
#include "frost/storage/local_history.hpp"
#include "frost/storage/schema.hpp"
#include "frost/storage/store.hpp"

#include "support/scratch_dir.hpp"
#include "support/storage_test_support.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <format>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;
using frost::storage::Synchronous;
using frost::test::ScratchDir;

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void report(std::string_view what, std::string_view value) {
    std::printf("measure %.*s: %.*s\n", static_cast<int>(what.size()), what.data(), static_cast<int>(value.size()),
                value.data());
    std::fflush(stdout);
}

TEST(StorageMeasure, DISABLED_Indicative) {
    ScratchDir dir("storage-measure");
    const std::string data64 = frost::storage::test::randomBytes(std::size_t{64} << 20, 1);

    // XXH3 throughput on 64 MiB (best of 5).
    for (const bool wide : {false, true}) {
        double best = 1e30;
        std::uint64_t sink = 0;
        for (int run = 0; run < 5; ++run) {
            const auto start = Clock::now();
            sink += wide ? frost::storage::xxh3Hash128(data64).low : frost::storage::xxh3Hash64(data64);
            best = std::min(best, millisecondsSince(start));
        }
        report(wide ? "xxh3-128 64 MiB" : "xxh3-64 64 MiB",
               std::format("{:.1f} ms, {:.1f} GB/s (sink {:x})", best, 64.0 / 1024.0 / (best / 1000.0), sink & 0xF));
    }

    // Local history: fresh open (creation and schema) and existing open.
    {
        frost::storage::HistoryOptions options;
        const std::string path = dir.join("history.db");
        auto start = Clock::now();
        {
            auto history = frost::storage::LocalHistory::open(path, frost::systemClock(), options);
            ASSERT_TRUE(history.has_value());
        }
        report("history fresh open+close", std::format("{:.2f} ms", millisecondsSince(start)));
        start = Clock::now();
        auto history = frost::storage::LocalHistory::open(path, frost::systemClock(), options);
        ASSERT_TRUE(history.has_value());
        report("history existing open", std::format("{:.2f} ms", millisecondsSince(start)));
        const std::string mib = frost::storage::test::randomBytes(std::size_t{1} << 20, 2);
        const std::string_view pieces[] = {mib};
        start = Clock::now();
        ASSERT_TRUE(history->record("k", "/k", frost::storage::RevisionKind::Save, {}, pieces).has_value());
        report("history record 1 MiB (Full)", std::format("{:.2f} ms", millisecondsSince(start)));
    }

    // Journal puts and a 64 MiB read.
    for (const Synchronous sync : {Synchronous::Normal, Synchronous::Full}) {
        const std::string journalDir = dir.join(sync == Synchronous::Full ? "jfull" : "jnormal");
        frost::storage::JournalOptions options;
        options.synchronous = sync;
        auto journal = frost::storage::HotExitJournal::create(journalDir, frost::systemClock(), options);
        ASSERT_TRUE(journal.has_value());
        for (const std::size_t size : {std::size_t{1} << 20, std::size_t{64} << 20}) {
            frost::storage::DocumentState state;
            state.documentKey = std::format("doc{}", size);
            state.generation = 1;
            const std::string_view pieces[] = {std::string_view(data64).substr(0, size)};
            const auto start = Clock::now();
            ASSERT_TRUE(journal->put(state, pieces).has_value());
            report(std::format("journal put {} MiB ({})", size >> 20, sync == Synchronous::Full ? "Full" : "Normal"),
                   std::format("{:.1f} ms", millisecondsSince(start)));
        }
        if (sync == Synchronous::Full) {
            auto entries = journal->entries();
            ASSERT_TRUE(entries.has_value());
            static_cast<void>(journal->closeCleanly());
            auto scan = frost::storage::findRecoverableJournals(journalDir);
            ASSERT_TRUE(scan.has_value() && scan->journals.size() == 1);
            auto recovered = std::move(scan->journals.front());
            auto all = recovered.entries();
            ASSERT_TRUE(all.has_value());
            for (const auto& entry : *all) {
                if (entry.contentSize == (std::uint64_t{64} << 20)) {
                    std::uint64_t bytes = 0;
                    const auto start = Clock::now();
                    ASSERT_TRUE(recovered
                                    .readContent(entry.id,
                                                 [&](std::string_view chunk) -> frost::Result<void> {
                                                     bytes += chunk.size();
                                                     return {};
                                                 })
                                    .has_value());
                    report("journal readContent 64 MiB", std::format("{:.1f} ms", millisecondsSince(start)));
                }
            }
        }
    }

    // Close with a 1000-page WAL (B20).
    {
        frost::storage::OpenOptions options;
        options.walAutoCheckpointPages = 0;
        const std::string path = dir.join("close.db");
        auto db = frost::storage::Database::open(path, options);
        ASSERT_TRUE(db.has_value());
        ASSERT_TRUE(db->execute("CREATE TABLE t (b BLOB)").has_value());
        auto filled = db->inTransaction(frost::storage::TransactionKind::Immediate, [&]() -> frost::Result<void> {
            return db->execute("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 1000) "
                               "INSERT INTO t SELECT randomblob(3900) FROM n");
        });
        ASSERT_TRUE(filled.has_value());
        const auto walBytes = frost::storage::test::fileSize(path + "-wal");
        const auto start = Clock::now();
        ASSERT_TRUE(db->close().has_value());
        report("close with WAL", std::format("{:.1f} ms for {} pages", millisecondsSince(start), walBytes / 4120));
    }

    // Prune of 10k revisions (record 10k small revisions, then prune them all by age).
    {
        frost::ManualClock clock(frost::IClock::SteadyTime{},
                                 frost::IClock::SystemTime(std::chrono::seconds(1'700'000'000)));
        frost::storage::HistoryOptions options;
        options.synchronous = Synchronous::Normal;
        auto history = frost::storage::LocalHistory::open(dir.join("prune.db"), clock, options);
        ASSERT_TRUE(history.has_value());
        for (int i = 0; i < 10'000; ++i) {
            const std::string content =
                std::format("revision {} {}", i, std::string(static_cast<std::size_t>(i % 200), 'x'));
            const std::string_view pieces[] = {content};
            ASSERT_TRUE(
                history->record(std::format("f{}", i % 50), "/f", frost::storage::RevisionKind::Save, {}, pieces)
                    .has_value());
        }
        clock.advance(std::chrono::hours(48));
        const auto start = Clock::now();
        auto pruned = history->prune(frost::storage::RetentionPolicy(std::chrono::hours(1), 1, false));
        ASSERT_TRUE(pruned.has_value());
        report("prune 10k revisions", std::format("{:.1f} ms ({} revisions, {} blobs)", millisecondsSince(start),
                                                  pruned->revisionsRemoved, pruned->blobsRemoved));
    }

    // Quarantine copy of a 256 MiB file.
    {
        const std::string path = dir.join("garbage.db");
        std::string garbage;
        garbage.reserve(std::size_t{256} << 20);
        for (int i = 0; i < 4; ++i) {
            garbage += data64;
        }
        garbage[0] = 'X'; // Not an SQLite header.
        frost::storage::test::writeBytes(path, garbage);
        garbage.clear();
        static constexpr frost::storage::Migration kMigrations[] = {{1, "CREATE TABLE t (v INTEGER);", nullptr}};
        static constexpr frost::storage::SchemaSpec kSpec{"measure", 0x4D454153, {}, kMigrations};
        frost::storage::OpenOptions options;
        options.schema = &kSpec;
        const auto start = Clock::now();
        auto outcome = frost::storage::openStore(path, options, frost::storage::CorruptPolicy::Quarantine);
        ASSERT_TRUE(outcome.has_value() && outcome->quarantined.has_value());
        report("quarantine 256 MiB", std::format("{:.1f} ms", millisecondsSince(start)));
    }
}

} // namespace
