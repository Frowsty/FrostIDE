// SPDX-License-Identifier: Apache-2.0
//
// HotExitJournal and the recovery scan in one process: round trips at chunk boundaries, stale
// generations, clean marks, cancellation, clean and crashed shutdowns and their bounded removal,
// damaged chunks and salvage, ownership by lock, paths too long to quarantine, and which files
// the scan matches, removes, quarantines or skips.

#include "frost/io/path.hpp"
#include "frost/storage/hot_exit_journal.hpp"

#include "database_files.hpp"
#include "support/hook_guard.hpp"
#include "support/scratch_dir.hpp"
#include "support/storage_test_support.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

namespace {

using frost::ErrorCode;
using frost::storage::DiskBase;
using frost::storage::DocumentState;
using frost::storage::findRecoverableJournals;
using frost::storage::Fingerprint128;
using frost::storage::HotExitJournal;
using frost::storage::JournalOptions;
using frost::storage::PutOutcome;
using frost::storage::RecoveredJournal;
using frost::storage::Synchronous;
using frost::storage::test::fileExists;
using frost::storage::test::HookGuard;
using frost::storage::test::randomBytes;
using frost::storage::test::readBytes;
using frost::storage::test::writeBytes;
using frost::test::ScratchDir;

constexpr auto kLockWait = std::chrono::seconds(5);

JournalOptions fastJournal(std::size_t chunkBytes = std::size_t{1} << 20) {
    JournalOptions options;
    options.synchronous = Synchronous::Normal;
    options.chunkBytes = chunkBytes;
    return options;
}

DocumentState document(std::string key, std::int64_t generation) {
    DocumentState state;
    state.documentKey = std::move(key);
    state.path = "/work/" + state.documentKey;
    state.generation = generation;
    state.encoding = "utf-8";
    state.lineEnding = "lf";
    state.base =
        DiskBase{123, frost::IClock::SystemTime(std::chrono::seconds(1700000000)), Fingerprint128{0x1111, 0x2222}};
    return state;
}

frost::Result<PutOutcome> put(HotExitJournal& journal, const DocumentState& state, std::string_view content) {
    const std::string_view pieces[] = {content};
    return journal.put(state, pieces);
}

// Scans `dir` and expects exactly one recoverable journal.
RecoveredJournal recoverOne(const std::string& dir) {
    auto scan = findRecoverableJournals(dir, kLockWait);
    EXPECT_TRUE(scan.has_value()) << scan.error().describe();
    EXPECT_EQ(scan->journals.size(), 1u);
    EXPECT_EQ(scan->inUse, 0u);
    EXPECT_TRUE(scan->unreadable.empty());
    EXPECT_TRUE(scan->skipped.empty());
    return std::move(scan->journals.front());
}

TEST(HotExitJournal, PutReadRoundTrip) {
    ScratchDir dir("storage-journal");
    std::string path;
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
        ASSERT_TRUE(journal.has_value()) << journal.error().describe();
        path = journal->path();
        EXPECT_TRUE(frost::io::fileName(path).starts_with("journal-"));
        auto written = put(*journal, document("a.cpp", 1), "int main() {}\n");
        ASSERT_TRUE(written.has_value()) << written.error().describe();
        EXPECT_EQ(*written, PutOutcome::Written);
        auto entries = journal->entries();
        ASSERT_TRUE(entries.has_value());
        ASSERT_EQ(entries->size(), 1u);
        EXPECT_EQ((*entries)[0].contentSize, 14u);
        EXPECT_EQ((*entries)[0].contentFingerprint, frost::storage::xxh3Hash128("int main() {}\n"));
    } // Destroyed without closeCleanly: a crash.
    RecoveredJournal recovered = recoverOne(dir.path());
    EXPECT_EQ(recovered.path(), path);
    EXPECT_FALSE(recovered.endedCleanly());
    EXPECT_FALSE(recovered.damaged());
    auto entries = recovered.entries();
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 1u);
    const auto& entry = (*entries)[0];
    const DocumentState expected = document("a.cpp", 1);
    EXPECT_EQ(entry.state.documentKey, expected.documentKey);
    EXPECT_EQ(entry.state.path, expected.path);
    EXPECT_EQ(entry.state.generation, 1);
    EXPECT_EQ(entry.state.encoding, "utf-8");
    EXPECT_EQ(entry.state.lineEnding, "lf");
    ASSERT_TRUE(entry.state.base.has_value());
    EXPECT_EQ(entry.state.base->size, 123u);
    EXPECT_EQ(entry.state.base->mtime, expected.base->mtime);
    EXPECT_EQ(entry.state.base->fingerprint, expected.base->fingerprint);
    auto content = recovered.readContent(entry.id);
    ASSERT_TRUE(content.has_value()) << content.error().describe();
    EXPECT_EQ(*content, "int main() {}\n");
    auto missing = recovered.readContent(entry.id + 1000);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    ASSERT_TRUE(std::move(recovered).discard().has_value());
    EXPECT_FALSE(fileExists(path));
    EXPECT_FALSE(fileExists(path + "-wal"));
}

