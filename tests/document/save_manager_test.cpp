// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for frost::document's SaveManager on MemFs (ADR 0028): byte-exact save round trips
// (UTF-8 with and without a BOM, windows-1252, UTF-16LE, CRLF), clean documents, a full disk that
// leaves the original intact, Save As for untitled buffers, Save All, local-history snapshots and
// pruning, validation of settings files on save, the policies from settings and the stale temp sweep.

#include "frost/document/autosave.hpp"
#include "frost/document/policies.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/registry.hpp"
#include "frost/settings/store.hpp"
#include "frost/storage/local_history.hpp"
#include "frost/text/encoding.hpp"

#include "document_fixture.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace frost::document::test {
namespace {

using namespace std::chrono_literals;
using SaveManagerTest = DocumentFixture;

struct Fixture {
    std::string_view name;
    std::string bom;
    std::string payload;       // The file's bytes after the BOM.
    std::string_view encoding; // encodingName of the detected encoding.
};

TEST_F(SaveManagerTest, SaveRoundTripIsByteExact) {
    policies.localHistory = false;
    options.load.text.detect.legacyCodePage = 1252; // Whatever the machine's ANSI code page.
    const std::vector<Fixture> fixtures = {
        {"utf8.txt", "", "int main() {\n    return 0;\n}\n", "UTF-8"},
        {"utf8-bom-crlf.cpp", "\xEF\xBB\xBF", "// caf\xC3\xA9\r\nint x;\r\n", "UTF-8"},
        {"cp1252.txt", "", "caf\xE9 na\xEFve r\xE9sum\xE9\r\nline two\r\n", "windows-1252"},
        {"utf16le.txt", "\xFF\xFE", std::string("h\0i\0\r\0\n\0", 8), "UTF-16LE"},
        {"mixed-eol.txt", "", "lf\ncrlf\r\ncr\rend", "UTF-8"},
    };
    for (const Fixture& fixture : fixtures) {
        SCOPED_TRACE(fixture.name);
        const std::string original = fixture.bom + fixture.payload;
        writeExternally(fixture.name, original);
        FileDocument* document = open(fixture.name);
        ASSERT_NE(document, nullptr);
        EXPECT_EQ(text::encodingName(document->encoding().format.encoding), fixture.encoding);
        EXPECT_EQ(document->encoding().format.bom, !fixture.bom.empty());
        EXPECT_FALSE(document->dirty());

        // An ASCII edit at the start: the file gains exactly that character, encoded.
        edit(*document, 0, 0, "Z");
        EXPECT_TRUE(document->dirty());
        auto saved = manager().save(*document);
        ASSERT_TRUE(saved) << saved.error().describe();
        EXPECT_TRUE(saved->written);
        const std::string z = fixture.encoding == "UTF-16LE" ? std::string("Z\0", 2) : std::string("Z");
        EXPECT_EQ(read(fixture.name), fixture.bom + z + fixture.payload);
        EXPECT_FALSE(document->dirty());

        // Taking the edit back restores the original bytes exactly (line breaks never rewritten).
        edit(*document, 0, 1, "");
        ASSERT_TRUE(manager().save(*document));
        EXPECT_EQ(read(fixture.name), original);
        EXPECT_FALSE(tempFilesLeft());
    }
}

TEST_F(SaveManagerTest, CleanDocumentWritesNothingAndUndoMakesItCleanAgain) {
    writeExternally("a.txt", "one\ntwo\n");
    FileDocument* document = open("a.txt");
    ASSERT_NE(document, nullptr);
    auto saved = manager().save(*document);
    ASSERT_TRUE(saved);
    EXPECT_FALSE(saved->written);

    edit(*document, 0, 3, "ONE");
    EXPECT_TRUE(document->dirty());
    ASSERT_TRUE(document->document().undo());
    EXPECT_FALSE(document->dirty());
    ASSERT_TRUE(document->document().redo());
    EXPECT_TRUE(document->dirty());
    ASSERT_TRUE(manager().save(*document));
    EXPECT_EQ(read("a.txt"), "ONE\ntwo\n");
    ASSERT_TRUE(document->document().undo());
    EXPECT_TRUE(document->dirty()); // The disk now holds "ONE".

    // Opening the same path again returns the open document.
    auto again = manager().open(path("a.txt"));
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, document);
}

