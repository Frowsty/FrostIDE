// SPDX-License-Identifier: Apache-2.0
//
// storage_stress_tests (label "stress"; tools/verify -Full runs them, Debug verification skips
// the label): the crash scenarios with 200 kills each (txn-loop in both synchronous modes and
// journal puts), retention with 2,000 seeds, the bank workload for 10 s, and a corruption sweep
// of 300 seeded byte flips, zeroed pages and truncations of a real journal and history file.
// FROST_STRESS_FULL=1 raises the kills to 1,000 and the seeds to 10,000 by adding shards: the
// test preset's 300 s limit applies to each case, so every shard stays at about 120 s or less.

#include "frost/storage/hot_exit_journal.hpp"
#include "frost/storage/local_history.hpp"

#include "support/bank.hpp"
#include "support/child_process.hpp"
#include "support/crash_check.hpp"
#include "support/retention_run.hpp"
#include "support/scratch_dir.hpp"
#include "support/storage_test_support.hpp"
#include <gtest/gtest.h>

#include <format>
#include <map>
#include <random>
#include <set>
#include <string>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::storage::test::stressFull;
using frost::test::ScratchDir;

constexpr int kKillsPerShard = 50;

// Shards of kKillsPerShard kills: 200 kills (4 shards), or 1,000 (20) under FROST_STRESS_FULL.
int killShards() { return stressFull() ? 20 : 4; }

class TxnKillShard : public ::testing::TestWithParam<std::tuple<const char*, int>> {};