TEST(HotExitJournal, ChunkBoundaries) {
    ScratchDir dir("storage-journal");
    const std::size_t sizes[] = {0, 1, 4095, 4096, 4097, (std::size_t{1} << 20) + 17};
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal(4096));
        ASSERT_TRUE(journal.has_value());
        // No single BLOB could hold the larger contents; only chunking makes them fit.
        sqlite3_limit(journal->database().nativeHandle(), SQLITE_LIMIT_LENGTH, 8192);
        for (const std::size_t size : sizes) {
            const std::string content = randomBytes(size, size);
            // Split oddly, so pieces straddle chunk boundaries.
            const std::size_t cut = size / 3;
            const std::string_view pieces[] = {std::string_view(content).substr(0, cut),
                                               std::string_view(content).substr(cut)};
            auto written = journal->put(document(std::format("doc{}", size), 1), pieces);
            ASSERT_TRUE(written.has_value()) << size << ": " << written.error().describe();
        }
    }
    RecoveredJournal recovered = recoverOne(dir.path());
    auto entries = recovered.entries();
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), std::size(sizes));
    for (const auto& entry : *entries) {
        const std::size_t size = std::stoull(entry.state.documentKey.substr(3));
        EXPECT_EQ(entry.contentSize, size);
        auto content = recovered.readContent(entry.id);
        ASSERT_TRUE(content.has_value()) << content.error().describe();
        EXPECT_TRUE(*content == randomBytes(size, size)) << size;
        // Whole chunks of 4096 bytes and one short tail; no chunk at all for empty content.
        auto chunks = recovered.database().prepare(std::format(
            "SELECT count(*), coalesce(max(length(data)), 0) FROM journal_chunk WHERE entry_id = {}", entry.id));
        ASSERT_TRUE(chunks.has_value() && chunks->step().value_or(false));
        EXPECT_EQ(chunks->columnInt64(0), static_cast<std::int64_t>((size + 4095) / 4096)) << size;
        EXPECT_LE(chunks->columnInt64(1), 4096) << size;
    }
}

TEST(HotExitJournal, TenThousandOneBytePieces) {
    ScratchDir dir("storage-journal");
    std::vector<std::string_view> pieces;
    const std::string bytes = randomBytes(10000, 77);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        pieces.emplace_back(bytes.data() + i, 1);
    }
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal(4096));
        ASSERT_TRUE(journal.has_value());
        ASSERT_TRUE(journal->put(document("pieces", 1), pieces).has_value());
    }
    RecoveredJournal recovered = recoverOne(dir.path());
    auto entries = recovered.entries();
    ASSERT_TRUE(entries.has_value() && entries->size() == 1);
    auto content = recovered.readContent((*entries)[0].id);
    ASSERT_TRUE(content.has_value());
    EXPECT_TRUE(*content == bytes);
}

TEST(HotExitJournal, BinaryContentByteExact) {
    ScratchDir dir("storage-journal");
    std::string binary;
    for (int i = 0; i < 1024; ++i) {
        binary.push_back(static_cast<char>(i & 0xFF));
    }
    binary += std::string_view("\r\n\r\n\xEF\xBB\xBF\0\0\xFF", 10);
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
        ASSERT_TRUE(journal.has_value());
        DocumentState state = document("bin", 1);
        state.hasBom = true;
        state.lineEnding = "crlf";
        state.encoding = "utf-16le";
        ASSERT_TRUE(put(*journal, state, binary).has_value());
    }
    RecoveredJournal recovered = recoverOne(dir.path());
    auto entries = recovered.entries();
    ASSERT_TRUE(entries.has_value() && entries->size() == 1);
    EXPECT_TRUE((*entries)[0].state.hasBom);
    EXPECT_EQ((*entries)[0].state.lineEnding, "crlf");
    EXPECT_EQ((*entries)[0].state.encoding, "utf-16le");
    auto content = recovered.readContent((*entries)[0].id);
    ASSERT_TRUE(content.has_value());
    EXPECT_TRUE(*content == binary);
}

TEST(HotExitJournal, StaleAndEqualGenerationAreNoOps) {
    ScratchDir dir("storage-journal");
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
        ASSERT_TRUE(journal.has_value());
        ASSERT_EQ(put(*journal, document("k", 5), "five").value(), PutOutcome::Written);
        EXPECT_EQ(put(*journal, document("k", 5), "five again").value(), PutOutcome::Stale);
        EXPECT_EQ(put(*journal, document("k", 4), "four").value(), PutOutcome::Stale);
        EXPECT_EQ(put(*journal, document("k", 6), "six").value(), PutOutcome::Written);
        auto negative = put(*journal, document("k", -1), "x");
        ASSERT_FALSE(negative.has_value());
        EXPECT_EQ(negative.error().code(), ErrorCode::InvalidArgument);
        auto empty = put(*journal, document("", 1), "x");
        ASSERT_FALSE(empty.has_value());
        EXPECT_EQ(empty.error().code(), ErrorCode::InvalidArgument);
    }
    RecoveredJournal recovered = recoverOne(dir.path());
    auto entries = recovered.entries();
    ASSERT_TRUE(entries.has_value() && entries->size() == 1);
    EXPECT_EQ((*entries)[0].state.generation, 6);
    EXPECT_EQ(recovered.readContent((*entries)[0].id).value(), "six");
}