TEST_F(SaveManagerTest, DiskFullLeavesTheOriginalIntact) {
    writeExternally("big.txt", "original\n");
    FileDocument* document = open("big.txt");
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 8, "replacement that needs more space");
    fs.injectFault(io::FsOp::Write, path("big.txt"), ErrorCode::IoError);
    auto saved = manager().save(*document);
    ASSERT_FALSE(saved);
    EXPECT_EQ(saved.error().code(), ErrorCode::IoError) << saved.error().describe();
    EXPECT_EQ(read("big.txt"), "original\n");
    EXPECT_TRUE(document->dirty());
    EXPECT_FALSE(tempFilesLeft());

    // The rename step failing is just as safe; the next attempt succeeds.
    fs.injectFault(io::FsOp::AtomicRename, path("big.txt"), ErrorCode::IoError);
    ASSERT_FALSE(manager().save(*document));
    EXPECT_EQ(read("big.txt"), "original\n");
    ASSERT_TRUE(manager().save(*document));
    EXPECT_EQ(read("big.txt"), "replacement that needs more space\n");
    EXPECT_FALSE(tempFilesLeft());
}

TEST_F(SaveManagerTest, ConvertToChangesTheSavedEncoding) {
    writeExternally("c.txt", "caf\xC3\xA9\n");
    FileDocument* document = open("c.txt");
    ASSERT_NE(document, nullptr);
    document->convertTo(text::Encoding::legacy(1252), false);
    EXPECT_TRUE(document->dirty());
    ASSERT_TRUE(manager().save(*document));
    EXPECT_EQ(read("c.txt"), "caf\xE9\n");
    document->convertTo(text::Encoding::utf8(), true);
    ASSERT_TRUE(manager().save(*document));
    EXPECT_EQ(read("c.txt"), "\xEF\xBB\xBF"
                             "caf\xC3\xA9\n");
}

TEST_F(SaveManagerTest, UntitledBuffersNeedSaveAs) {
    FileDocument& document =
        manager().createUntitled(text::TextFileFormat{text::Encoding::utf8(), false, text::LineSeparator::CrLf});
    EXPECT_TRUE(document.untitled());
    EXPECT_EQ(document.key(), "untitled:1");
    EXPECT_EQ(document.diskState(), DiskState::Unsaved);
    edit(document, 0, 0, "new file\r\n");
    auto refused = manager().save(document);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);

    const SaveAllResult all = manager().saveAll();
    ASSERT_EQ(all.skipped.size(), 1U);
    EXPECT_EQ(all.skipped[0].reason, SkipReason::Untitled);

    auto saved = manager().saveAs(document, path("new.txt"));
    ASSERT_TRUE(saved) << saved.error().describe();
    EXPECT_EQ(saved->method, io::SafeWriteMethod::Created);
    EXPECT_EQ(read("new.txt"), "new file\r\n");
    EXPECT_EQ(document.path(), path("new.txt"));
    EXPECT_EQ(document.key(), path("new.txt"));
    EXPECT_FALSE(document.dirty());
    EXPECT_EQ(manager().find(path("new.txt")), &document);
}

