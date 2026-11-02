// SPDX-License-Identifier: Apache-2.0
//
// Functional tests of the Find in Files engine (frost/search/find_in_files.hpp; ADR 0045) on the
// real file system: plain, case-sensitive, whole-word and regex queries over a small tree with
// UTF-8, UTF-8 BOM, UTF-16LE (with and without a BOM) and windows-1252 files and a binary file;
// file masks and the Directory scope's default excludes; the Solution and Project scopes over the
// tiny MSBuild fixture; an open document's unsaved text; cancellation; the 10,000-match limit; and
// the file mask parser.

#include "frost/core/clock.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/search/file_mask.hpp"
#include "frost/search/find_in_files.hpp"
#include "frost/search/search_scope.hpp"

#include "search_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace frost::search::test {
namespace {

// The encodings tree: one "needle" line per file, in each encoding.
class EncodingsTree : public ::testing::Test {
protected:
    EncodingsTree() {
        tree.write("src/plain.cpp", "int needle = 1;\nint Needle = 2;\nint needles = 3;\n");
        tree.write("src/bom.h", "\xEF\xBB\xBF// needle in a BOM file\n");
        tree.write("src/wide.txt", "\xFF\xFE" + utf16le("first line\r\nthe needle here\r\n"));
        tree.write("src/wide_nobom.txt", utf16le("a needle without a byte order mark\n"));
        tree.write("src/legacy.txt", "caf\xE9 needle\r\nna\xEFve\r\n");
        tree.write("bin/skipped.cpp", "needle in an excluded directory\n");
        tree.write("data/blob.bin", std::string("\x00\x00\x00\x00needle\x00\x00\x00\x00\x01\x02", 18));
    }

