// SPDX-License-Identifier: Apache-2.0
//
// Functional tests of Replace in Files (frost/search/replace_in_files.hpp, SaveManager::
// rewriteClosedFile; ADR 0045) on the real file system: a closed windows-1252 CRLF file keeps every
// byte but the replacements and gets a local-history snapshot first; an open document takes all of
// its replacements as one undo step and stays unsaved; an excluded match is left alone; a read-only
// file is skipped and reported; a file changed since the search is skipped; regex templates expand.

#include "frost/core/clock.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/search/find_in_files.hpp"
#include "frost/search/replace_in_files.hpp"
#include "frost/storage/local_history.hpp"

#include "search_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace frost::search::test {
namespace {

class ReplaceTest : public ::testing::Test {
protected:
    ReplaceTest() {
        services.fs = &io::realFileSystem();
        services.clock = &systemClock();
        policies.hotExit = false;
        options.load = loadOptions();
    }

    document::SaveManager& manager() {
        if (!m_manager) {
            m_manager = std::make_unique<document::SaveManager>(services, options, policies);
        }
        return *m_manager;
    }

    // Searches the tree for `pattern` (open documents included) and returns the result.
    FindRun search(std::string pattern, text::SearchOptions searchOptions = {}) {
        FindRequest request = directoryRequest(tree.root(), std::move(pattern), searchOptions);
        request.openDocuments = SaveManagerDocuments(manager()).openDocuments();
        FindRun run = runFind(manager().loadService(), std::move(request));
        EXPECT_TRUE(run.summary) << run.summary.error().describe();
        return run;
    }

    // The plan that replaces every match of `run`, except the matches `excluded` names by
    // (file name, index).
    static ReplaceRequest planOf(const FindRun& run, std::string pattern, std::string replacement,
                                 text::SearchOptions searchOptions = {},
                                 std::vector<std::pair<std::string, std::size_t>> excluded = {}) {
        ReplaceRequest request;
        request.pattern = std::move(pattern);
        request.options = searchOptions;
        request.replacement = std::move(replacement);
        for (const FileMatches& file : run.files) {
            ReplaceFilePlan plan;
            plan.path = file.path;
            plan.textHash = file.textHash;
            for (std::size_t i = 0; i < file.matches.size(); ++i) {
                const std::pair<std::string, std::size_t> key{std::string(io::fileName(file.path)), i};
                if (std::ranges::find(excluded, key) != excluded.end()) {
                    continue;
                }
                const FindMatch& match = file.matches[i];
                plan.ranges.push_back(
                    text::ByteRange{ByteOffset{match.offset}, ByteOffset{match.offset + match.length}});
            }
            request.files.push_back(std::move(plan));
        }
        return request;
    }

    static void setReadOnly(const std::string& file, bool readOnly) {
        std::error_code error;
        std::filesystem::permissions(
            io::toStdPath(file).value(),
            std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                std::filesystem::perms::others_write,
            readOnly ? std::filesystem::perm_options::remove : std::filesystem::perm_options::add, error);
        ASSERT_FALSE(error) << error.message();
    }

