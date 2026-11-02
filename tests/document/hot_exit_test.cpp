// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the SaveManager's hot exit (ADR 0028): unsaved buffers are journaled into a real
// storage::HotExitJournal in a scratch directory, a "crash" (the journal destroyed without a clean
// shutdown) leaves them for findRecoverableJournals, and restore() brings them back with their
// encoding and line breaks, rebases them on the disk file, and carries over entries the large-file
// guard refuses. Also the journal's encoding labels.

#include "frost/storage/hot_exit_journal.hpp"
#include "frost/text/encoding.hpp"

#include "document_fixture.hpp"
#include "journal_labels.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>

namespace frost::document::test {
namespace {

using namespace std::chrono_literals;

class HotExitTest : public DocumentFixture {
protected:
    HotExitTest() { options.load.text.detect.legacyCodePage = 1252; }

    // A journal for the current manager (created before it).
    void startJournal() {
        auto created = storage::HotExitJournal::create(m_scratch.join("journals"), clock);
        ASSERT_TRUE(created) << created.error().describe();
        m_journal.emplace(std::move(*created));
        services.journal = &*m_journal;
    }

    // The process dies: the manager goes, the journal closes without marking a clean shutdown.
    void crash() {
        resetManager();
        services.journal = nullptr;
        m_journal.reset();
    }

    // The next start: a fresh journal, then the scan for the journals the last run left.
    storage::RecoveryScan nextStart() {
        startJournal();
        auto scan = storage::findRecoverableJournals(m_scratch.join("journals"), 500ms, m_journal->path());
        EXPECT_TRUE(scan) << scan.error().describe();
        return scan ? std::move(*scan) : storage::RecoveryScan{};
    }

    std::size_t journalEntries() {
        auto entries = m_journal->entries();
        EXPECT_TRUE(entries) << entries.error().describe();
        return entries ? entries->size() : 0;
    }

    ~HotExitTest() override { crash(); }

    std::optional<storage::HotExitJournal> m_journal;

private:
    frost::test::ScratchDir m_scratch{"document-hot-exit"};
};

TEST_F(HotExitTest, JournalWrittenAndRestoredAfterACrash) {
    writeExternally("legacy.txt", "caf\xE9\r\nline\r\n");
    startJournal();
    FileDocument* legacy = open("legacy.txt");
    ASSERT_NE(legacy, nullptr);
    edit(*legacy, 0, 0, "// ");
    FileDocument& untitled =
        manager().createUntitled(text::TextFileFormat{text::Encoding::utf8(), false, text::LineSeparator::Lf});
    edit(untitled, 0, 0, "scratch notes\n");

    const JournalReport first = manager().journalDirty();
    EXPECT_TRUE(first.errors.empty());
    EXPECT_EQ(first.written, 2U);
    EXPECT_EQ(manager().journalDirty().written, 0U); // Nothing changed since.
    EXPECT_EQ(journalEntries(), 2U);
    crash();

    storage::RecoveryScan scan = nextStart();
    ASSERT_EQ(scan.journals.size(), 1U);
    const RestoreReport report = manager().restore(scan);
    EXPECT_TRUE(report.errors.empty());
    EXPECT_TRUE(report.deferred.empty());
    EXPECT_TRUE(report.diskChanged.empty());
    EXPECT_EQ(report.journalsDiscarded, 1U);
    ASSERT_EQ(report.restored.size(), 2U);
    EXPECT_EQ(journalEntries(), 2U); // Safe in the new journal before the old one went.

    FileDocument* restored = manager().find(path("legacy.txt"));
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->restored());
    EXPECT_TRUE(restored->dirty());
    EXPECT_EQ(textOf(*restored), "// caf\xC3\xA9\r\nline\r\n");
    EXPECT_EQ(text::encodingName(restored->encoding().format.encoding), "windows-1252");
    EXPECT_EQ(restored->encoding().format.lineSeparator, text::LineSeparator::CrLf);
    EXPECT_EQ(restored->diskState(), DiskState::InSync);
    EXPECT_EQ(restored->baseText().text(), "caf\xC3\xA9\r\nline\r\n");

    const auto untitledRestored =
        std::ranges::find_if(report.restored, [](const FileDocument* document) { return document->untitled(); });
    ASSERT_NE(untitledRestored, report.restored.end());
    EXPECT_EQ(textOf(**untitledRestored), "scratch notes\n");

    // Saving writes the journal's text in the file's own encoding and clears its entry.
    ASSERT_TRUE(manager().save(*restored));
    EXPECT_EQ(read("legacy.txt"), "// caf\xE9\r\nline\r\n");
    EXPECT_EQ(journalEntries(), 1U);

    // A clean shutdown keeps the untitled buffer for the next start (hot exit).
    ASSERT_TRUE(manager().closeJournal());
    services.journal = nullptr;
    m_journal.reset();
    resetManager();
    storage::RecoveryScan after = nextStart();
    ASSERT_EQ(after.journals.size(), 1U);
    EXPECT_TRUE(after.journals[0].endedCleanly());
    EXPECT_EQ(manager().restore(after).restored.size(), 1U);
}

TEST_F(HotExitTest, UndoBackToTheSavedStateClearsTheEntry) {
    writeExternally("a.txt", "a\n");
    startJournal();
    FileDocument* document = open("a.txt");
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 1, "A");
    EXPECT_EQ(manager().journalDirty().written, 1U);
    ASSERT_TRUE(document->document().undo());
    const JournalReport cleared = manager().journalDirty();
    EXPECT_EQ(cleared.cleared, 1U);
    EXPECT_EQ(journalEntries(), 0U);

