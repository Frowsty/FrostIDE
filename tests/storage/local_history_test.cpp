// SPDX-License-Identifier: Apache-2.0
//
// LocalHistory: recording, dedup by content (with byte comparison against XXH3 collisions),
// streaming reads, and each retention rule, including the races a prune must survive (orphans of
// an interrupted prune, a blob reused between snapshot and delete) and clock oddities.

#include "frost/storage/local_history.hpp"

#include "support/scratch_dir.hpp"
#include "support/storage_test_support.hpp"
#include "test_hooks.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::ManualClock;
using frost::storage::HistoryOptions;
using frost::storage::LocalHistory;
using frost::storage::RetentionPolicy;
using frost::storage::RevisionKind;
using frost::storage::Synchronous;
using frost::storage::test::randomBytes;
using frost::test::ScratchDir;

using SystemTime = frost::IClock::SystemTime;
constexpr SystemTime kStart{std::chrono::seconds(1'700'000'000)};

HistoryOptions fastHistory() {
    HistoryOptions options;
    options.synchronous = Synchronous::Normal;
    return options;
}

class LocalHistoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto history = LocalHistory::open(m_dir.join("history.db"), m_clock, fastHistory());
        ASSERT_TRUE(history.has_value()) << history.error().describe();
        m_history = std::make_unique<LocalHistory>(std::move(*history));
    }

    frost::Result<frost::storage::RecordOutcome> record(std::string_view key, std::string_view content,
                                                        std::string_view label = {},
                                                        RevisionKind kind = RevisionKind::Save) {
        const std::string_view pieces[] = {content};
        return m_history->record(key, std::string("/work/") + std::string(key), kind, label, pieces);
    }

    // Records `content` and returns the new revision's id (a test failure if none was added).
    std::int64_t add(std::string_view key, std::string_view content, std::string_view label = {}) {
        auto outcome = record(key, content, label);
        EXPECT_TRUE(outcome.has_value()) << outcome.error().describe();
        if (!outcome) {
            return -1;
        }
        EXPECT_TRUE(outcome->added);
        return outcome->revision.id;
    }

    std::vector<std::int64_t> ids(std::string_view key) {
        std::vector<std::int64_t> out;
        auto revisions = m_history->revisions(key);
        EXPECT_TRUE(revisions.has_value());
        if (revisions) {
            for (const auto& revision : *revisions) {
                out.push_back(revision.id);
            }
        }
        return out;
    }

    frost::storage::HistoryStats stats() {
        auto value = m_history->stats();
        EXPECT_TRUE(value.has_value()) << value.error().describe();
        return value.value_or(frost::storage::HistoryStats{});
    }

    std::int64_t queryInt(std::string_view sql) {
        auto statement = m_history->database().prepare(sql);
        EXPECT_TRUE(statement.has_value()) << statement.error().describe();
        if (!statement || !statement->step().value_or(false)) {
            ADD_FAILURE() << "no row for " << sql;
            return -1;
        }
        return statement->columnInt64(0);
    }

    ScratchDir m_dir{"storage-history"};
    ManualClock m_clock{frost::IClock::SteadyTime{}, kStart};
    std::unique_ptr<LocalHistory> m_history;
};