TEST_F(SaveManagerTest, SaveAllSavesEveryDirtyDocument) {
    writeExternally("a.txt", "a\n");
    writeExternally("b.txt", "b\n");
    writeExternally("c.txt", "c\n");
    FileDocument* a = open("a.txt");
    FileDocument* b = open("b.txt");
    FileDocument* c = open("c.txt");
    ASSERT_TRUE(a != nullptr && b != nullptr && c != nullptr);
    edit(*a, 0, 1, "A");
    edit(*c, 0, 1, "C");
    fs.injectFault(io::FsOp::Write, path("c.txt"), ErrorCode::PermissionDenied);
    const SaveAllResult all = manager().saveAll();
    ASSERT_EQ(all.saved.size(), 1U);
    EXPECT_EQ(all.saved[0], a);
    ASSERT_EQ(all.failed.size(), 1U);
    EXPECT_EQ(all.failed[0].document, c);
    EXPECT_EQ(all.failed[0].error.code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(read("a.txt"), "A\n");
    EXPECT_EQ(read("b.txt"), "b\n");
    EXPECT_EQ(read("c.txt"), "c\n");
    EXPECT_TRUE(c->dirty());
}

TEST_F(SaveManagerTest, LocalHistorySnapshotsOnSaveAndPrunes) {
    auto history = storage::LocalHistory::openInMemory(clock);
    ASSERT_TRUE(history) << history.error().describe();
    services.history = &*history;
    writeExternally("h.txt", "v0\r\n");
    FileDocument* document = open("h.txt");
    ASSERT_NE(document, nullptr);
    edit(*document, 1, 2, "1");
    ASSERT_TRUE(manager().save(*document));
    edit(*document, 1, 2, "2");
    ASSERT_TRUE(manager().save(*document));

    auto revisions = history->revisions(document->key());
    ASSERT_TRUE(revisions);
    ASSERT_EQ(revisions->size(), 2U);
    EXPECT_EQ((*revisions)[0].kind, storage::RevisionKind::Save);
    EXPECT_EQ(history->content((*revisions)[0].id).value_or(""), "v2\r\n"); // The saved bytes, CRLF included.
    EXPECT_EQ(history->content((*revisions)[1].id).value_or(""), "v1\r\n");

    // Six days later (the default keeps five) a save prunes the old revisions; the newest stays.
    clock.advance(std::chrono::days(6));
    edit(*document, 1, 2, "3");
    ASSERT_TRUE(manager().save(*document));
    revisions = history->revisions(document->key());
    ASSERT_TRUE(revisions);
    ASSERT_EQ(revisions->size(), 1U);
    EXPECT_EQ(history->content((*revisions)[0].id).value_or(""), "v3\r\n");

    // An external change is recorded too, before it is loaded.
    writeExternally("h.txt", "external\r\n");
    EXPECT_EQ(manager().checkDisk(*document).kind, ExternalChangeKind::Reloaded);
    revisions = history->revisions(document->key());
    ASSERT_TRUE(revisions);
    ASSERT_EQ(revisions->size(), 2U);
    EXPECT_EQ((*revisions)[0].kind, storage::RevisionKind::ExternalChange);

    // With local history off, saves record nothing.
    SavePolicies off = manager().policies();
    off.localHistory = false;
    manager().setPolicies(off);
    edit(*document, 0, 1, "E");
    ASSERT_TRUE(manager().save(*document));
    EXPECT_EQ(history->revisions(document->key()).value().size(), 2U);
}

// A settings store over the MemFs with an application file holding `json`.
std::unique_ptr<settings::SettingsStore> openSettings(io::MemFs& fs, const std::string& file, std::string_view json) {
    EXPECT_TRUE(fs.writeFileAtomic(file, json));
    settings::SettingsPaths paths;
    paths.applicationFile = file;
    settings::StoreOptions storeOptions;
    storeOptions.style = io::PathStyle::Posix;
    auto store = settings::SettingsStore::open(settings::builtinRegistry(), fs, std::move(paths), storeOptions);
    EXPECT_TRUE(store) << store.error().describe();
    return store ? std::move(*store) : nullptr;
}

TEST_F(SaveManagerTest, PoliciesComeFromSettings) {
    const std::string file = path("settings.json");
    auto defaults = openSettings(fs, file, R"({"schemaVersion": 1})");
    ASSERT_NE(defaults, nullptr);
    const SavePolicies fromDefaults = SavePolicies::fromSettings(*defaults->snapshot());
    const SavePolicies builtIn;
    EXPECT_EQ(fromDefaults.autosave, builtIn.autosave);
    EXPECT_EQ(fromDefaults.hotExit, builtIn.hotExit);
    EXPECT_EQ(fromDefaults.localHistory, builtIn.localHistory);
    EXPECT_EQ(fromDefaults.retention.maxAge, builtIn.retention.maxAge);
    EXPECT_EQ(fromDefaults.retention.maxTotalBytes, builtIn.retention.maxTotalBytes);
    EXPECT_EQ(fromDefaults.retention.keepNewestPerPath, builtIn.retention.keepNewestPerPath);

    auto custom = openSettings(fs, file,
                               R"({"schemaVersion": 1, "save.autosave.idleSeconds": 30, "save.autosave.onIdle": false,
                                   "save.hotExit": false, "localHistory.maxAgeDays": 2, "localHistory.maxTotalMiB": 64})");
    ASSERT_NE(custom, nullptr);
    manager().applySettings(*custom->snapshot());
    const SavePolicies& applied = manager().policies();
    EXPECT_EQ(applied.autosave.idleDelay, 30s);
    EXPECT_FALSE(applied.autosave.onIdle);
    EXPECT_TRUE(applied.autosave.onFrameDeactivation);
    EXPECT_FALSE(applied.hotExit);
    EXPECT_EQ(applied.retention.maxAge, std::chrono::days(2));
    EXPECT_EQ(applied.retention.maxTotalBytes, std::uint64_t{64} << 20);
}