    Tree tree{"search-replace"};
    document::SaveManagerServices services{};
    document::SaveManagerOptions options{};
    document::SavePolicies policies{};

private:
    std::unique_ptr<document::SaveManager> m_manager;
};

TEST_F(ReplaceTest, ClosedLegacyCrlfFileKeepsEveryOtherByte) {
    auto history = storage::LocalHistory::openInMemory(systemClock());
    ASSERT_TRUE(history) << history.error().describe();
    services.history = &*history;
    const std::string original = "// caf\xE9\r\nint value = 1; // value\r\nna\xEFve value\r\n";
    const std::string file = tree.write("legacy.cpp", original);

    FindRun run = search("value");
    ASSERT_EQ(run.matchesIn("legacy.cpp"), 3U);
    auto summary = replaceInFiles(manager(), planOf(run, "value", "r\xC3\xA9sultat"));
    ASSERT_TRUE(summary) << summary.error().describe();
    EXPECT_EQ(summary->replacements, 3U);
    ASSERT_EQ(summary->replaced.size(), 1U);
    EXPECT_FALSE(summary->replaced[0].openDocument);
    EXPECT_TRUE(summary->skipped.empty());
    // windows-1252 bytes, the CRLFs and the other text exactly as they were; é became E9.
    EXPECT_EQ(tree.read(file), "// caf\xE9\r\nint r\xE9sultat = 1; // r\xE9sultat\r\nna\xEFve r\xE9sultat\r\n");

    // Local history holds the old bytes ("Before Replace in Files") and the new ones.
    auto key = io::normalizePath(file);
    ASSERT_TRUE(key);
    auto revisions = history->revisions(*key);
    ASSERT_TRUE(revisions) << revisions.error().describe();
    ASSERT_EQ(revisions->size(), 2U);
    EXPECT_EQ((*revisions)[1].label, "Before Replace in Files");
    EXPECT_EQ(history->content((*revisions)[1].id).value_or(""), original);
    EXPECT_EQ(history->content((*revisions)[0].id).value_or(""), tree.read(file));
}

TEST_F(ReplaceTest, BomAndUtf16FilesKeepTheirFormat) {
    const std::string bom = tree.write("bom.txt", "\xEF\xBB\xBFold\nold\n");
    const std::string wide = tree.write("wide.txt", "\xFF\xFE" + utf16le("say old\r\n"));
    FindRun run = search("old");
    auto summary = replaceInFiles(manager(), planOf(run, "old", "new"));
    ASSERT_TRUE(summary) << summary.error().describe();
    EXPECT_EQ(summary->replacements, 3U);
    EXPECT_EQ(tree.read(bom), "\xEF\xBB\xBFnew\nnew\n");
    EXPECT_EQ(tree.read(wide), "\xFF\xFE" + utf16le("say new\r\n"));
}

TEST_F(ReplaceTest, OpenDocumentTakesOneUndoStepAndStaysUnsaved) {
    const std::string file = tree.write("open.cpp", "a = old; b = old;\nc = old;\n");
    auto opened = manager().open(file);
    ASSERT_TRUE(opened) << opened.error().describe();
    document::FileDocument& document = **opened;

    FindRun run = search("old");
    ASSERT_EQ(run.matchesIn("open.cpp"), 3U);
    std::size_t notified = 0;
    auto summary = replaceInFiles(manager(), planOf(run, "old", "new"),
                                  [&](document::FileDocument& edited, const text::ChangeSet& change) {
                                      EXPECT_EQ(&edited, &document);
                                      EXPECT_EQ(change.edits.size(), 3U);
                                      ++notified;
                                  });
    ASSERT_TRUE(summary) << summary.error().describe();
    EXPECT_EQ(notified, 1U);
    EXPECT_EQ(summary->replacements, 3U);
    ASSERT_EQ(summary->replaced.size(), 1U);
    EXPECT_TRUE(summary->replaced[0].openDocument);
    EXPECT_EQ(document.document().committedText().text(), "a = new; b = new;\nc = new;\n");
    EXPECT_TRUE(document.dirty());
    EXPECT_EQ(tree.read(file), "a = old; b = old;\nc = old;\n"); // Saved later by autosave or the user.

    ASSERT_TRUE(document.document().undo());
    EXPECT_EQ(document.document().committedText().text(), "a = old; b = old;\nc = old;\n");
    EXPECT_FALSE(document.dirty());
    EXPECT_FALSE(document.document().undoState().canUndo);
}

TEST_F(ReplaceTest, ExcludedMatchesAreNotReplaced) {
    const std::string first = tree.write("a.txt", "one two one\n");
    const std::string second = tree.write("b.txt", "one\n");
    FindRun run = search("one");
    auto summary = replaceInFiles(manager(), planOf(run, "one", "1", {}, {{"a.txt", 1}, {"b.txt", 0}}));
    ASSERT_TRUE(summary) << summary.error().describe();
    EXPECT_EQ(summary->replacements, 1U);
    EXPECT_EQ(tree.read(first), "1 two one\n");
    EXPECT_EQ(tree.read(second), "one\n");
}

TEST_F(ReplaceTest, ReadOnlyFilesAreSkippedAndReported) {
    const std::string locked = tree.write("locked.txt", "keep me\n");
    const std::string free = tree.write("free.txt", "keep me\n");
    setReadOnly(locked, true);
    FindRun run = search("keep");
    auto summary = replaceInFiles(manager(), planOf(run, "keep", "drop"));
    setReadOnly(locked, false);
    ASSERT_TRUE(summary) << summary.error().describe();
    EXPECT_EQ(summary->replacements, 1U);
    ASSERT_EQ(summary->skipped.size(), 1U);
    EXPECT_EQ(io::fileName(summary->skipped[0].path), "locked.txt");
    EXPECT_EQ(summary->skipped[0].reason, ReplaceSkipReason::ReadOnly) << summary->skipped[0].error.describe();
    EXPECT_EQ(tree.read(locked), "keep me\n");
    EXPECT_EQ(tree.read(free), "drop me\n");
}

TEST_F(ReplaceTest, FilesChangedSinceTheSearchAreSkipped) {
    const std::string file = tree.write("moving.txt", "target\n");
    FindRun run = search("target");
    tree.write("moving.txt", "target moved\n");
    auto summary = replaceInFiles(manager(), planOf(run, "target", "x"));
    ASSERT_TRUE(summary);
    ASSERT_EQ(summary->skipped.size(), 1U);
    EXPECT_EQ(summary->skipped[0].reason, ReplaceSkipReason::Changed);
    EXPECT_EQ(tree.read(file), "target moved\n");
}

TEST_F(ReplaceTest, RegexTemplatesExpandAsInTheEditor) {
    const std::string file = tree.write("calls.cpp", "call(a, b);\ncall(c, d);\n");
    text::SearchOptions regex;
    regex.regex = true;
    FindRun run = search(R"(call\((\w), (\w)\))", regex);
    ASSERT_EQ(run.matchesIn("calls.cpp"), 2U);
    auto summary = replaceInFiles(manager(), planOf(run, R"(call\((\w), (\w)\))", "call($2, $1) /* $0 */", regex));
    ASSERT_TRUE(summary) << summary.error().describe();
    EXPECT_EQ(tree.read(file), "call(b, a) /* call(a, b) */;\ncall(d, c) /* call(c, d) */;\n");
}

} // namespace
} // namespace frost::search::test
