// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for workspace edits (workspace_edit.hpp): decoding `changes` and `documentChanges`,
// byte conversion with sorting, UTF-16 columns and overlap refusal, mapping offsets through
// edits, and WorkspaceEditApplier over a fake set of open documents and a MemFs: open documents
// get one edit call each, closed files are rewritten, and conflicts refuse the whole edit.

#include "frost/io/mem_fs.hpp"
#include "frost/lang/uri.hpp"
#include "frost/lang/workspace_edit.hpp"
#include "frost/text/text_buffer.hpp"

#include "lang/feature_server.hpp"
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace {

using frost::ByteOffset;
using frost::ErrorCode;
using frost::lang::ByteEdit;
using frost::lang::Json;
using frost::lang::LspRange;
using frost::lang::OpenDocumentState;
using frost::lang::PositionEncoding;
using frost::lang::TextEdit;
using frost::lang::WorkspaceEdit;
using frost::test::workPath;

std::string uriOf(const std::string& path) { return frost::lang::pathToUri(path).value(); }

Json editJson(std::uint32_t line, std::uint32_t from, std::uint32_t to, std::string text) {
    return Json::object()
        .set("range", frost::lang::toJson(LspRange{{line, from}, {line, to}}))
        .set("newText", std::move(text));
}

// Open documents for the applier: each holds a TextBuffer; applyEdits applies and counts calls.
class FakeDocuments final : public frost::lang::IWorkspaceDocuments {
public:
    struct Doc {
        frost::text::TextBuffer buffer;
        std::optional<std::int64_t> lspVersion;
        bool dirty = false;
        bool changedOnDisk = false;
        int applyCalls = 0;
        std::string lastLabel;
    };

    void open(const std::string& path, std::string text, std::optional<std::int64_t> version = 1) {
        m_docs.insert_or_assign(frost::lang::documentKey(path),
                                Doc{.buffer = frost::text::TextBuffer::fromUtf8(std::move(text)).value(),
                                    .lspVersion = version,
                                    .dirty = false,
                                    .changedOnDisk = false,
                                    .applyCalls = 0,
                                    .lastLabel = {}});
    }
    Doc& doc(const std::string& path) { return m_docs.at(frost::lang::documentKey(path)); }

    std::optional<OpenDocumentState> find(std::string_view path) override {
        auto it = m_docs.find(frost::lang::documentKey(path));
        if (it == m_docs.end()) {
            return std::nullopt;
        }
        return OpenDocumentState{.snapshot = it->second.buffer.snapshot(),
                                 .lspVersion = it->second.lspVersion,
                                 .dirty = it->second.dirty,
                                 .changedOnDisk = it->second.changedOnDisk};
    }
    frost::Result<void> applyEdits(std::string_view path, std::span<const ByteEdit> edits,
                                   std::string_view label) override {
        Doc& d = m_docs.at(frost::lang::documentKey(path));
        std::vector<frost::text::Edit> plain;
        for (const ByteEdit& edit : edits) {
            plain.push_back({edit.range, edit.text});
        }
        auto applied = d.buffer.applyEdits(plain);
        if (!applied) {
            return std::unexpected(applied.error());
        }
        ++d.applyCalls;
        d.lastLabel = std::string(label);
        return {};
    }

private:
    std::map<std::string, Doc> m_docs;
};

TEST(WorkspaceEdit, DecodesChangesAndDocumentChanges) {
    const std::string a = workPath("a.cpp");
    const std::string b = workPath("b.cpp");
    auto changes = frost::lang::parseWorkspaceEdit(Json::object().set(
        "changes", Json::object()
                       .set(uriOf(a), Json::array({editJson(0, 0, 1, "x")}))
                       .set(uriOf(b), Json::array({editJson(1, 0, 0, "y"), editJson(2, 0, 0, "z")}))));
    ASSERT_TRUE(changes.has_value()) << changes.error().describe();
    ASSERT_EQ(changes->files.size(), 2u);
    EXPECT_EQ(changes->editCount(), 3u);

    auto documents = frost::lang::parseWorkspaceEdit(Json::object().set(
        "documentChanges", Json::array({Json::object()
                                            .set("textDocument", Json::object().set("uri", uriOf(a)).set("version", 7))
                                            .set("edits", Json::array({editJson(0, 0, 1, "x")})),
                                        Json::object()
                                            .set("textDocument", Json::object().set("uri", uriOf(a)).set("version", 7))
                                            .set("edits", Json::array({editJson(3, 0, 1, "w")}))})));
    ASSERT_TRUE(documents.has_value());
    ASSERT_EQ(documents->files.size(), 1u); // Merged per file.
    EXPECT_EQ(documents->files[0].version, 7);
    EXPECT_EQ(documents->files[0].edits.size(), 2u);

    auto fileOp = frost::lang::parseWorkspaceEdit(Json::object().set(
        "documentChanges", Json::array({Json::object().set("kind", "create").set("uri", uriOf(a))})));
    ASSERT_FALSE(fileOp.has_value());
    EXPECT_EQ(fileOp.error().code(), ErrorCode::Unsupported);
}

TEST(WorkspaceEdit, ByteEditsAreSortedAndOverlapsRefused) {
    // "é" is two UTF-8 bytes and one UTF-16 unit.
    const auto snapshot = frost::text::TextBuffer::fromUtf8("caf\xC3\xA9 bar\nx\n").value().snapshot();
    const std::vector<TextEdit> edits{{.range = LspRange{{1, 0}, {1, 1}}, .newText = "y"},
                                      {.range = LspRange{{0, 5}, {0, 8}}, .newText = "baz"}};
    auto utf16 = frost::lang::toByteEdits(snapshot, edits, PositionEncoding::Utf16);
    ASSERT_TRUE(utf16.has_value()) << utf16.error().describe();
    ASSERT_EQ(utf16->size(), 2u);
    EXPECT_EQ((*utf16)[0].range.begin, ByteOffset{6}); // UTF-16 column 5 is byte 6.
    EXPECT_EQ((*utf16)[0].text, "baz");
    EXPECT_EQ((*utf16)[1].range.begin, ByteOffset{10});

    const std::vector<TextEdit> overlapping{{.range = LspRange{{0, 0}, {0, 4}}, .newText = "a"},
                                            {.range = LspRange{{0, 2}, {0, 6}}, .newText = "b"}};
    auto refused = frost::lang::toByteEdits(snapshot, overlapping, PositionEncoding::Utf8);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);

    auto text = frost::lang::applyTextEdits("one two\n", std::vector<TextEdit>{{LspRange{{0, 4}, {0, 7}}, "2"}},
                                            PositionEncoding::Utf8);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "one 2\n");

    const std::vector<ByteEdit> sorted{{.range = {ByteOffset{0}, ByteOffset{0}}, .text = "ab"},
                                       {.range = {ByteOffset{4}, ByteOffset{6}}, .text = "x"}};
    EXPECT_EQ(frost::lang::mapThroughEdits(ByteOffset{2}, sorted), ByteOffset{4});
    EXPECT_EQ(frost::lang::mapThroughEdits(ByteOffset{5}, sorted), ByteOffset{7}); // Inside: end of "x".
    EXPECT_EQ(frost::lang::mapThroughEdits(ByteOffset{8}, sorted), ByteOffset{9});
}