TEST_P(TxnKillShard, AtomicAndDurable) {
    const auto [sync, shard] = GetParam();
    ScratchDir dir("storage-stress");
    const std::string path = dir.join("crash.db");
    std::mt19937_64 rng(static_cast<std::uint64_t>(shard) * 7919 + (std::string_view(sync) == "full" ? 1 : 0));
    std::set<std::int64_t> printed;
    for (int kill = 0; kill < kKillsPerShard; ++kill) {
        SCOPED_TRACE(std::format("shard {} kill {}", shard, kill));
        frost::storage::test::killTxnLoopOnce(path, sync, 0x5EED + static_cast<std::uint64_t>(shard), rng, printed);
        if (HasFatalFailure() || HasFailure()) {
            return;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Kills, TxnKillShard,
                         ::testing::Combine(::testing::Values("normal", "full"), ::testing::Range(0, killShards())),
                         [](const ::testing::TestParamInfo<std::tuple<const char*, int>>& info) {
                             return std::format("{}_{}", std::get<0>(info.param), std::get<1>(info.param));
                         });

class JournalKillShard : public ::testing::TestWithParam<int> {};

TEST_P(JournalKillShard, OldOrNew) {
    const int shard = GetParam();
    ScratchDir dir("storage-stress");
    std::mt19937_64 rng(static_cast<std::uint64_t>(shard) * 104729);
    for (int kill = 0; kill < kKillsPerShard; ++kill) {
        SCOPED_TRACE(std::format("shard {} kill {}", shard, kill));
        frost::storage::test::killJournalPutOnce(dir.join(std::format("k{}", kill)), 64 * 1024,
                                                 static_cast<std::uint64_t>(shard * 1000 + kill), rng);
        if (HasFatalFailure() || HasFailure()) {
            return;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Kills, JournalKillShard, ::testing::Range(0, killShards()));

// Retention: 2,000 seeds in 8 shards of 250, or 10,000 in 40 under FROST_STRESS_FULL.
constexpr int kSeedsPerShard = 250;
int retentionShards() { return stressFull() ? 40 : 8; }

class RetentionShard : public ::testing::TestWithParam<int> {};

TEST_P(RetentionShard, MatchesOracle) {
    const int shard = GetParam();
    for (int i = 0; i < kSeedsPerShard; ++i) {
        const auto seed = static_cast<std::uint64_t>(100'000 + shard * kSeedsPerShard + i);
        frost::ManualClock clock(frost::IClock::SteadyTime{},
                                 frost::IClock::SystemTime(std::chrono::seconds(1'700'000'000)));
        auto history = frost::storage::LocalHistory::openInMemory(clock);
        ASSERT_TRUE(history.has_value()) << history.error().describe();
        const std::string failure = frost::storage::test::runRetentionSeed(seed, 200, *history, clock);
        ASSERT_TRUE(failure.empty()) << failure;
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, RetentionShard, ::testing::Range(0, retentionShards()));

TEST(StorageStress, BankForTenSeconds) {
    ScratchDir dir("storage-stress");
    frost::storage::test::Failures failures;
    const std::uint64_t reads = frost::storage::test::runBank(dir.join("bank.db"), 0, 10s, 4, failures);
    failures.report();
    EXPECT_EQ(failures.count(), 0u);
    EXPECT_GT(reads, 0u);
}

/*
 * Corruption sweep (the seed of the later libFuzzer target): 300 mutations, 100 per shard, of a
 * pristine journal and a pristine history file. Open, scan and read must never crash or hang;
 * they succeed or fail with Corrupt, IoError or Unsupported (a damaged version number reads as a
 * newer FrostIDE's file), and content they return passed its checksum and equals the original.
 */
struct Pristine {
    std::string journalBytes;
    std::string historyBytes;
    std::map<std::string, std::string> journalContent; // documentKey -> content
    std::map<std::string, std::string> historyContent; // path_key + "#" + n -> content
};

const Pristine& pristine() {
    static const Pristine files = [] {
        Pristine out;
        ScratchDir dir("storage-pristine");
        {
            frost::storage::JournalOptions options;
            options.chunkBytes = 4096;
            options.synchronous = frost::storage::Synchronous::Normal;
            auto journal = frost::storage::HotExitJournal::create(dir.path(), frost::systemClock(), options);
            EXPECT_TRUE(journal.has_value());
            for (int i = 0; i < 3; ++i) {
                frost::storage::DocumentState state;
                state.documentKey = std::format("doc{}", i);
                state.generation = 1;
                const std::string content = frost::storage::test::randomBytes(
                    static_cast<std::size_t>(1000 + i * 30000), static_cast<std::uint64_t>(i));
                const std::string_view pieces[] = {content};
                EXPECT_TRUE(journal->put(state, pieces).has_value());
                out.journalContent[state.documentKey] = content;
            }
            const std::string path = journal->path();
            EXPECT_TRUE(journal->closeCleanly().has_value());
            out.journalBytes = frost::storage::test::readBytes(path);
        }
        {
            const std::string path = dir.join("history.db");
            frost::storage::HistoryOptions options;
            options.synchronous = frost::storage::Synchronous::Normal;
            {
                auto history = frost::storage::LocalHistory::open(path, frost::systemClock(), options);
                EXPECT_TRUE(history.has_value());
                for (int i = 0; i < 10; ++i) {
                    const std::string content = frost::storage::test::randomBytes(static_cast<std::size_t>(i * 7000),
                                                                                  static_cast<std::uint64_t>(100 + i));
                    const std::string_view pieces[] = {content};
                    auto recorded = history->record(std::format("f{}", i % 3), "/f", frost::storage::RevisionKind::Save,
                                                    {}, pieces);
                    EXPECT_TRUE(recorded.has_value());
                    if (recorded) {
                        out.historyContent[std::to_string(recorded->revision.id)] = content;
                    }
                }
            }
            out.historyBytes = frost::storage::test::readBytes(path);
        }
        return out;
    }();
    return files;
}

std::string mutate(std::string bytes, std::mt19937_64& rng, std::string& what) {
    if (bytes.empty()) {
        return bytes;
    }
    switch (rng() % 3) {
    case 0: {
        const std::size_t flips = 1 + rng() % 8;
        for (std::size_t i = 0; i < flips; ++i) {
            const std::size_t at = rng() % bytes.size();
            bytes[at] = static_cast<char>(bytes[at] ^ static_cast<char>(1 + rng() % 255));
        }
        what = std::format("{} byte flips", flips);
        break;
    }
    case 1: {
        const std::size_t pages = bytes.size() / 4096;
        const std::size_t page = pages > 0 ? rng() % pages : 0;
        std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(page * 4096), std::min<std::size_t>(4096, bytes.size()),
                    '\0');
        what = std::format("page {} zeroed", page + 1);
        break;
    }
    default: {
        const std::size_t length = rng() % bytes.size();
        bytes.resize(length);
        what = std::format("truncated to {}", length);
        break;
    }
    }
    return bytes;
}

bool allowedFailure(const frost::Error& error) {
    return error.code() == ErrorCode::Corrupt || error.code() == ErrorCode::IoError ||
           error.code() == ErrorCode::Unsupported;
}

void sweepJournal(const std::string& dir, const std::string& bytes) {
    frost::storage::test::writeBytes(dir + "/journal-20261101T120000000Z-00000000000000aa.db", bytes);
    auto scan = frost::storage::findRecoverableJournals(dir, 5s);
    ASSERT_TRUE(scan.has_value()) << scan.error().describe();
    for (const auto& skipped : scan->skipped) {
        EXPECT_TRUE(allowedFailure(skipped.reason)) << skipped.reason.describe();
    }
    for (frost::storage::RecoveredJournal& journal : scan->journals) {
        auto entries = journal.entries();
        if (!entries) {
            EXPECT_TRUE(allowedFailure(entries.error())) << entries.error().describe();
        } else {
            for (const auto& entry : *entries) {
                auto content = journal.readContent(entry.id);
                if (!content) {
                    EXPECT_TRUE(allowedFailure(content.error())) << content.error().describe();
                    continue;
                }
                const auto original = pristine().journalContent.find(entry.state.documentKey);
                ASSERT_NE(original, pristine().journalContent.end()) << entry.state.documentKey;
                EXPECT_TRUE(*content == original->second) << "verified content differs for " << entry.state.documentKey;
            }
        }
        static_cast<void>(std::move(journal).discard());
    }
}

void sweepHistory(const std::string& path, const std::string& bytes) {
    frost::storage::test::writeBytes(path, bytes);
    for (const frost::storage::Verify verify : {frost::storage::Verify::Header, frost::storage::Verify::QuickCheck}) {
        frost::storage::HistoryOptions options;
        options.verify = verify;
        options.synchronous = frost::storage::Synchronous::Normal;
        auto history = frost::storage::LocalHistory::open(path, frost::systemClock(), options);
        if (!history) {
            EXPECT_TRUE(allowedFailure(history.error())) << history.error().describe();
            continue;
        }
        auto ids = history->database().prepare("SELECT id FROM lh_revision");
        if (!ids) {
            EXPECT_TRUE(allowedFailure(ids.error())) << ids.error().describe();
            continue;
        }
        std::vector<std::int64_t> all;
        while (true) {
            auto row = ids->step();
            if (!row) {
                EXPECT_TRUE(allowedFailure(row.error())) << row.error().describe();
                break;
            }
            if (!*row) {
                break;
            }
            all.push_back(ids->columnInt64(0));
        }
        ids = std::unexpected(frost::Error(ErrorCode::Unknown));
        for (const std::int64_t id : all) {
            auto content = history->content(id);
            if (!content) {
                EXPECT_TRUE(allowedFailure(content.error()) || content.error().code() == ErrorCode::NotFound)
                    << content.error().describe();
                continue;
            }
            const auto original = pristine().historyContent.find(std::to_string(id));
            if (original != pristine().historyContent.end()) {
                EXPECT_TRUE(*content == original->second) << "verified content differs for revision " << id;
            }
        }
        auto pruned = history->prune(frost::storage::RetentionPolicy(1h, 1, true));
        if (!pruned) {
            EXPECT_TRUE(allowedFailure(pruned.error())) << pruned.error().describe();
        }
    }
}

class CorruptionShard : public ::testing::TestWithParam<int> {};

TEST_P(CorruptionShard, NeverCrashesOrReturnsUnverifiedContent) {
    const int shard = GetParam();
    ASSERT_FALSE(pristine().journalBytes.empty());
    ASSERT_FALSE(pristine().historyBytes.empty());
    ScratchDir dir("storage-sweep");
    for (int i = 0; i < 100; ++i) {
        std::mt19937_64 rng(static_cast<std::uint64_t>(shard * 100 + i));
        const bool journal = rng() % 2 == 0;
        std::string what;
        const std::string bytes = mutate(journal ? pristine().journalBytes : pristine().historyBytes, rng, what);
        SCOPED_TRACE(std::format("case {}: {} of the {}", shard * 100 + i, what, journal ? "journal" : "history"));
        const std::string caseDir = dir.join(std::format("c{}", i));
        ASSERT_TRUE(frost::io::realFileSystem().createDirectories(caseDir).has_value());
        if (journal) {
            sweepJournal(caseDir, bytes);
        } else {
            sweepHistory(caseDir + "/history.db", bytes);
        }
        if (HasFatalFailure()) {
            return;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Sweep, CorruptionShard, ::testing::Range(0, 3));

} // namespace