TEST_F(SaveManagerTest, SettingsFilesAreValidatedOnSave) {
    const std::string file = path("settings.json");
    auto store = openSettings(fs, file, R"({"schemaVersion": 1, "editor.tabSize": 4})");
    ASSERT_NE(store, nullptr);
    services.settings = store.get();
    FileDocument* document = open("settings.json");
    ASSERT_NE(document, nullptr);
    const std::string text = textOf(*document);
    const std::size_t four = text.find('4');
    ASSERT_NE(four, std::string::npos);
    edit(*document, four, four + 1, "8");
    auto saved = manager().save(*document);
    ASSERT_TRUE(saved) << saved.error().describe();
    ASSERT_TRUE(saved->settingsCheck.has_value());
    EXPECT_EQ(saved->settingsCheck->scope, settings::Scope::Application);
    EXPECT_EQ(store->snapshot()->get(settings::key::editorTabSize), 8);
}

TEST_F(SaveManagerTest, OpeningSweepsStaleTempFiles) {
    // A killed process's temp file (another pid), one of this process's own and an unrelated name.
    const std::string stale = path(".a.txt.frost-tmp-4000000000-7");
    const std::string fresh = path(".b.txt.frost-tmp-4000000001-1");
    ASSERT_TRUE(fs.writeFileAtomic(stale, "half a save"));
    clock.advance(2h);
    ASSERT_TRUE(fs.writeFileAtomic(fresh, "a save in flight elsewhere"));
    writeExternally("a.txt", "a\n");
    ASSERT_NE(open("a.txt"), nullptr);
    EXPECT_FALSE(fs.exists(stale).value());
    EXPECT_TRUE(fs.exists(fresh).value()); // Younger than SaveManagerOptions::staleTempAge.
}

} // namespace
// Simulates an OS save that committed bytes but needs post-publication recovery.
class CommittedWarningFs final : public io::IFileSystem {
public:
    explicit CommittedWarningFs(io::MemFs& fs) : m_fs(fs) {}
    Result<std::string> readFile(std::string_view p) override { return m_fs.readFile(p); }
    Result<void> writeFileAtomic(std::string_view p, std::string_view d) override { return m_fs.writeFileAtomic(p, d); }
    Result<io::FileStatus> status(std::string_view p) override { return m_fs.status(p); }
    Result<std::vector<io::DirEntry>> listDirectory(std::string_view p) override { return m_fs.listDirectory(p); }
    Result<io::DirListing> listDirectoryWithDiagnostics(std::string_view p) override {
        return m_fs.listDirectoryWithDiagnostics(p);
    }
    Result<void> createDirectories(std::string_view p) override { return m_fs.createDirectories(p); }
    Result<void> removeFile(std::string_view p) override { return m_fs.removeFile(p); }
    Result<void> removeDirectory(std::string_view p) override { return m_fs.removeDirectory(p); }
    Result<bool> exists(std::string_view p) override { return m_fs.exists(p); }
    Result<std::string> canonicalPath(std::string_view p) override { return m_fs.canonicalPath(p); }
    Result<io::SafeWriteResult> safeWrite(std::string_view p, std::string_view d,
                                          const io::SafeWriteOptions& options) override {
        auto result = m_fs.safeWrite(p, d, options);
        if (result)
            result->warning = Error(ErrorCode::IoError, "saved; recovery record needs attention");
        return result;
    }

private:
    io::MemFs& m_fs;
};