TEST(WorkspaceEditApplier, EditsOpenDocumentsAndClosedFiles) {
    frost::io::MemFs fs(frost::systemClock());
    const std::string a = workPath("a.cpp");
    const std::string b = workPath("b.cpp");
    ASSERT_TRUE(fs.createDirectories(frost::io::parentPath(a)).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(b, "int value = value;\n").has_value());
    FakeDocuments documents;
    documents.open(a, "int value();\nint x = value();\n", 3);

    WorkspaceEdit edit;
    edit.files.push_back({.path = a,
                          .uri = uriOf(a),
                          .version = 3,
                          .edits = {{.range = LspRange{{0, 4}, {0, 9}}, .newText = "amount"},
                                    {.range = LspRange{{1, 8}, {1, 13}}, .newText = "amount"}}});
    edit.files.push_back({.path = b,
                          .uri = uriOf(b),
                          .version = std::nullopt,
                          .edits = {{.range = LspRange{{0, 4}, {0, 9}}, .newText = "amount"},
                                    {.range = LspRange{{0, 12}, {0, 17}}, .newText = "amount"}}});
    frost::lang::WorkspaceEditApplier applier(documents, fs, PositionEncoding::Utf8);
    auto applied = applier.apply(edit, "Rename");
    ASSERT_TRUE(applied.has_value()) << applied.error().describe();
    EXPECT_EQ(documents.doc(a).applyCalls, 1); // One undo step for both edits.
    EXPECT_EQ(documents.doc(a).lastLabel, "Rename");
    EXPECT_EQ(documents.doc(a).buffer.current().text(), "int amount();\nint x = amount();\n");
    EXPECT_EQ(fs.readFile(b).value(), "int amount = amount;\n");
}