TEST(HotExitJournal, UntitledWithoutBase) {
    ScratchDir dir("storage-journal");
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
        ASSERT_TRUE(journal.has_value());
        DocumentState state;
        state.documentKey = "untitled:1";
        state.generation = 0;
        ASSERT_TRUE(put(*journal, state, "draft").has_value());
    }
    RecoveredJournal recovered = recoverOne(dir.path());
    auto entries = recovered.entries();
    ASSERT_TRUE(entries.has_value() && entries->size() == 1);
    EXPECT_EQ((*entries)[0].state.path, "");
    EXPECT_FALSE((*entries)[0].state.base.has_value());
    EXPECT_EQ(recovered.readContent((*entries)[0].id).value(), "draft");
}

TEST(HotExitJournal, NonAsciiAndLongKeys) {
    ScratchDir dir("storage-journal");
    const std::string keys[] = {"C:\\Ünïcødé\\日本語\\файл.cpp", std::string(5000, 'k'), "b", "a", "\xF0\x9F\x98\x80"};
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
        ASSERT_TRUE(journal.has_value());
        for (const std::string& key : keys) {
            ASSERT_TRUE(put(*journal, document(key, 1), key).has_value());
        }
        auto entries = journal->entries();
        ASSERT_TRUE(entries.has_value());
        ASSERT_EQ(entries->size(), std::size(keys));
        // Sorted bytewise.
        for (std::size_t i = 1; i < entries->size(); ++i) {
            EXPECT_LT((*entries)[i - 1].state.documentKey, (*entries)[i].state.documentKey);
        }
    }
    RecoveredJournal recovered = recoverOne(dir.path());
    auto entries = recovered.entries();
    ASSERT_TRUE(entries.has_value());
    for (const auto& entry : *entries) {
        EXPECT_EQ(recovered.readContent(entry.id).value(), entry.state.documentKey);
    }
}

TEST(HotExitJournal, ChunkBytesOutOfRangeIsInvalidArgument) {
    ScratchDir dir("storage-journal");
    for (const std::size_t chunk : {std::size_t{0}, std::size_t{4095}, (std::size_t{64} << 20) + 1}) {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal(chunk));
        ASSERT_FALSE(journal.has_value());
        EXPECT_EQ(journal.error().code(), ErrorCode::InvalidArgument);
    }
    auto listing = frost::io::realFileSystem().listDirectory(dir.path());
    ASSERT_TRUE(listing.has_value());
    EXPECT_TRUE(listing->empty());
}

TEST(HotExitJournal, MarkCleanKeepsNewerEntryAndStalesOlderPuts) {
    ScratchDir dir("storage-journal");
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
        ASSERT_TRUE(journal.has_value());
        ASSERT_TRUE(put(*journal, document("k", 3), "three").has_value());
        // Saved as of 2: the entry (3) is newer and stays.
        EXPECT_EQ(journal->markClean("k", 2).value(), false);
        EXPECT_EQ(journal->entries().value().size(), 1u);
        // Saved as of 3: the entry goes.
        EXPECT_EQ(journal->markClean("k", 3).value(), true);
        EXPECT_TRUE(journal->entries().value().empty());
        // A put queued before the save (generation <= 3) is Stale now.
        EXPECT_EQ(put(*journal, document("k", 3), "late").value(), PutOutcome::Stale);
        EXPECT_EQ(put(*journal, document("k", 1), "older").value(), PutOutcome::Stale);
        // The mark never moves back.
        EXPECT_EQ(journal->markClean("k", 1).value(), true);
        EXPECT_EQ(put(*journal, document("k", 2), "still older").value(), PutOutcome::Stale);
        EXPECT_EQ(put(*journal, document("k", 4), "new edit").value(), PutOutcome::Written);
        // Without an entry, markClean returns true.
        EXPECT_EQ(journal->markClean("other", 9).value(), true);
    }
    RecoveredJournal recovered = recoverOne(dir.path());
    auto entries = recovered.entries();
    ASSERT_TRUE(entries.has_value() && entries->size() == 1);
    EXPECT_EQ((*entries)[0].state.generation, 4);
}

TEST(HotExitJournal, ForgetClearsCleanMark) {
    ScratchDir dir("storage-journal");
    auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
    ASSERT_TRUE(journal.has_value());
    ASSERT_TRUE(put(*journal, document("k", 5), "five").has_value());
    EXPECT_TRUE(journal->markClean("k", 5).value());
    EXPECT_EQ(put(*journal, document("k", 2), "x").value(), PutOutcome::Stale);
    EXPECT_TRUE(journal->forget("k").value()); // The clean mark existed.
    EXPECT_FALSE(journal->forget("k").value());
    // A new buffer may reuse the key from any generation once forgotten.
    EXPECT_EQ(put(*journal, document("k", 2), "reopened").value(), PutOutcome::Written);
    EXPECT_TRUE(journal->forget("k").value());
    EXPECT_TRUE(journal->entries().value().empty());
}