TEST_F(SaveManagerTest, CommittedWarningSurvivesSingleSaveSaveAllAndAutosave) {
    policies.hotExit = false;
    policies.localHistory = false;
    policies.autosave.onFrameDeactivation = true;
    ASSERT_TRUE(fs.writeFileAtomic(path("warning.txt"), "old"));
    CommittedWarningFs warningFs(fs);
    SaveManagerServices warningServices{};
    warningServices.fs = &warningFs;
    warningServices.clock = &clock;
    SaveManager warningManager(warningServices, options, policies);
    auto opened = warningManager.open(path("warning.txt"));
    ASSERT_TRUE(opened);
    FileDocument& doc = **opened;
    edit(doc, 0, 3, "one");
    auto one = warningManager.save(doc);
    ASSERT_TRUE(one);
    ASSERT_EQ(one->warnings.size(), 1u);
    EXPECT_EQ(one->warnings[0].message(), "saved; recovery record needs attention");
    EXPECT_FALSE(doc.dirty());
    EXPECT_EQ(read("warning.txt"), "one");
    edit(doc, 0, 3, "two");
    auto all = warningManager.saveAll();
    ASSERT_EQ(all.saved.size(), 1u);
    ASSERT_EQ(all.warnings.size(), 1u);
    EXPECT_EQ(all.warnings[0].document, &doc);
    EXPECT_FALSE(doc.dirty());
    EXPECT_EQ(read("warning.txt"), "two");
    edit(doc, 0, 3, "end");
    AutosaveController autosave(warningManager, clock);
    auto automatic = autosave.frameDeactivated();
    ASSERT_TRUE(automatic);
    ASSERT_EQ(automatic->saved.size(), 1u);
    ASSERT_EQ(automatic->warnings.size(), 1u);
    EXPECT_EQ(automatic->warnings[0].document, &doc);
    EXPECT_FALSE(doc.dirty());
    EXPECT_EQ(read("warning.txt"), "end");
    ASSERT_TRUE(fs.writeFileAtomic(path("closed.txt"), "old"));
    const auto replace = [](const text::Snapshot&) -> Result<std::vector<RewriteEdit>> {
        return std::vector<RewriteEdit>{{text::ByteRange{ByteOffset{0}, ByteOffset{3}}, "new"}};
    };
    auto rewritten = warningManager.rewriteClosedFile(path("closed.txt"), replace, "test");
    ASSERT_TRUE(rewritten);
    ASSERT_EQ(rewritten->warnings.size(), 1u);
    EXPECT_NE(rewritten->warnings[0].describe().find("/work/closed.txt"), std::string::npos);
    EXPECT_EQ(read("closed.txt"), "new");
}

// --- The pre-save transform (editor.trimTrailingWhitespace, editor.insertFinalNewline) ---

text::Snapshot snapshotOf(std::string text) {
    auto buffer = text::TextBuffer::fromUtf8(std::move(text));
    EXPECT_TRUE(buffer);
    return buffer->snapshot();
}

std::string applied(std::string text, const std::vector<SaveEdit>& edits) {
    for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit)
        text.replace(edit->range.begin.value(), edit->range.size(), edit->text);
    return text;
}

TEST(SaveTransformEdits, TrimsEveryLineOrOnlyModifiedOnesAndKeepsCaretLines) {
    const std::string base = "a  \nb\t\nc \n";
    const std::string current = "a  \nb2\t \nc \r\nd  ";
    SaveTransform all;
    all.trim = TrimTrailingWhitespace::All;
    EXPECT_EQ(applied(current, saveTransformEdits(snapshotOf(current), snapshotOf(base), all, "\n")), "a\nb2\nc\r\nd");
    // Modified: only the lines that differ from the saved text (b2, c's line break changed, d).
    SaveTransform modified;
    modified.trim = TrimTrailingWhitespace::Modified;
    EXPECT_EQ(applied(current, saveTransformEdits(snapshotOf(current), snapshotOf(base), modified, "\n")),
              "a  \nb2\nc\r\nd");
    // A caret line keeps its whitespace (typing is never undone under the caret).
    all.keepLines = {LineIndex{1}};
    EXPECT_EQ(applied(current, saveTransformEdits(snapshotOf(current), snapshotOf(base), all, "\n")),
              "a\nb2\t \nc\r\nd");
    SaveTransform none;
    EXPECT_TRUE(saveTransformEdits(snapshotOf(current), snapshotOf(base), none, "\n").empty());
}