TEST_F(LocalHistoryTest, RecordRevisionsContent) {
    const std::int64_t first = add("a.cpp", "one");
    m_clock.advance(1s);
    const std::int64_t second = add("a.cpp", "two", {});
    m_clock.advance(1s);
    auto labelled = record("a.cpp", "three", "before refactor", RevisionKind::Labelled);
    ASSERT_TRUE(labelled.has_value());
    EXPECT_GT(second, first);
    auto revisions = m_history->revisions("a.cpp");
    ASSERT_TRUE(revisions.has_value());
    ASSERT_EQ(revisions->size(), 3u);
    const auto& newest = (*revisions)[0];
    EXPECT_EQ(newest.id, labelled->revision.id);
    EXPECT_EQ(newest.pathKey, "a.cpp");
    EXPECT_EQ(newest.path, "/work/a.cpp");
    EXPECT_EQ(newest.kind, RevisionKind::Labelled);
    EXPECT_EQ(newest.label, "before refactor");
    EXPECT_EQ(newest.size, 5u);
    EXPECT_EQ(newest.createdAt, kStart + 2s);
    EXPECT_EQ(newest.fingerprint, frost::storage::xxh3Hash128("three"));
    EXPECT_EQ((*revisions)[2].id, first);
    EXPECT_EQ(m_history->content(first).value(), "one");
    EXPECT_EQ(m_history->content(second).value(), "two");
    // limit, and other keys are separate.
    EXPECT_EQ(m_history->revisions("a.cpp", 1).value().size(), 1u);
    EXPECT_TRUE(m_history->revisions("b.cpp").value().empty());
    auto missing = m_history->content(9999);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    // Bad arguments.
    EXPECT_EQ(record("", "x").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(record("k", "x", {}, static_cast<RevisionKind>(9)).error().code(), ErrorCode::InvalidArgument);
}

TEST_F(LocalHistoryTest, IdenticalConsecutiveSaveNotAdded) {
    const std::int64_t first = add("a.cpp", "same");
    auto again = record("a.cpp", "same");
    ASSERT_TRUE(again.has_value());
    EXPECT_FALSE(again->added);
    EXPECT_EQ(again->revision.id, first);
    // The same bytes in other pieces are the same content.
    const std::string_view pieces[] = {"sa", "", "me"};
    auto split = m_history->record("a.cpp", "/work/a.cpp", RevisionKind::Save, {}, pieces);
    ASSERT_TRUE(split.has_value());
    EXPECT_FALSE(split->added);
    // Not consecutive: A, B, A adds three revisions (sharing a blob).
    add("a.cpp", "other");
    add("a.cpp", "same");
    EXPECT_EQ(ids("a.cpp").size(), 3u);
    EXPECT_EQ(stats().blobs, 2u);
}

TEST_F(LocalHistoryTest, LabelForcesNewRevision) {
    add("a.cpp", "same");
    auto labelled = record("a.cpp", "same", "checkpoint", RevisionKind::Labelled);
    ASSERT_TRUE(labelled.has_value());
    EXPECT_TRUE(labelled->added);
    EXPECT_EQ(ids("a.cpp").size(), 2u);
    EXPECT_EQ(stats().blobs, 1u);
}

TEST_F(LocalHistoryTest, ContentSinkStreams) {
    const std::string large = randomBytes((std::size_t{5} << 20) + 3, 11); // Six 1 MiB chunks.
    const std::int64_t id = add("big.bin", large);
    std::vector<std::size_t> pieces;
    std::string received;
    auto read = m_history->content(id, [&](std::string_view bytes) -> frost::Result<void> {
        pieces.push_back(bytes.size());
        received.append(bytes);
        return {};
    });
    ASSERT_TRUE(read.has_value()) << read.error().describe();
    EXPECT_EQ(pieces.size(), 6u);
    EXPECT_EQ(pieces.front(), std::size_t{1} << 20);
    EXPECT_TRUE(received == large);
    // A sink error stops the read.
    std::size_t calls = 0;
    auto stopped = m_history->content(id, [&](std::string_view) -> frost::Result<void> {
        ++calls;
        return frost::fail(ErrorCode::Timeout, "sink full");
    });
    ASSERT_FALSE(stopped.has_value());
    EXPECT_EQ(stopped.error().code(), ErrorCode::Timeout);
    EXPECT_EQ(calls, 1u);
    // A stop request cancels.
    std::stop_source stop;
    stop.request_stop();
    EXPECT_EQ(m_history->content(
                           id, [](std::string_view) -> frost::Result<void> { return {}; }, stop.get_token())
                  .error()
                  .code(),
              ErrorCode::Cancelled);
}

TEST_F(LocalHistoryTest, DedupAcrossPaths) {
    const std::string content = randomBytes(300000, 3);
    add("a.cpp", content);
    add("b.cpp", content);
    add("c.cpp", content, "label");
    const auto value = stats();
    EXPECT_EQ(value.revisions, 3u);
    EXPECT_EQ(value.blobs, 1u);
    EXPECT_EQ(value.contentBytes, content.size());
}

// Rewrites blob `id`'s fingerprint, as an XXH3 collision would look.
void setFingerprint(LocalHistory& history, std::int64_t blobId, const frost::storage::Fingerprint128& fingerprint) {
    auto update = history.database().prepare("UPDATE lh_blob SET fingerprint = ?1 WHERE id = ?2");
    ASSERT_TRUE(update.has_value());
    FROST_STORAGE_ASSERT_OK(update->bindFingerprint(1, fingerprint));
    FROST_STORAGE_ASSERT_OK(update->bindInt64(2, blobId));
    FROST_STORAGE_ASSERT_OK(update->run());
}

TEST_F(LocalHistoryTest, FingerprintCollisionStoresSeparateBlob) {
    const std::string a = randomBytes(5000, 1);
    const std::string b = randomBytes(5000, 2);
    add("a.cpp", a);
    setFingerprint(*m_history, 1, frost::storage::xxh3Hash128(b)); // Blob 1 now claims b's key.
    const std::int64_t revisionB = add("b.cpp", b);
    EXPECT_EQ(stats().blobs, 2u);
    EXPECT_EQ(m_history->content(revisionB).value(), b);
    EXPECT_EQ(queryInt(std::format("SELECT blob_id FROM lh_revision WHERE id = {}", revisionB)), 2);
}

TEST_F(LocalHistoryTest, DedupComparesEveryCandidate) {
    const std::string a = randomBytes(5000, 1);
    const std::string b = randomBytes(5000, 2);
    const std::string c = randomBytes(5000, 3);
    add("a.cpp", a);
    add("b.cpp", b);
    setFingerprint(*m_history, 1, frost::storage::xxh3Hash128(c));
    setFingerprint(*m_history, 2, frost::storage::xxh3Hash128(c));
    const std::int64_t first = add("c.cpp", c); // Two false candidates: a new blob 3.
    EXPECT_EQ(queryInt(std::format("SELECT blob_id FROM lh_revision WHERE id = {}", first)), 3);
    const std::int64_t second = add("d.cpp", c); // The third candidate matches.
    EXPECT_EQ(queryInt(std::format("SELECT blob_id FROM lh_revision WHERE id = {}", second)), 3);
    EXPECT_EQ(stats().blobs, 3u);
    EXPECT_EQ(m_history->content(second).value(), c);
}

TEST_F(LocalHistoryTest, ByteExactCrlfBomContent) {
    const std::string content = std::string("\xEF\xBB\xBF") + "line one\r\nline two\r\n\r\n" + std::string(3, '\0') +
                                "\xFF\xFE tail without newline";
    const std::int64_t id = add("crlf.txt", content);
    EXPECT_TRUE(m_history->content(id).value() == content);
    // LF-only is different content, so it is a new revision.
    const std::int64_t lf = add("crlf.txt", "\xEF\xBB\xBFline one\nline two\n\n");
    EXPECT_GT(lf, id);
}

RetentionPolicy byAge(std::chrono::nanoseconds age, bool keepNewest) {
    return RetentionPolicy(age, std::numeric_limits<std::uint64_t>::max(), keepNewest);
}

TEST_F(LocalHistoryTest, RevisionIdsNeverReused) {
    add("a.cpp", "one");
    const std::int64_t highest = add("a.cpp", "two");
    m_clock.advance(1h);
    auto pruned = m_history->prune(byAge(1min, false));
    ASSERT_TRUE(pruned.has_value()) << pruned.error().describe();
    EXPECT_EQ(pruned->revisionsRemoved, 2u);
    EXPECT_EQ(stats().revisions, 0u);
    EXPECT_GT(add("a.cpp", "three"), highest);
    // Blob ids are not reused either.
    EXPECT_EQ(queryInt("SELECT max(id) FROM lh_blob"), 3);
}

TEST_F(LocalHistoryTest, PruneByAge) {
    const std::int64_t old1 = add("a.cpp", "a1");
    const std::int64_t old2 = add("b.cpp", "b1");
    m_clock.advance(10min);
    const std::int64_t mid = add("a.cpp", "a2");
    m_clock.advance(10min);
    const std::int64_t fresh = add("a.cpp", "a3");
    // now = start + 20 min; cutoff = start + 15 min with maxAge 5 min.
    auto pruned = m_history->prune(byAge(5min, false));
    ASSERT_TRUE(pruned.has_value()) << pruned.error().describe();
    EXPECT_EQ(pruned->revisionsRemoved, 3u);
    EXPECT_EQ(pruned->blobsRemoved, 3u);
    EXPECT_EQ(pruned->contentBytesFreed, 6u);
    EXPECT_EQ(ids("a.cpp"), std::vector<std::int64_t>{fresh});
    EXPECT_TRUE(ids("b.cpp").empty());
    static_cast<void>(old1);
    static_cast<void>(old2);
    static_cast<void>(mid);
    // Exactly at the cutoff is kept (createdAt < now - maxAge goes).
    m_clock.advance(5min);
    auto boundary = m_history->prune(byAge(5min, false));
    ASSERT_TRUE(boundary.has_value());
    EXPECT_EQ(boundary->revisionsRemoved, 0u);
    m_clock.advance(1ns);
    EXPECT_EQ(m_history->prune(byAge(5min, false)).value().revisionsRemoved, 1u);
}

TEST_F(LocalHistoryTest, PruneBySize) {
    std::vector<std::int64_t> added;
    for (int i = 0; i < 10; ++i) {
        added.push_back(add(std::format("f{}.cpp", i % 3), randomBytes(1000, static_cast<std::uint64_t>(i))));
        m_clock.advance(1s);
    }
    // 10,000 referenced bytes; keep at most 4,500: the six oldest go, oldest first.
    auto pruned = m_history->prune(RetentionPolicy(24h * 365, 4500, false));
    ASSERT_TRUE(pruned.has_value()) << pruned.error().describe();
    EXPECT_EQ(pruned->revisionsRemoved, 6u);
    EXPECT_EQ(pruned->contentBytesFreed, 6000u);
    const auto value = stats();
    EXPECT_EQ(value.revisions, 4u);
    EXPECT_EQ(value.contentBytes, 4000u);
    for (int i = 6; i < 10; ++i) {
        EXPECT_TRUE(m_history->content(added[static_cast<std::size_t>(i)]).has_value()) << i;
    }
}

TEST_F(LocalHistoryTest, KeepsNewestPerPath) {
    const std::int64_t a1 = add("a.cpp", randomBytes(1000, 1));
    const std::int64_t b1 = add("b.cpp", randomBytes(1000, 2));
    const std::int64_t a2 = add("a.cpp", randomBytes(1000, 3));
    m_clock.advance(24h);
    // Everything is old, and the size limit is tiny: only the newest of each path survives.
    auto pruned = m_history->prune(RetentionPolicy(1h, 1, true));
    ASSERT_TRUE(pruned.has_value());
    EXPECT_EQ(pruned->revisionsRemoved, 1u);
    EXPECT_EQ(ids("a.cpp"), std::vector<std::int64_t>{a2});
    EXPECT_EQ(ids("b.cpp"), std::vector<std::int64_t>{b1});
    static_cast<void>(a1);
}

TEST_F(LocalHistoryTest, SharedBlobSurvivesWhileReferenced) {
    const std::string shared = randomBytes(2000, 7);
    add("a.cpp", shared);
    m_clock.advance(2h);
    const std::int64_t keep = add("b.cpp", shared);
    auto pruned = m_history->prune(byAge(1h, false));
    ASSERT_TRUE(pruned.has_value());
    EXPECT_EQ(pruned->revisionsRemoved, 1u);
    EXPECT_EQ(pruned->blobsRemoved, 0u);
    EXPECT_EQ(m_history->content(keep).value(), shared);
    // Size counts the shared blob once: 2000 bytes fit in 2000.
    add("c.cpp", shared);
    EXPECT_EQ(m_history->prune(RetentionPolicy(24h, 2000, false)).value().revisionsRemoved, 0u);
    m_clock.advance(3h);
    auto all = m_history->prune(byAge(1h, false));
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->revisionsRemoved, 2u);
    EXPECT_EQ(all->blobsRemoved, 1u);
    EXPECT_EQ(all->contentBytesFreed, 2000u);
}