TEST(HotExitJournal, StopTokenCancelsPutAndKeepsOldEntry) {
    ScratchDir dir("storage-journal");
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal(4096));
        ASSERT_TRUE(journal.has_value());
        ASSERT_TRUE(put(*journal, document("k", 1), "old content").has_value());
        std::stop_source stop;
        HookGuard guard(
            frost::storage::detail::g_afterChunkWritten,
            [](void* source) { static_cast<std::stop_source*>(source)->request_stop(); }, &stop);
        const std::string large = randomBytes(64 * 1024, 3);
        const std::string_view pieces[] = {large};
        auto cancelled = journal->put(document("k", 2), pieces, stop.get_token());
        ASSERT_FALSE(cancelled.has_value());
        EXPECT_EQ(cancelled.error().code(), ErrorCode::Cancelled);
        // An already-stopped token never starts.
        auto early = journal->put(document("k", 3), pieces, stop.get_token());
        ASSERT_FALSE(early.has_value());
        EXPECT_EQ(early.error().code(), ErrorCode::Cancelled);
    }
    RecoveredJournal recovered = recoverOne(dir.path());
    auto entries = recovered.entries();
    ASSERT_TRUE(entries.has_value() && entries->size() == 1);
    EXPECT_EQ((*entries)[0].state.generation, 1);
    EXPECT_EQ(recovered.readContent((*entries)[0].id).value(), "old content");
}

TEST(HotExitJournal, CloseCleanlyWithoutEntriesRemovesFiles) {
    ScratchDir dir("storage-journal");
    auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
    ASSERT_TRUE(journal.has_value());
    const std::string path = journal->path();
    ASSERT_TRUE(put(*journal, document("k", 1), "x").has_value());
    ASSERT_TRUE(journal->markClean("k", 1).value()); // Only a clean mark remains.
    ASSERT_TRUE(journal->closeCleanly().has_value());
    for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
        EXPECT_FALSE(fileExists(path + suffix)) << suffix;
    }
    auto scan = findRecoverableJournals(dir.path(), kLockWait);
    ASSERT_TRUE(scan.has_value());
    EXPECT_TRUE(scan->journals.empty());
}

TEST(HotExitJournal, CloseCleanlyKeepsEntriesForHotExit) {
    ScratchDir dir("storage-journal");
    std::string path;
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
        ASSERT_TRUE(journal.has_value());
        path = journal->path();
        ASSERT_TRUE(put(*journal, document("k", 1), "unsaved").has_value());
        ASSERT_TRUE(journal->closeCleanly().has_value());
        EXPECT_TRUE(fileExists(path));
        EXPECT_FALSE(fileExists(path + "-wal")); // Checkpointed on close.
    }
    RecoveredJournal recovered = recoverOne(dir.path());
    EXPECT_TRUE(recovered.endedCleanly());
    EXPECT_EQ(recovered.entries().value().size(), 1u);
}

TEST(HotExitJournal, DestructorMeansCrash) {
    ScratchDir dir("storage-journal");
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
        ASSERT_TRUE(journal.has_value());
        ASSERT_TRUE(put(*journal, document("k", 1), "unsaved").has_value());
    }
    RecoveredJournal recovered = recoverOne(dir.path());
    EXPECT_FALSE(recovered.endedCleanly());
    // An empty crashed journal is still offered (and discarded by the caller).
    ASSERT_TRUE(std::move(recovered).discard().has_value());
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
        ASSERT_TRUE(journal.has_value());
    }
    RecoveredJournal empty = recoverOne(dir.path());
    EXPECT_FALSE(empty.endedCleanly());
    EXPECT_TRUE(empty.entries().value().empty());
}