TEST(WorkspaceEditApplier, ConflictsRefuseTheWholeEdit) {
    frost::io::MemFs fs(frost::systemClock());
    const std::string a = workPath("a.cpp");
    const std::string b = workPath("b.cpp");
    const std::string missing = workPath("gone.cpp");
    ASSERT_TRUE(fs.createDirectories(frost::io::parentPath(a)).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(b, "int value;\n").has_value());
    FakeDocuments documents;
    documents.open(a, "int value;\n", 4);
    frost::lang::WorkspaceEditApplier applier(documents, fs, PositionEncoding::Utf8);
    const auto renameIn = [&](const std::string& path, std::optional<std::int64_t> version) {
        return frost::lang::FileEdit{.path = path,
                                     .uri = uriOf(path),
                                     .version = version,
                                     .edits = {{.range = LspRange{{0, 4}, {0, 9}}, .newText = "amount"}}};
    };

    // The server computed the edit for version 3; the document is at 4.
    WorkspaceEdit stale;
    stale.files = {renameIn(b, std::nullopt), renameIn(a, 3)};
    auto refusedStale = applier.apply(stale, "Rename");
    ASSERT_FALSE(refusedStale.has_value());
    EXPECT_EQ(refusedStale.error().code(), ErrorCode::Busy);
    EXPECT_EQ(fs.readFile(b).value(), "int value;\n"); // Nothing was written.

    // Unsaved changes in the editor and a change on disk: the user must save or reload first.
    documents.doc(a).dirty = true;
    documents.doc(a).changedOnDisk = true;
    WorkspaceEdit crossFile;
    crossFile.files = {renameIn(b, std::nullopt), renameIn(a, std::nullopt)};
    auto refusedConflict = applier.apply(crossFile, "Rename");
    ASSERT_FALSE(refusedConflict.has_value());
    EXPECT_EQ(refusedConflict.error().code(), ErrorCode::Busy);
    EXPECT_NE(refusedConflict.error().message().find("save or reload"), std::string::npos);
    EXPECT_EQ(documents.doc(a).applyCalls, 0);
    EXPECT_EQ(fs.readFile(b).value(), "int value;\n");

    // A closed file that cannot be read.
    documents.doc(a).changedOnDisk = false;
    WorkspaceEdit withMissing;
    withMissing.files = {renameIn(a, std::nullopt), renameIn(missing, std::nullopt)};
    EXPECT_FALSE(applier.apply(withMissing, "Rename").has_value());
    EXPECT_EQ(documents.doc(a).applyCalls, 0);
    EXPECT_EQ(documents.doc(a).buffer.current().text(), "int value;\n");
}

} // namespace