TEST_F(LocalHistoryTest, PruneShrinksFile) {
    for (int i = 0; i < 40; ++i) {
        add(std::format("f{}.bin", i), randomBytes(256 * 1024, static_cast<std::uint64_t>(i)));
    }
    m_clock.advance(2h);
    const std::int64_t kept = add("kept.bin", "small");
    FROST_STORAGE_ASSERT_OK(m_history->database().checkpoint(frost::storage::CheckpointMode::Truncate));
    const auto before = stats();
    auto pruned = m_history->prune(byAge(1h, false));
    ASSERT_TRUE(pruned.has_value()) << pruned.error().describe();
    EXPECT_EQ(pruned->revisionsRemoved, 40u);
    EXPECT_EQ(pruned->contentBytesFreed, 40u * 256 * 1024);
    const auto after = stats();
    const std::int64_t pageSize = queryInt("PRAGMA page_size");
    const std::uint64_t freedPages = pruned->contentBytesFreed / static_cast<std::uint64_t>(pageSize);
    ASSERT_GT(before.fileBytes, after.fileBytes);
    EXPECT_GE((before.fileBytes - after.fileBytes) / static_cast<std::uint64_t>(pageSize), freedPages / 2);
    EXPECT_GE(pruned->pagesReleased, freedPages / 2);
    EXPECT_EQ(queryInt("PRAGMA freelist_count"), 0);
    EXPECT_EQ(m_history->content(kept).value(), "small");
    // The Truncate checkpoint emptied the WAL.
    EXPECT_EQ(frost::storage::test::fileSize(m_dir.join("history.db-wal")), 0u);
}