TEST(HotExitJournal, CorruptChunkFailsOnlyThatEntry) {
    ScratchDir dir("storage-journal");
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal(4096));
        ASSERT_TRUE(journal.has_value());
        for (int i = 0; i < 3; ++i) {
            const std::string content = randomBytes(10000, static_cast<std::uint64_t>(i));
            ASSERT_TRUE(put(*journal, document(std::format("doc{}", i), 1), content).has_value());
        }
    }
    RecoveredJournal recovered = recoverOne(dir.path());
    auto entries = recovered.entries();
    ASSERT_TRUE(entries.has_value() && entries->size() == 3);
    const std::int64_t damagedId = (*entries)[1].id;
    FROST_STORAGE_ASSERT_OK(recovered.database().execute(
        std::format("UPDATE journal_chunk SET data = zeroblob(4096) WHERE entry_id = {} AND seq = 1", damagedId)));
    for (const auto& entry : *entries) {
        std::string received;
        auto read = recovered.readContent(entry.id, [&](std::string_view bytes) -> frost::Result<void> {
            received.append(bytes);
            return {};
        });
        if (entry.id == damagedId) {
            ASSERT_FALSE(read.has_value());
            EXPECT_EQ(read.error().code(), ErrorCode::Corrupt);
        } else {
            ASSERT_TRUE(read.has_value()) << read.error().describe();
            EXPECT_EQ(received.size(), 10000u);
        }
    }
    // A missing chunk and a truncated chunk are caught too.
    FROST_STORAGE_ASSERT_OK(recovered.database().execute(
        std::format("DELETE FROM journal_chunk WHERE entry_id = {} AND seq = 0", (*entries)[0].id)));
    EXPECT_EQ(recovered.readContent((*entries)[0].id).error().code(), ErrorCode::Corrupt);
    FROST_STORAGE_ASSERT_OK(recovered.database().execute(std::format(
        "UPDATE journal_chunk SET data = substr(data, 1, 10) WHERE entry_id = {} AND seq = 2", (*entries)[2].id)));
    EXPECT_EQ(recovered.readContent((*entries)[2].id).error().code(), ErrorCode::Corrupt);
    // A sink error stops the read and is returned.
    auto stopped = recovered.readContent((*entries)[1].id, [](std::string_view) -> frost::Result<void> {
        return frost::fail(ErrorCode::Timeout, "sink gave up");
    });
    ASSERT_FALSE(stopped.has_value());
    EXPECT_EQ(stopped.error().code(), ErrorCode::Timeout);
}

TEST(HotExitJournal, SecondOwnerIsBusy) {
    ScratchDir dir("storage-journal");
    auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
    ASSERT_TRUE(journal.has_value());
    frost::storage::OpenOptions options;
    options.mode = frost::storage::OpenMode::ReadWrite;
    options.exclusiveLock = true;
    options.busyTimeout = std::chrono::milliseconds(100);
    const auto started = std::chrono::steady_clock::now();
    auto second = frost::storage::Database::open(journal->path(), options);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code(), ErrorCode::Busy);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
    // The scan counts it as in use.
    auto scan = findRecoverableJournals(dir.path(), std::chrono::milliseconds(100));
    ASSERT_TRUE(scan.has_value());
    EXPECT_EQ(scan->inUse, 1u);
    EXPECT_TRUE(scan->journals.empty());
    // The owner is undisturbed.
    EXPECT_TRUE(put(*journal, document("k", 1), "x").has_value());
}

TEST(HotExitJournal, RecoveryScanMatchesOnlyJournalNames) {
    ScratchDir dir("storage-journal");
    const std::string valid = "journal-20261101T120000123Z-0123456789abcdef.db";
    const std::string garbage = randomBytes(100, 1);
    for (const std::string& name :
         {valid + "-wal", valid + "-shm", valid + "-journal", valid + ".corrupt-20261101T120000Z",
          std::string("journal-20261101T120000123Z-0123456789ABCDEF.db"), std::string("journal-x.db"),
          std::string("journal-20261101T120000123Z-0123456789abcde.db"), std::string("other.db")}) {
        writeBytes(dir.join(name), garbage);
    }
    // A directory with a journal's name is not a regular file.
    const std::string directoryNamedLikeAJournal = dir.join("journal-20261101T120000124Z-0123456789abcdef.db");
    auto made = frost::io::realFileSystem().createDirectories(directoryNamedLikeAJournal);
    ASSERT_TRUE(made.has_value()) << made.error().describe();
    auto scan = findRecoverableJournals(dir.path(), kLockWait);
    ASSERT_TRUE(scan.has_value()) << scan.error().describe();
    EXPECT_TRUE(scan->journals.empty());
    EXPECT_EQ(scan->inUse, 0u);
    EXPECT_TRUE(scan->unreadable.empty());
    EXPECT_TRUE(scan->skipped.empty());
    EXPECT_EQ(scan->abandoned, 0u);
    auto listing = frost::io::realFileSystem().listDirectory(dir.path());
    ASSERT_TRUE(listing.has_value());
    EXPECT_EQ(listing->size(), 9u); // Nothing removed or added.
    // A missing directory is an empty scan.
    auto missing = findRecoverableJournals(dir.join("absent"), kLockWait);
    ASSERT_TRUE(missing.has_value());
    EXPECT_TRUE(missing->journals.empty());
}

TEST(HotExitJournal, GarbageJournalQuarantined) {
    ScratchDir dir("storage-journal");
    const std::string path = dir.join("journal-20261101T120000123Z-0123456789abcdef.db");
    const std::string garbage = randomBytes(5000, 2);
    writeBytes(path, garbage);
    auto scan = findRecoverableJournals(dir.path(), kLockWait);
    ASSERT_TRUE(scan.has_value());
    ASSERT_EQ(scan->unreadable.size(), 1u);
    EXPECT_EQ(scan->unreadable[0].reason.code(), ErrorCode::Corrupt);
    EXPECT_EQ(readBytes(scan->unreadable[0].movedTo), garbage);
    EXPECT_FALSE(fileExists(path));
}