    // A save clears it too, and closing the document forgets it.
    edit(*document, 0, 1, "B");
    EXPECT_EQ(manager().journalDirty().written, 1U);
    ASSERT_TRUE(manager().save(*document));
    EXPECT_EQ(journalEntries(), 0U);
    edit(*document, 0, 1, "C");
    EXPECT_EQ(manager().journalDirty().written, 1U);
    ASSERT_TRUE(manager().close(*document));
    EXPECT_EQ(journalEntries(), 0U);
}

TEST_F(HotExitTest, RestoreRebasesOnAFileThatChanged) {
    writeExternally("a.txt", "one\n");
    startJournal();
    FileDocument* document = open("a.txt");
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 3, "mine");
    EXPECT_EQ(manager().journalDirty().written, 1U);
    crash();

    writeExternally("a.txt", "theirs\n"); // Changed while FrostIDE was not running.
    storage::RecoveryScan scan = nextStart();
    const RestoreReport report = manager().restore(scan);
    ASSERT_EQ(report.restored.size(), 1U);
    ASSERT_EQ(report.diskChanged.size(), 1U);
    FileDocument* restored = report.diskChanged[0];
    EXPECT_EQ(textOf(*restored), "mine\n");
    EXPECT_EQ(restored->baseText().text(), "theirs\n");
    EXPECT_TRUE(restored->dirty());
    ASSERT_TRUE(manager().save(*restored)); // The disk's text is the base: saving is not a conflict.
    EXPECT_EQ(read("a.txt"), "mine\n");
}

TEST_F(HotExitTest, RestoreOfAFileDeletedMeanwhile) {
    writeExternally("gone.txt", "x\n");
    startJournal();
    FileDocument* document = open("gone.txt");
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 1, "y");
    EXPECT_EQ(manager().journalDirty().written, 1U);
    crash();
    ASSERT_TRUE(fs.removeFile(path("gone.txt")));
    storage::RecoveryScan scan = nextStart();
    const RestoreReport report = manager().restore(scan);
    ASSERT_EQ(report.restored.size(), 1U);
    EXPECT_EQ(report.restored[0]->diskState(), DiskState::Deleted);
    EXPECT_EQ(textOf(*report.restored[0]), "y\n");
    ASSERT_TRUE(manager().save(*report.restored[0]));
    EXPECT_EQ(read("gone.txt"), "y\n");
}

TEST_F(HotExitTest, EntriesTheGuardRefusesAreCarriedOver) {
    writeExternally("a.txt", "a\n");
    startJournal();
    FileDocument* document = open("a.txt");
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 1, std::string(200, 'A'));
    EXPECT_EQ(manager().journalDirty().written, 1U);
    crash();

    options.load.maxFileBytes = 100; // This run's guard refuses the entry.
    storage::RecoveryScan scan = nextStart();
    const RestoreReport report = manager().restore(scan);
    EXPECT_TRUE(report.restored.empty());
    ASSERT_EQ(report.deferred.size(), 1U);
    EXPECT_EQ(report.deferred[0].path, path("a.txt"));
    EXPECT_EQ(report.deferred[0].reason.code(), ErrorCode::OutOfRange);
    EXPECT_EQ(report.journalsDiscarded, 1U);
    EXPECT_EQ(journalEntries(), 1U); // Kept for a later run, in this run's journal.
    auto entries = m_journal->entries();
    ASSERT_TRUE(entries);
    ASSERT_EQ(entries->size(), 1U);
    EXPECT_EQ(entries->front().contentSize, 201U);
}

TEST(JournalLabels, RoundTripAndConservativeFallback) {
    text::DocumentEncoding encoding;
    encoding.format =
        text::TextFileFormat{text::Encoding::unicode(text::EncodingForm::Utf16Le), true, text::LineSeparator::CrLf};
    encoding.fidelity.loss = text::LossKind::Replaced;
    encoding.fidelity.replacements = 3;
    encoding.fidelity.firstLossFileOffset = 10;
    encoding.fidelity.firstLossTextOffset = ByteOffset{4};
    encoding.detection = text::EncodingGuess{text::Encoding::unicode(text::EncodingForm::Utf16Le), true, 100,
                                             text::DetectionSource::Bom};
    storage::DocumentState state;
    detail::encodeJournalLabels(encoding, state);
    EXPECT_TRUE(state.hasBom);
    EXPECT_EQ(detail::decodeJournalLabels(state), encoding);

    // A label this build cannot read never claims a lossless load.
    state.encoding = "no-such-encoding|none|0|||UTF-8|0|100|bom";
    const text::DocumentEncoding fallback = detail::decodeJournalLabels(state);
    EXPECT_EQ(fallback.format.encoding, text::Encoding::utf8());
    EXPECT_FALSE(fallback.fidelity.lossless());
}

} // namespace
} // namespace frost::document::test