    Tree tree{"search-encodings"};
    document::LoadService loader{io::realFileSystem(), loadOptions()};
};

TEST_F(EncodingsTree, PlainSearchFindsEveryEncodingAndSkipsBinaryFiles) {
    FindRun run = runFind(loader, directoryRequest(tree.root(), "needle"));
    ASSERT_TRUE(run.summary) << run.summary.error().describe();
    EXPECT_EQ(run.matchesIn("plain.cpp"), 3U); // Case-insensitive by default, substrings included.
    EXPECT_EQ(run.matchesIn("bom.h"), 1U);
    EXPECT_EQ(run.matchesIn("wide.txt"), 1U);
    EXPECT_EQ(run.matchesIn("wide_nobom.txt"), 1U);
    EXPECT_EQ(run.matchesIn("legacy.txt"), 1U);
    EXPECT_EQ(run.matchesIn("skipped.cpp"), 0U); // bin/ is a default exclude.
    EXPECT_EQ(run.matchesIn("blob.bin"), 0U);
    EXPECT_EQ(run.summary->skippedBinary, 1U);
    EXPECT_EQ(run.summary->matches, 7U);
    EXPECT_EQ(run.summary->filesMatched, 5U);
    EXPECT_FALSE(run.summary->limitReached);
    EXPECT_FALSE(run.summary->cancelled);

    // Positions are in the decoded UTF-8 text; the preview is the line without its break.
    const FileMatches* wide = run.file("wide.txt");
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(wide->matches[0].line, 1U);
    EXPECT_EQ(wide->matches[0].column, 4U);
    EXPECT_EQ(wide->matches[0].length, 6U);
    EXPECT_EQ(wide->matches[0].lineText, "the needle here");
    ASSERT_EQ(wide->matches[0].before.size(), 1U);
    EXPECT_EQ(wide->matches[0].before[0], "first line");
    const FileMatches* legacy = run.file("legacy.txt");
    ASSERT_NE(legacy, nullptr);
    EXPECT_EQ(legacy->matches[0].lineText, "caf\xC3\xA9 needle"); // windows-1252 decoded to UTF-8.
    EXPECT_EQ(legacy->matches[0].column, 6U);                     // "café " is 6 bytes in UTF-8.
    ASSERT_EQ(legacy->matches[0].after.size(), 2U);
    EXPECT_EQ(legacy->matches[0].after[0], "na\xC3\xAFve");
    EXPECT_NE(legacy->textHash, 0U);

    // File order: the batches arrive sorted by path, as the walk lists them.
    std::vector<std::string> paths;
    for (const FileMatches& file : run.files) {
        paths.push_back(file.path);
    }
    EXPECT_TRUE(std::ranges::is_sorted(paths)) << paths.front();
}

TEST_F(EncodingsTree, CaseWordAndRegexOptions) {
    text::SearchOptions caseSensitive;
    caseSensitive.caseSensitive = true;
    FindRun exact = runFind(loader, directoryRequest(tree.root(), "Needle", caseSensitive));
    ASSERT_TRUE(exact.summary);
    EXPECT_EQ(exact.summary->matches, 1U);
    EXPECT_EQ(exact.matchesIn("plain.cpp"), 1U);

    text::SearchOptions word;
    word.wholeWord = true;
    FindRun words = runFind(loader, directoryRequest(tree.root(), "needle", word));
    ASSERT_TRUE(words.summary);
    EXPECT_EQ(words.matchesIn("plain.cpp"), 2U); // "needles" is not the word.
    EXPECT_EQ(words.summary->matches, 6U);

    text::SearchOptions regex;
    regex.regex = true;
    FindRun pattern = runFind(loader, directoryRequest(tree.root(), R"(caf. ne+dle)", regex));
    ASSERT_TRUE(pattern.summary);
    EXPECT_EQ(pattern.summary->matches, 1U);
    EXPECT_EQ(pattern.matchesIn("legacy.txt"), 1U);

    // Matching is per line: a regex across a line break finds nothing unless multiline is on.
    FindRequest across = directoryRequest(tree.root(), R"(needle = 1;\nint)", regex);
    FindRun perLine = runFind(loader, across);
    ASSERT_TRUE(perLine.summary);
    EXPECT_EQ(perLine.summary->matches, 0U);
    across.query.multiline = true;
    FindRun multi = runFind(loader, across);
    ASSERT_TRUE(multi.summary);
    EXPECT_EQ(multi.summary->matches, 1U);

    FindRun invalid = runFind(loader, directoryRequest(tree.root(), "(unclosed", regex));
    ASSERT_FALSE(invalid.summary);
    EXPECT_EQ(invalid.summary.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(EncodingsTree, FileMasksAndExcludes) {
    FindRun headers = runFind(loader, directoryRequest(tree.root(), "needle", {}, "*.cpp, *.h, !plain.*"));
    ASSERT_TRUE(headers.summary);
    EXPECT_EQ(headers.summary->matches, 1U); // bom.h; plain.cpp is excluded, bin/ is skipped.
    EXPECT_EQ(headers.matchesIn("bom.h"), 1U);

    // An emptied exclude list walks bin/ too.
    FindRequest everything = directoryRequest(tree.root(), "needle", {}, "*.cpp");
    everything.target.excludes.clear();
    FindRun all = runFind(loader, everything);
    ASSERT_TRUE(all.summary);
    EXPECT_EQ(all.matchesIn("skipped.cpp"), 1U);
    EXPECT_EQ(all.matchesIn("plain.cpp"), 3U);

    FindRun badMask = runFind(loader, directoryRequest(tree.root(), "needle", {}, "*.cpp, !"));
    ASSERT_FALSE(badMask.summary);
    EXPECT_EQ(badMask.summary.error().code(), ErrorCode::InvalidArgument);
}

TEST(FileMask, ParsesInclusionsAndExclusions) {
    auto mask = FileMask::parse(" *.cpp; *.H , !*.generated.h ");
    ASSERT_TRUE(mask);
    EXPECT_EQ(mask->inclusions().size(), 2U);
    EXPECT_EQ(mask->exclusions().size(), 1U);
    EXPECT_TRUE(mask->matches("main.cpp"));
    EXPECT_TRUE(mask->matches("View.h")); // ASCII case is ignored.
    EXPECT_FALSE(mask->matches("View.generated.h"));
    EXPECT_FALSE(mask->matches("notes.txt"));
    EXPECT_TRUE(FileMask().matches("anything"));
    auto onlyExclusions = FileMask::parse("!*.txt");
    ASSERT_TRUE(onlyExclusions);
    EXPECT_TRUE(onlyExclusions->matches("a.cpp"));
    EXPECT_FALSE(onlyExclusions->matches("a.txt"));
    EXPECT_TRUE(globMatch("a?c.*", "a\xC3\xA9" "c.txt")); // '?' takes a whole UTF-8 character.
    EXPECT_FALSE(globMatch("a?c", "ac"));
    EXPECT_TRUE(globMatch("*", ""));
}

// The tiny MSBuild fixture: Lib (lib.h, lib.cpp) and App (main.cpp), all mentioning tinyAnswer.
class TinySolution : public ::testing::Test {
protected:
    void SetUp() override {
        auto root = io::normalizePath(FROST_TINY_SLN_DIR);
        ASSERT_TRUE(root) << FROST_TINY_SLN_DIR;
        directory = *root;
        auto sln = io::joinPath(directory, "Tiny.sln");
        ASSERT_TRUE(sln);
        auto loaded = project::SolutionModel::load(io::realFileSystem(), *sln);
        ASSERT_TRUE(loaded) << loaded.error().describe();
        model.emplace(std::move(*loaded));
    }

    [[nodiscard]] FindRequest request(SearchScope scope) const {
        FindRequest out;
        out.query.pattern = "tinyAnswer";
        auto target = resolveScope(scope, &*model, {});
        EXPECT_TRUE(target) << target.error().describe();
        out.target = target ? std::move(*target) : ScopeTarget{};
        return out;
    }

    std::string directory;
    std::optional<project::SolutionModel> model;
    document::LoadService loader{io::realFileSystem(), loadOptions()};
};

TEST_F(TinySolution, SolutionScopeSearchesEveryProjectFile) {
    FindRun run = runFind(loader, request(SearchScope{}));
    ASSERT_TRUE(run.summary) << run.summary.error().describe();
    EXPECT_EQ(run.matchesIn("main.cpp"), 1U);
    EXPECT_EQ(run.matchesIn("lib.cpp"), 1U);
    EXPECT_EQ(run.matchesIn("lib.h"), 1U);
    EXPECT_EQ(run.summary->filesMatched, 3U);
}

TEST_F(TinySolution, ProjectScopeSearchesTheCurrentFilesProject) {
    SearchScope scope;
    scope.kind = ScopeKind::Project;
    scope.currentFile = *io::joinPath(directory, io::kNativePathStyle == io::PathStyle::Windows ? "App\\main.cpp"
                                                                                                : "App/main.cpp");
    FindRun run = runFind(loader, request(scope));
    ASSERT_TRUE(run.summary) << run.summary.error().describe();
    EXPECT_EQ(run.summary->filesMatched, 1U);
    EXPECT_EQ(run.matchesIn("main.cpp"), 1U);

    scope.currentFile = *io::joinPath(directory, "Tiny.sln");
    auto outside = resolveScope(scope, &*model, {});
    ASSERT_FALSE(outside);
    EXPECT_EQ(outside.error().code(), ErrorCode::NotFound);
    EXPECT_FALSE(resolveScope(SearchScope{}, nullptr, {})); // No solution open.
}

TEST(FindInFiles, OpenDocumentsAreSearchedInTheirUnsavedText) {
    Tree tree("search-open-docs");
    const std::string file = tree.write("notes.txt", "old text on disk\n");
    tree.write("other.txt", "fresh words elsewhere\n");
    document::SaveManagerServices services;
    services.fs = &io::realFileSystem();
    services.clock = &systemClock();
    document::SavePolicies policies;
    policies.hotExit = false;
    policies.localHistory = false;
    document::SaveManagerOptions options;
    options.load = loadOptions();
    document::SaveManager manager(services, options, policies);
    auto opened = manager.open(file);
    ASSERT_TRUE(opened) << opened.error().describe();
    auto edited = (*opened)->document().replace(text::ByteRange{ByteOffset{0}, ByteOffset{3}}, "fresh", {});
    ASSERT_TRUE(edited);

    FindRequest request = directoryRequest(tree.root(), "fresh");
    request.openDocuments = SaveManagerDocuments(manager).openDocuments();
    ASSERT_EQ(request.openDocuments.size(), 1U);
    FindRun run = runFind(manager.loadService(), request);
    ASSERT_TRUE(run.summary);
    const FileMatches* notes = run.file("notes.txt");
    ASSERT_NE(notes, nullptr);
    EXPECT_TRUE(notes->openDocument);
    EXPECT_EQ(notes->matches[0].lineText, "fresh text on disk");
    EXPECT_EQ(run.matchesIn("other.txt"), 1U);
    EXPECT_EQ(tree.read(file), "old text on disk\n"); // The disk was not touched.

    // The disk's text is not searched for an open document.
    FindRequest stale = directoryRequest(tree.root(), "old text");
    stale.openDocuments = SaveManagerDocuments(manager).openDocuments();
    FindRun none = runFind(manager.loadService(), stale);
    ASSERT_TRUE(none.summary);
    EXPECT_EQ(none.summary->matches, 0U);

    // The Open files scope lists exactly the open documents.
    SearchScope openScope;
    openScope.kind = ScopeKind::OpenFiles;
    auto target = resolveScope(openScope, nullptr, {file, file});
    ASSERT_TRUE(target);
    EXPECT_EQ(target->files, std::vector<std::string>{file});
}

TEST(FindInFiles, CancelStopsEarly) {
    Tree tree("search-cancel");
    for (int i = 0; i < 120; ++i) {
        tree.write(std::format("f{:03}.txt", i), "match\n");
    }
    document::LoadService loader(io::realFileSystem(), loadOptions());
    FindRequest request = directoryRequest(tree.root(), "match");
    request.limits.parallelism = 1;
    request.limits.batchFiles = 1;
    std::stop_source stop;
    FindRun run = runFind(loader, request, &stop, [&stop](const FindRun& progress) {
        if (progress.files.size() >= 3) {
            stop.request_stop();
        }
    });
    ASSERT_TRUE(run.summary) << run.summary.error().describe();
    EXPECT_TRUE(run.summary->cancelled);
    EXPECT_GE(run.files.size(), 3U);
    EXPECT_LT(run.summary->filesSearched, 10U);
    EXPECT_EQ(run.summary->filesInScope, 120U);
}

TEST(FindInFiles, StopsAtTenThousandMatches) {
    Tree tree("search-limit");
    std::string many;
    for (int i = 0; i < 6000; ++i) {
        many += "x x\n";
    }
    tree.write("a.txt", many); // 12,000 matches on its own.
    tree.write("b.txt", "x\n");
    document::LoadService loader(io::realFileSystem(), loadOptions());
    FindRequest request = directoryRequest(tree.root(), "x");
    request.limits.contextLines = 0;
    FindRun run = runFind(loader, request);
    ASSERT_TRUE(run.summary);
    EXPECT_EQ(run.summary->matches, kMaxMatches);
    EXPECT_TRUE(run.summary->limitReached);
    EXPECT_EQ(run.matchesIn("a.txt"), kMaxMatches);
    EXPECT_EQ(run.matchesIn("b.txt"), 0U);

    // Below the limit nothing is cut.
    request.limits.maxMatches = 20'000;
    FindRun all = runFind(loader, request);
    ASSERT_TRUE(all.summary);
    EXPECT_EQ(all.summary->matches, 12'001U);
    EXPECT_FALSE(all.summary->limitReached);
}

TEST(FindInFiles, LongLinesArePreviewedAroundTheMatch) {
    Tree tree("search-preview");
    const std::string line = std::string(2000, 'a') + "needle" + std::string(2000, 'b');
    tree.write("long.txt", line + "\n");
    document::LoadService loader(io::realFileSystem(), loadOptions());
    FindRun run = runFind(loader, directoryRequest(tree.root(), "needle"));
    ASSERT_TRUE(run.summary);
    const FileMatches* file = run.file("long.txt");
    ASSERT_NE(file, nullptr);
    const FindMatch& match = file->matches[0];
    EXPECT_EQ(match.column, 2000U);
    EXPECT_LE(match.lineText.size(), 320U);
    EXPECT_EQ(match.lineText.substr(match.previewColumn, match.previewLength), "needle");
    EXPECT_EQ(match.previewStart + match.previewColumn, match.column);
}

} // namespace
} // namespace frost::search::test
