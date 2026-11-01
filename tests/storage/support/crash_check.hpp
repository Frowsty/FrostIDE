// SPDX-License-Identifier: Apache-2.0
//
// The crash tests' verifiers, shared with the stress shards. They check a store that a killed
// storage_test_child wrote through the raw sqlite3 API and PRAGMA integrity_check, not through
// the wrapper (the same engine, but separate code paths: a consistency check), against the
// commit lines the child printed and the content crash_protocol.hpp regenerates (the independent
// knowledge). Test code only.
#pragma once

#include "frost/storage/hot_exit_journal.hpp"
#include "frost/storage/local_history.hpp"

#include "support/child_process.hpp"
#include "support/crash_protocol.hpp"
#include "support/storage_test_support.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <sqlite3.h>

namespace frost::storage::test {

// The commit numbers among `lines` ("commit <t>").
inline std::set<std::int64_t> printedCommits(const std::vector<std::string>& lines) {
    std::set<std::int64_t> out;
    for (const std::string& text : lines) {
        if (text.starts_with("commit ")) {
            out.insert(std::stoll(text.substr(7)));
        }
    }
    return out;
}

/*
 * Checks a crash store after a kill: integrity_check is "ok"; every ledger transaction is
 * complete (its row count and the XXH3-64 of its values match the ledger and crash_protocol); no
 * rows exist outside a ledger transaction; every printed commit is present. Then a Truncate
 * checkpoint must leave a 0-byte -wal, checked with io::status while the connection is open.
 * Returns the highest transaction present.
 */
inline std::int64_t verifyCrashStore(const std::string& path, std::uint64_t seed,
                                     const std::set<std::int64_t>& printed) {
    RawDb raw(path);
    sqlite3_busy_timeout(raw.get(), 5000); // Windows can release a killed process's locks late.
    EXPECT_EQ(raw.queryText("PRAGMA integrity_check"), "ok");
    std::set<std::int64_t> ledger;
    sqlite3_stmt* entries = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(raw.get(), "SELECT txn, rows, checksum FROM crash_ledger ORDER BY txn", -1, &entries,
                                 nullptr),
              SQLITE_OK);
    sqlite3_stmt* rows = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(raw.get(), "SELECT row, value FROM crash_data WHERE txn = ?1 ORDER BY row", -1, &rows,
                                 nullptr),
              SQLITE_OK);
    while (entries != nullptr && rows != nullptr && sqlite3_step(entries) == SQLITE_ROW) {
        const std::int64_t txn = sqlite3_column_int64(entries, 0);
        const auto count = static_cast<int>(sqlite3_column_int64(entries, 1));
        const std::int64_t checksum = sqlite3_column_int64(entries, 2);
        ledger.insert(txn);
        EXPECT_EQ(checksum, ledgerChecksum(seed, txn, count)) << "txn " << txn;
        sqlite3_bind_int64(rows, 1, txn);
        Xxh3Stream64 hash;
        int seen = 0;
        while (sqlite3_step(rows) == SQLITE_ROW) {
            EXPECT_EQ(sqlite3_column_int(rows, 0), seen) << "txn " << txn;
            const auto* bytes = static_cast<const char*>(sqlite3_column_blob(rows, 1));
            hash.update(std::string_view(bytes != nullptr ? bytes : "",
                                         static_cast<std::size_t>(sqlite3_column_bytes(rows, 1))));
            ++seen;
        }
        sqlite3_reset(rows);
        EXPECT_EQ(seen, count) << "txn " << txn << " is incomplete";
        EXPECT_EQ(static_cast<std::int64_t>(hash.digest()), checksum) << "txn " << txn << " has wrong values";
    }
    sqlite3_finalize(entries);
    sqlite3_finalize(rows);
    EXPECT_EQ(raw.queryInt("SELECT count(*) FROM crash_data WHERE txn NOT IN (SELECT txn FROM crash_ledger)"), 0)
        << "rows of an uncommitted transaction survived";
    for (const std::int64_t txn : printed) {
        EXPECT_TRUE(ledger.contains(txn)) << "reported commit " << txn << " was lost";
    }
    raw.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    EXPECT_EQ(fileSize(path + "-wal"), 0u) << "the WAL was not truncated";
    return ledger.empty() ? 0 : *ledger.rbegin();
}