TEST(HotExitJournal, AbandonedFreshJournalRemoved) {
    ScratchDir dir("storage-journal");
    const std::string path = dir.join("journal-20261101T120000123Z-0123456789abcdef.db");
    writeBytes(path, "");
    auto scan = findRecoverableJournals(dir.path(), kLockWait);
    ASSERT_TRUE(scan.has_value());
    EXPECT_EQ(scan->abandoned, 1u);
    EXPECT_TRUE(scan->journals.empty());
    EXPECT_FALSE(fileExists(path));
}

/*
 * A recovery scan in the instant between create()'s new file and its first lock sees an empty,
 * unlocked journal, exactly like an abandoned create. On POSIX it removes it, and create() must
 * notice and start again under a new name, so the journal it returns is a real file whose
 * entries survive a crash. On Windows the removal fails (create's open handle).
 */
TEST(HotExitJournal, ScanDuringCreateCannotOrphanTheJournal) {
    ScratchDir dir("storage-journal");
    struct Race {
        std::string directory;
        bool ran = false;
        std::size_t abandoned = 0;
        std::string error;
    } race{dir.path(), false, 0, {}};
    std::string path;
    {
        HookGuard guard(
            frost::storage::detail::g_afterSqliteOpen,
            [](void* context) {
                auto* state = static_cast<Race*>(context);
                if (state->ran) {
                    return; // The scan's own opens pass this hook too.
                }
                state->ran = true;
                auto scan = findRecoverableJournals(state->directory, std::chrono::milliseconds(0));
                if (!scan) {
                    state->error = scan.error().describe();
                    return;
                }
                state->abandoned = scan->abandoned;
            },
            &race);
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
        ASSERT_TRUE(journal.has_value()) << journal.error().describe();
        ASSERT_TRUE(race.ran);
        ASSERT_TRUE(race.error.empty()) << race.error;
#ifdef _WIN32
        EXPECT_EQ(race.abandoned, 0u);
#else
        EXPECT_EQ(race.abandoned, 1u);
#endif
        path = journal->path();
        EXPECT_TRUE(fileExists(path));
        auto written = put(*journal, document("a.cpp", 1), "kept\n");
        ASSERT_TRUE(written.has_value()) << written.error().describe();
    } // The journal is destroyed without closeCleanly (a crash), then the hook is cleared.
    auto scan = findRecoverableJournals(dir.path(), kLockWait);
    ASSERT_TRUE(scan.has_value());
    EXPECT_EQ(scan->abandoned, 0u);
    EXPECT_TRUE(scan->unreadable.empty());
    EXPECT_TRUE(scan->skipped.empty());
    ASSERT_EQ(scan->journals.size(), 1u);
    EXPECT_EQ(scan->journals[0].path(), path);
    auto entries = scan->journals[0].entries();
    ASSERT_TRUE(entries.has_value()) << entries.error().describe();
    ASSERT_EQ(entries->size(), 1u);
    EXPECT_EQ((*entries)[0].state.documentKey, "a.cpp");
    // Nothing of the removed first name is left: every file belongs to the returned journal.
    auto listing = frost::io::realFileSystem().listDirectory(dir.path());
    ASSERT_TRUE(listing.has_value());
    for (const frost::io::DirEntry& entry : *listing) {
        EXPECT_TRUE(entry.name.starts_with(frost::io::fileName(path))) << entry.name;
    }
}

TEST(HotExitJournal, NewerJournalSkippedUntouched) {
    ScratchDir dir("storage-journal");
    std::string path;
    // A journal that a newer FrostIDE (schema version 2) left behind at hot exit.
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
        ASSERT_TRUE(journal.has_value());
        path = journal->path();
        ASSERT_TRUE(put(*journal, document("k", 1), "x").has_value());
        FROST_STORAGE_ASSERT_OK(journal->database().execute("PRAGMA user_version=2"));
        ASSERT_TRUE(journal->closeCleanly().has_value());
    }
    const std::string before = readBytes(path);
    auto scan = findRecoverableJournals(dir.path(), kLockWait);
    ASSERT_TRUE(scan.has_value());
    EXPECT_TRUE(scan->journals.empty());
    ASSERT_EQ(scan->skipped.size(), 1u);
    EXPECT_EQ(scan->skipped[0].path, path);
    EXPECT_EQ(scan->skipped[0].reason.code(), ErrorCode::Unsupported);
    EXPECT_EQ(readBytes(path), before);
    EXPECT_FALSE(fileExists(path + "-wal"));
}

TEST(HotExitJournal, OwnJournalSkippedWithoutWaiting) {
    ScratchDir dir("storage-journal");
    auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
    ASSERT_TRUE(journal.has_value());
    const auto started = std::chrono::steady_clock::now();
    auto scan = findRecoverableJournals(dir.path(), kLockWait, journal->path());
    ASSERT_TRUE(scan.has_value());
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
    EXPECT_EQ(scan->inUse, 0u);
    EXPECT_TRUE(scan->journals.empty());
}