TEST_F(LocalHistoryTest, PruneCollectsOrphansFromInterruptedPrune) {
    add("a.cpp", randomBytes(3000, 1));
    add("b.cpp", randomBytes(3000, 2));
    const std::int64_t kept = add("c.cpp", randomBytes(3000, 3));
    // A prune that deleted revisions but died before deleting their blobs.
    FROST_STORAGE_ASSERT_OK(
        m_history->database().execute("DELETE FROM lh_revision WHERE path_key IN ('a.cpp', 'b.cpp')"));
    EXPECT_EQ(stats().blobs, 3u);
    auto pruned = m_history->prune(byAge(24h, false));
    ASSERT_TRUE(pruned.has_value());
    EXPECT_EQ(pruned->revisionsRemoved, 0u);
    EXPECT_EQ(pruned->blobsRemoved, 2u);
    EXPECT_EQ(pruned->contentBytesFreed, 6000u);
    EXPECT_EQ(queryInt("SELECT count(*) FROM lh_chunk"), 1); // Chunks cascade with their blobs.
    EXPECT_TRUE(m_history->content(kept).has_value());
}

TEST_F(LocalHistoryTest, PruneSkipsBlobReusedAfterSnapshot) {
    const std::string reused = randomBytes(4000, 9);
    add("a.cpp", reused);
    m_clock.advance(2h);
    add("a.cpp", "newer");
    auto second = LocalHistory::open(m_dir.join("history.db"), m_clock, fastHistory());
    ASSERT_TRUE(second.has_value()) << second.error().describe();
    struct Race {
        LocalHistory* other;
        const std::string* content;
        frost::Result<frost::storage::RecordOutcome> outcome = frost::fail(ErrorCode::Unknown, "hook did not run");
    } race{&*second, &reused};
    frost::storage::detail::g_pruneAfterSnapshot.install(
        [](void* context) {
            auto* r = static_cast<Race*>(context);
            const std::string_view pieces[] = {*r->content};
            r->outcome = r->other->record("b.cpp", "/work/b.cpp", RevisionKind::Save, {}, pieces);
        },
        &race);
    // The plan removes a.cpp's old revision and, as an orphan, its blob; b.cpp reuses the blob first.
    auto pruned = m_history->prune(byAge(1h, false));
    frost::storage::detail::g_pruneAfterSnapshot.clear();
    ASSERT_TRUE(pruned.has_value()) << pruned.error().describe();
    ASSERT_TRUE(race.outcome.has_value()) << race.outcome.error().describe();
    EXPECT_EQ(pruned->revisionsRemoved, 1u);
    EXPECT_EQ(pruned->blobsRemoved, 0u);
    // The revision recorded after the snapshot was not removed by age (it was never seen).
    auto revisions = m_history->revisions("b.cpp");
    ASSERT_TRUE(revisions.has_value());
    ASSERT_EQ(revisions->size(), 1u);
    EXPECT_EQ(m_history->content((*revisions)[0].id).value(), reused);
    EXPECT_EQ(queryInt("SELECT count(*) FROM lh_blob"), 2);
}