// Sleeps a uniformly random 0..15 ms (the kill offset after a commit line).
inline void randomKillDelay(std::mt19937_64& rng) {
    std::this_thread::sleep_for(std::chrono::microseconds(rng() % 15'001));
}

/*
 * One txn-loop kill: runs the child on `path` until its `commits`-th commit line, waits 0-15 ms,
 * kills it and verifies. `printed` accumulates every commit line across kills.
 */
inline void killTxnLoopOnce(const std::string& path, const char* sync, std::uint64_t seed, std::mt19937_64& rng,
                            std::set<std::int64_t>& printed, const char* mode = "txn-loop") {
    const std::size_t commits = 1 + rng() % 20;
    std::vector<std::string> args = std::string_view(mode) == "checkpoint-loop"
                                        ? std::vector<std::string>{mode, path, std::to_string(seed)}
                                        : std::vector<std::string>{mode, path, "10", sync, std::to_string(seed)};
    auto child = StorageChild::start(std::move(args));
    ASSERT_NE(child, nullptr);
    ASSERT_TRUE(child->waitFor("commit ", commits).has_value());
    randomKillDelay(rng);
    child->kill();
    const std::set<std::int64_t> now = printedCommits(child->lines());
    printed.insert(now.begin(), now.end());
    verifyCrashStore(path, seed, printed);
}

/*
 * One journal-put-loop kill in a fresh `dir`: the recovered generation is the last printed one
 * or the next (none or 1 before the first print), its content is journalContent(seed, g) byte for
 * byte (with a verified checksum), and the journal reports a crash. Discards it afterwards.
 */
inline void killJournalPutOnce(const std::string& dir, std::size_t size, std::uint64_t seed, std::mt19937_64& rng) {
    const std::size_t puts = rng() % 6; // 0: kill right after the journal exists.
    auto child = StorageChild::start({"journal-put-loop", dir, std::to_string(size), std::to_string(seed)});
    ASSERT_NE(child, nullptr);
    if (puts == 0) {
        ASSERT_TRUE(child->waitFor("journal ").has_value());
    } else {
        ASSERT_TRUE(child->waitFor("put ", puts).has_value());
    }
    randomKillDelay(rng);
    child->kill();
    const std::int64_t last = std::max<std::int64_t>(child->lastNumber("put "), 0);
    auto scan = findRecoverableJournals(dir, std::chrono::seconds(5));
    ASSERT_TRUE(scan.has_value()) << scan.error().describe();
    EXPECT_TRUE(scan->unreadable.empty());
    EXPECT_EQ(scan->inUse, 0u);
    ASSERT_EQ(scan->journals.size(), 1u) << child->output();
    RecoveredJournal recovered = std::move(scan->journals.front());
    EXPECT_FALSE(recovered.endedCleanly());
    EXPECT_FALSE(recovered.damaged());
    auto entries = recovered.entries();
    ASSERT_TRUE(entries.has_value()) << entries.error().describe();
    if (entries->empty()) {
        EXPECT_EQ(last, 0) << "put " << last << " was reported but is gone";
    } else {
        ASSERT_EQ(entries->size(), 1u);
        const std::int64_t generation = (*entries)[0].state.generation;
        EXPECT_TRUE(generation == last || generation == last + 1) << generation << " after put " << last;
        auto content = recovered.readContent((*entries)[0].id);
        ASSERT_TRUE(content.has_value()) << content.error().describe();
        EXPECT_TRUE(*content == journalContent(seed, generation, size)) << "generation " << generation;
    }
    EXPECT_TRUE(recovered.database().integrityCheck().has_value());
    ASSERT_TRUE(std::move(recovered).discard().has_value());
}

/*
 * Checks a local-history store after a kill of history-record-loop: integrity, every revision's
 * content verifies (size, chunk count, XXH3-128), no orphan chunks or blobs, chunk counts match,
 * and the revision count is the number of printed records or one more.
 */
inline void verifyHistoryStore(const std::string& path, std::uint64_t printedTotal, std::uint64_t kills) {
    {
        RawDb raw(path);
        sqlite3_busy_timeout(raw.get(), 5000);
        EXPECT_EQ(raw.queryText("PRAGMA integrity_check"), "ok");
        EXPECT_EQ(raw.queryInt("SELECT count(*) FROM lh_chunk c WHERE NOT EXISTS "
                               "(SELECT 1 FROM lh_blob b WHERE b.id = c.blob_id)"),
                  0);
        EXPECT_EQ(raw.queryInt("SELECT count(*) FROM lh_blob b WHERE NOT EXISTS "
                               "(SELECT 1 FROM lh_revision r WHERE r.blob_id = b.id)"),
                  0);
        EXPECT_EQ(raw.queryInt("SELECT count(*) FROM lh_blob b WHERE chunk_count != "
                               "(SELECT count(*) FROM lh_chunk c WHERE c.blob_id = b.id)"),
                  0);
        const auto revisions = static_cast<std::uint64_t>(raw.queryInt("SELECT count(*) FROM lh_revision"));
        EXPECT_GE(revisions, printedTotal);
        EXPECT_LE(revisions, printedTotal + kills);
    }
    HistoryOptions options;
    options.busyTimeout = std::chrono::seconds(5);
    auto history = LocalHistory::open(path, systemClock(), options);
    ASSERT_TRUE(history.has_value()) << history.error().describe();
    auto ids = history->database().prepare("SELECT id FROM lh_revision");
    ASSERT_TRUE(ids.has_value());
    std::vector<std::int64_t> all;
    while (ids->step().value_or(false)) {
        all.push_back(ids->columnInt64(0));
    }
    ids = std::unexpected(Error(ErrorCode::Unknown));
    for (const std::int64_t id : all) {
        std::uint64_t bytes = 0;
        auto read = history->content(id, [&](std::string_view chunk) -> Result<void> {
            bytes += chunk.size();
            return {};
        });
        EXPECT_TRUE(read.has_value()) << "revision " << id << ": " << read.error().describe();
    }
}

} // namespace frost::storage::test