TEST(HotExitJournal, DiscardEmptiesBeforeRemoving) {
    ScratchDir dir("storage-journal");
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
        ASSERT_TRUE(journal.has_value());
        ASSERT_TRUE(put(*journal, document("a", 1), "x").has_value());
        ASSERT_TRUE(put(*journal, document("b", 1), "y").has_value());
        ASSERT_TRUE(journal->markClean("c", 1).has_value());
    }
    RecoveredJournal recovered = recoverOne(dir.path());
    const std::string path = recovered.path();
    struct Probe {
        frost::storage::Database* db;
        std::int64_t entries = -1;
        std::int64_t marks = -1;
    } probe{&recovered.database()};
    HookGuard guard(
        frost::storage::detail::g_discardAfterCommit,
        [](void* context) {
            auto* p = static_cast<Probe*>(context);
            auto count = [&](std::string_view sql) {
                auto statement = p->db->prepare(sql);
                return statement && statement->step().value_or(false) ? statement->columnInt64(0) : -2;
            };
            p->entries = count("SELECT count(*) FROM journal_entry");
            p->marks = count("SELECT count(*) FROM journal_clean");
        },
        &probe);
    ASSERT_TRUE(std::move(recovered).discard().has_value());
    EXPECT_EQ(probe.entries, 0);
    EXPECT_EQ(probe.marks, 0);
    EXPECT_FALSE(fileExists(path));
}

TEST(HotExitJournal, DamagedJournalOfferedThenQuarantined) {
    ScratchDir dir("storage-journal");
    std::string path;
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal(4096));
        ASSERT_TRUE(journal.has_value());
        path = journal->path();
        ASSERT_TRUE(put(*journal, document("k", 1), randomBytes(50000, 1)).has_value());
        ASSERT_TRUE(journal->closeCleanly().has_value());
    }
    std::int64_t pageSize = 0;
    std::int64_t lastPage = 0;
    {
        frost::storage::test::RawDb raw(path);
        pageSize = raw.queryInt("PRAGMA page_size");
        lastPage = raw.queryInt("PRAGMA page_count");
    }
    // Overwrite a whole chunk page with 0x5A bytes: its structure breaks, so quick_check fails.
    frost::storage::test::patchBytes(path, static_cast<std::size_t>((lastPage - 2) * pageSize),
                                     std::string(static_cast<std::size_t>(pageSize), '\x5A'));
    auto scan = findRecoverableJournals(dir.path(), kLockWait);
    ASSERT_TRUE(scan.has_value());
    ASSERT_EQ(scan->journals.size(), 1u);
    RecoveredJournal recovered = std::move(scan->journals.front());
    EXPECT_TRUE(recovered.damaged());
    EXPECT_TRUE(recovered.endedCleanly());
    ASSERT_TRUE(std::move(recovered).discard().has_value());
    EXPECT_FALSE(fileExists(path));
    std::size_t quarantined = 0;
    for (const auto& entry : frost::io::realFileSystem().listDirectory(dir.path()).value()) {
        quarantined += entry.name.find(".corrupt-") != std::string::npos && !entry.name.ends_with("-wal") ? 1 : 0;
    }
    EXPECT_EQ(quarantined, 1u);
}

/*
 * Salvage: one overflow page of entry "b"'s content is overwritten (its next-page pointer too, so
 * quick_check fails and the journal is offered as damaged). Entry "a", on other pages, still
 * reads back intact; "b" is Corrupt, never wrong bytes.
 */
TEST(HotExitJournal, DamagedJournalSalvagesIntactEntries) {
    ScratchDir dir("storage-journal");
    const std::string contentA = randomBytes(20000, 11);
    const std::string contentB = randomBytes(50000, 12);
    std::string path;
    {
        auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal(4096));
        ASSERT_TRUE(journal.has_value());
        path = journal->path();
        ASSERT_TRUE(put(*journal, document("a", 1), contentA).has_value());
        ASSERT_TRUE(put(*journal, document("b", 1), contentB).has_value());
    } // A crash; closing the connection still checkpoints the WAL into the main file.
    ASSERT_FALSE(fileExists(path + "-wal"));
    std::int64_t pageSize = 0;
    {
        frost::storage::test::RawDb raw(path);
        pageSize = raw.queryInt("PRAGMA page_size");
    }
    ASSERT_GT(pageSize, 64);
    // An overflow page holds a 4-byte next-page number, then payload: find one of b's.
    const std::string bytes = readBytes(path);
    const auto page = static_cast<std::size_t>(pageSize);
    std::size_t target = 0;
    for (std::size_t offset = page; offset + page <= bytes.size(); offset += page) {
        if (contentB.find(std::string_view(bytes).substr(offset + 4, 64)) != std::string::npos) {
            target = offset;
            break;
        }
    }
    ASSERT_NE(target, 0u) << "no overflow page of entry b found";
    frost::storage::test::patchBytes(path, target, std::string(page, '\x5A'));
    auto scan = findRecoverableJournals(dir.path(), kLockWait);
    ASSERT_TRUE(scan.has_value());
    ASSERT_EQ(scan->journals.size(), 1u);
    RecoveredJournal recovered = std::move(scan->journals.front());
    EXPECT_TRUE(recovered.damaged());
    EXPECT_FALSE(recovered.endedCleanly());
    auto entries = recovered.entries();
    ASSERT_TRUE(entries.has_value()) << entries.error().describe();
    ASSERT_EQ(entries->size(), 2u);
    ASSERT_EQ((*entries)[0].state.documentKey, "a");
    ASSERT_EQ((*entries)[1].state.documentKey, "b");
    auto a = recovered.readContent((*entries)[0].id);
    ASSERT_TRUE(a.has_value()) << a.error().describe();
    EXPECT_TRUE(*a == contentA);
    auto b = recovered.readContent((*entries)[1].id);
    ASSERT_FALSE(b.has_value()) << "damaged content read back as complete";
    EXPECT_EQ(b.error().code(), ErrorCode::Corrupt) << b.error().describe();
    ASSERT_TRUE(std::move(recovered).discard().has_value());
    EXPECT_FALSE(fileExists(path));
}