TEST_F(LocalHistoryTest, RetentionPolicyRejectsZero) {
    add("a.cpp", "x");
    m_clock.advance(24h);
    for (const RetentionPolicy& policy :
         {RetentionPolicy(0ns, 100, false), RetentionPolicy(1ns, 0, false), RetentionPolicy(-1h, 100, false)}) {
        auto pruned = m_history->prune(policy);
        ASSERT_FALSE(pruned.has_value());
        EXPECT_EQ(pruned.error().code(), ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(stats().revisions, 1u);
}

TEST_F(LocalHistoryTest, ClockBackwardsAndFutureRevisions) {
    m_clock.setSystemTime(kStart + 1000s);
    const std::int64_t later = add("a.cpp", "recorded at 1000 s");
    m_clock.setSystemTime(kStart + 500s); // The wall clock jumped back.
    const std::int64_t highest = add("a.cpp", "recorded at 500 s");
    // revisions() orders by time, so the highest id comes second.
    EXPECT_EQ(ids("a.cpp"), (std::vector<std::int64_t>{later, highest}));
    // Dedup compares with the highest id, not the latest time.
    auto again = record("a.cpp", "recorded at 500 s");
    ASSERT_TRUE(again.has_value());
    EXPECT_FALSE(again->added);
    EXPECT_EQ(again->revision.id, highest);
    // now = 1100 s, cutoff 800 s: the highest id is old but protected; the later one is young.
    m_clock.setSystemTime(kStart + 1100s);
    EXPECT_EQ(m_history->prune(byAge(300s, true)).value().revisionsRemoved, 0u);
    EXPECT_EQ(m_history->prune(byAge(300s, false)).value().revisionsRemoved, 1u);
    EXPECT_EQ(ids("a.cpp"), std::vector<std::int64_t>{later});
    // A revision dated in the future is never removed by age.
    m_clock.setSystemTime(kStart + 100'000s);
    const std::int64_t future = add("b.cpp", "from the future");
    m_clock.setSystemTime(kStart + 2000s);
    EXPECT_EQ(m_history->prune(byAge(1ns, false)).value().revisionsRemoved, 1u); // a.cpp's 1000 s revision.
    EXPECT_EQ(ids("b.cpp"), std::vector<std::int64_t>{future});
    // The cutoff saturates instead of overflowing (now before 1970, maxAge at its maximum).
    m_clock.setSystemTime(SystemTime{} - 1h);
    EXPECT_TRUE(m_history->prune(RetentionPolicy(frost::IClock::Duration::max(), 1, true)).has_value());
}

TEST_F(LocalHistoryTest, EmptyStorePrune) {
    auto pruned = m_history->prune(byAge(1ns, false));
    ASSERT_TRUE(pruned.has_value()) << pruned.error().describe();
    EXPECT_EQ(pruned->revisionsRemoved, 0u);
    EXPECT_EQ(pruned->blobsRemoved, 0u);
    EXPECT_EQ(stats().revisions, 0u);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_EQ(m_history->prune(byAge(1ns, false), stop.get_token()).error().code(), ErrorCode::Cancelled);
}

TEST_F(LocalHistoryTest, OpenInMemoryWorks) {
    auto memory = LocalHistory::openInMemory(m_clock);
    ASSERT_TRUE(memory.has_value()) << memory.error().describe();
    const std::string_view pieces[] = {"memory"};
    auto recorded = memory->record("k", "/k", RevisionKind::Save, {}, pieces);
    ASSERT_TRUE(recorded.has_value());
    EXPECT_EQ(memory->content(recorded->revision.id).value(), "memory");
    m_clock.advance(1h);
    EXPECT_EQ(memory->prune(byAge(1min, false)).value().revisionsRemoved, 1u);
}

} // namespace