TEST(SaveTransformEdits, InsertsTheFinalNewlineWithTheDocumentSeparator) {
    SaveTransform transform;
    transform.insertFinalNewline = true;
    const auto empty = snapshotOf("");
    EXPECT_EQ(applied("x\r\ny", saveTransformEdits(snapshotOf("x\r\ny"), empty, transform, "\r\n")), "x\r\ny\r\n");
    EXPECT_TRUE(saveTransformEdits(snapshotOf("x\n"), empty, transform, "\n").empty());
    EXPECT_TRUE(saveTransformEdits(snapshotOf("x\r"), empty, transform, "\n").empty());
    EXPECT_TRUE(saveTransformEdits(empty, empty, transform, "\n").empty()); // An empty file stays empty.
    // Trimming the last line and adding the newline is one edit.
    transform.trim = TrimTrailingWhitespace::All;
    const auto edits = saveTransformEdits(snapshotOf("x  "), empty, transform, "\n");
    ASSERT_EQ(edits.size(), 1U);
    EXPECT_EQ(applied("x  ", edits), "x\n");
}

TEST_F(SaveManagerTest, SaveTransformRunsOnEverySaveIncludingAutosaveAsOneUndoableEdit) {
    policies.localHistory = false;
    writeExternally("t.txt", "one  \ntwo");
    FileDocument* document = open("t.txt");
    ASSERT_NE(document, nullptr);
    std::vector<SaveReason> asked;
    manager().setSaveTransform([&](const FileDocument&, SaveReason reason) {
        asked.push_back(reason);
        SaveTransform transform;
        transform.trim = TrimTrailingWhitespace::All;
        transform.insertFinalNewline = true;
        return transform;
    });
    // A clean document is not touched.
    ASSERT_TRUE(manager().save(*document));
    EXPECT_TRUE(asked.empty());
    EXPECT_EQ(read("t.txt"), "one  \ntwo");
    // An autosave applies the save actions too: autosave is on by default, and skipping it left
    // most saves untrimmed.
    edit(*document, 9, 9, "!  ");
    SaveOptions autosave;
    autosave.reason = SaveReason::Autosave;
    ASSERT_TRUE(manager().save(*document, autosave));
    EXPECT_EQ(asked, std::vector<SaveReason>{SaveReason::Autosave});
    EXPECT_EQ(read("t.txt"), "one\ntwo!\n");
    EXPECT_EQ(textOf(*document), "one\ntwo!\n"); // The editor shows what was written.
    EXPECT_FALSE(document->dirty());
    edit(*document, 0, 0, "#  x  ");
    auto saved = manager().save(*document);
    ASSERT_TRUE(saved) << saved.error().describe();
    EXPECT_EQ(asked, (std::vector<SaveReason>{SaveReason::Autosave, SaveReason::Explicit}));
    EXPECT_EQ(read("t.txt"), "#  x  one\ntwo!\n"); // Only trailing blanks go; inner ones stay.
    EXPECT_FALSE(document->dirty());
    // Nothing trailing to trim: the explicit save added no undo step of its own.
    ASSERT_TRUE(document->document().undo());
    EXPECT_EQ(textOf(*document), "one\ntwo!\n");
    ASSERT_TRUE(document->document().undo()); // One undo takes the autosave's save actions back.
    EXPECT_EQ(textOf(*document), "one  \ntwo!  ");
}