/*
 * closeCleanly's removal is bounded (200 ms, B20) even when another handle without
 * FILE_SHARE_DELETE is open on the file, as another instance's scan holds one between
 * sqlite3_open_v2 and its first lock: on Windows the delete then fails with Busy within the bound
 * (the win32 VFS's own retries took about 1.4 s); on POSIX the unlink goes through.
 */
TEST(HotExitJournal, CloseCleanlyRemovalIsBounded) {
    ScratchDir dir("storage-journal");
    auto journal = HotExitJournal::create(dir.path(), frost::systemClock(), fastJournal());
    ASSERT_TRUE(journal.has_value()) << journal.error().describe();
    const std::string path = journal->path();
    frost::storage::test::RawDb other(path); // sqlite3_open_v2 opens the file; no lock until a read.
    const auto started = std::chrono::steady_clock::now();
    auto closed = journal->closeCleanly();
    const auto took = std::chrono::steady_clock::now() - started;
    EXPECT_LT(took, std::chrono::milliseconds(1000))
        << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() << " ms";
#ifdef _WIN32
    ASSERT_FALSE(closed.has_value());
    EXPECT_EQ(closed.error().code(), ErrorCode::Busy) << closed.error().describe();
    EXPECT_TRUE(fileExists(path)); // A later scan removes it (clean and empty).
#else
    ASSERT_TRUE(closed.has_value()) << closed.error().describe();
    EXPECT_FALSE(fileExists(path));
#endif
}

#ifndef _WIN32
/*
 * Linux: create() refuses a directory so long that a recovery scan could not quarantine the
 * journal (the quarantine copy's name would exceed the unix VFS's 512 bytes: openStore's rule);
 * a journal path one byte shorter is created.
 */
TEST(HotExitJournal, DirectoryTooLongForQuarantineRefused) {
    ScratchDir dir("storage-journal");
    sqlite3_vfs* defaultVfs = sqlite3_vfs_find(nullptr);
    ASSERT_NE(defaultVfs, nullptr);
    const std::size_t limit = static_cast<std::size_t>(defaultVfs->mxPathname);
    std::size_t nameBytes = 0; // Journal names have a fixed length.
    {
        auto journal = HotExitJournal::create(dir.join("short"), frost::systemClock(), fastJournal());
        ASSERT_TRUE(journal.has_value()) << journal.error().describe();
        nameBytes = journal->path().size() - dir.join("short").size() - 1;
    }
    // A directory of exactly `bytes` bytes, in components well under NAME_MAX.
    const auto folderOf = [&dir](std::size_t bytes, char fill) {
        std::string folder = dir.path();
        while (bytes - folder.size() > 200) {
            folder += "/" + std::string(99, 'd');
        }
        folder += "/" + std::string(bytes - folder.size() - 1, fill);
        return folder;
    };
    const std::size_t longest = limit - 1 - 8 - frost::storage::detail::kQuarantineSuffixBytes;
    {
        const std::string folder = folderOf(longest - 1 - nameBytes, 'a');
        auto journal = HotExitJournal::create(folder, frost::systemClock(), fastJournal());
        ASSERT_TRUE(journal.has_value()) << journal.error().describe();
        EXPECT_EQ(journal->path().size(), longest);
    }
    const std::string folder = folderOf(longest - nameBytes, 'b');
    auto refused = HotExitJournal::create(folder, frost::systemClock(), fastJournal());
    ASSERT_FALSE(refused.has_value()) << refused->path();
    EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument) << refused.error().describe();
    auto listed = frost::io::realFileSystem().listDirectory(folder);
    ASSERT_TRUE(listed.has_value());
    EXPECT_TRUE(listed->empty()) << "a file was created: " << listed->front().name;
}
#endif

} // namespace