TEST_F(SaveManagerTest, AnExplicitSaveAfterAnAutosaveAppliesWhatTheAutosaveSpared) {
    policies.localHistory = false;
    writeExternally("m.txt", "keep  \nb\nc\n");
    writeExternally("n.txt", "x\n");
    FileDocument* document = open("m.txt");
    FileDocument* other = open("n.txt");
    ASSERT_NE(document, nullptr);
    ASSERT_NE(other, nullptr);
    std::vector<LineIndex> caret{LineIndex{1}};
    int asked = 0;
    manager().setSaveTransform([&](const FileDocument&, SaveReason) {
        ++asked;
        SaveTransform transform;
        transform.trim = TrimTrailingWhitespace::Modified;
        transform.keepLines = caret;
        return transform;
    });
    edit(*document, 8, 8, "  ");   // "b  ", the caret line.
    edit(*document, 12, 12, "  "); // "c  "
    SaveOptions autosave;
    autosave.reason = SaveReason::Autosave;
    ASSERT_TRUE(manager().save(*document, autosave));
    // The idle autosave trims the edited line the caret is not on and spares the caret line.
    EXPECT_EQ(read("m.txt"), "keep  \nb  \nc\n");
    EXPECT_FALSE(document->dirty());
    // A later autosave of the clean document writes nothing.
    EXPECT_TRUE(manager().saveAll(SaveReason::Autosave).saved.empty());
    EXPECT_EQ(read("m.txt"), "keep  \nb  \nc\n");
    // The caret moved away: the user's Save All still trims line 1, which changed since the last
    // explicit save although the autosave moved the merge base. The unedited "keep  " stays.
    caret.clear();
    const SaveAllResult all = manager().saveAll(SaveReason::SaveAll);
    EXPECT_EQ(all.saved, std::vector<FileDocument*>{document}); // n.txt has nothing pending.
    EXPECT_TRUE(all.skipped.empty());
    EXPECT_TRUE(all.failed.empty());
    EXPECT_EQ(read("m.txt"), "keep  \nb\nc\n");
    EXPECT_EQ(textOf(*document), "keep  \nb\nc\n");
    EXPECT_FALSE(document->dirty());
    // That save was explicit: the next save of the clean document has nothing pending, asks nothing.
    const int before = asked;
    auto again = manager().save(*document);
    ASSERT_TRUE(again);
    EXPECT_FALSE(again->written);
    EXPECT_EQ(asked, before);

    // After an autosave that left nothing to do, an explicit save of the clean document writes nothing.
    caret = {LineIndex{0}};
    edit(*other, 0, 0, "y");
    ASSERT_TRUE(manager().save(*other, autosave));
    EXPECT_EQ(read("n.txt"), "yx\n");
    auto clean = manager().save(*other);
    ASSERT_TRUE(clean);
    EXPECT_FALSE(clean->written);
    EXPECT_EQ(read("n.txt"), "yx\n");
}

TEST_F(SaveManagerTest, AReloadEndsTheSaveActionsAnAutosaveLeftPending) {
    policies.localHistory = false;
    writeExternally("r.txt", "a\n");
    FileDocument* document = open("r.txt");
    ASSERT_NE(document, nullptr);
    std::vector<LineIndex> caret{LineIndex{0}};
    manager().setSaveTransform([&](const FileDocument&, SaveReason) {
        SaveTransform transform;
        transform.trim = TrimTrailingWhitespace::Modified;
        transform.keepLines = caret;
        return transform;
    });
    edit(*document, 1, 1, "  "); // "a  ", the caret line.
    SaveOptions autosave;
    autosave.reason = SaveReason::Autosave;
    ASSERT_TRUE(manager().save(*document, autosave));
    EXPECT_EQ(read("r.txt"), "a  \n");
    // Another program rewrites the file; the clean buffer reloads it. Its trailing blanks are not
    // the user's edit, so the user's next save leaves them.
    writeExternally("r.txt", "zz  \nmore  \n");
    const ExternalChange change = manager().checkDisk(*document);
    ASSERT_EQ(change.kind, ExternalChangeKind::Reloaded);
    EXPECT_EQ(textOf(*document), "zz  \nmore  \n");
    caret.clear();
    auto saved = manager().save(*document);
    ASSERT_TRUE(saved);
    EXPECT_FALSE(saved->written);
    EXPECT_EQ(read("r.txt"), "zz  \nmore  \n");
}
} // namespace frost::document::test
