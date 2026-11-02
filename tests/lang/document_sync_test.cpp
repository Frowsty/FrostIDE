// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for document synchronisation (document_sync.hpp): positions in UTF-8 and UTF-16,
// and didChange content changes built from frost::text ChangeSets (single edits, multi-edit
// batches, undo and redo, non-ASCII text), checked by replaying them the way a server does.

#include "frost/lang/document_sync.hpp"
#include "frost/text/document.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using frost::ByteOffset;
using frost::lang::ContentChange;
using frost::lang::contentChanges;
using frost::lang::LspPosition;
using frost::lang::PositionEncoding;
using frost::text::ByteRange;
using frost::text::Document;
using frost::text::Edit;
using frost::text::EditContext;

ByteRange range(std::uint64_t begin, std::uint64_t end) {
    return ByteRange{.begin = ByteOffset{begin}, .end = ByteOffset{end}};
}

// The byte offset of an LSP position in `text`, as a server computes it.
std::size_t offsetOf(const std::string& text, LspPosition position, PositionEncoding encoding) {
    std::size_t at = 0;
    for (std::uint32_t line = 0; line < position.line; ++line) {
        at = text.find('\n', at);
        EXPECT_NE(at, std::string::npos);
        ++at;
    }
    std::uint32_t units = 0;
    while (at < text.size() && text[at] != '\n' && units < position.character) {
        const auto lead = static_cast<unsigned char>(text[at]);
        const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        units += encoding == PositionEncoding::Utf8 ? static_cast<std::uint32_t>(length) : (length == 4 ? 2U : 1U);
        at += length;
    }
    return at;
}

// Applies content changes in order, as LSP prescribes.
std::string replay(std::string text, const std::vector<ContentChange>& changes, PositionEncoding encoding) {
    for (const ContentChange& change : changes) {
        if (!change.range) {
            text = change.text;
            continue;
        }
        const std::size_t begin = offsetOf(text, change.range->start, encoding);
        const std::size_t end = offsetOf(text, change.range->end, encoding);
        text.replace(begin, end - begin, change.text);
    }
    return text;
}

class DocumentSync : public ::testing::TestWithParam<PositionEncoding> {};

TEST_P(DocumentSync, SingleEditBecomesOneRangeChange) {
    auto document = Document::fromUtf8("int main() {\n    return x;\n}\n");
    ASSERT_TRUE(document.has_value());
    const auto before = document->text();
    auto changes = document->replace(range(24, 25), "0", EditContext{});
    ASSERT_TRUE(changes.has_value());
    const auto out = contentChanges(*changes, before, document->text(), GetParam());
    ASSERT_EQ(out.size(), 1U);
    ASSERT_TRUE(out[0].range.has_value());
    EXPECT_EQ(out[0].range->start.line, 1U);
    EXPECT_EQ(replay(before.text(), out, GetParam()), document->text().text());
}

TEST_P(DocumentSync, MultiEditBatchesReplayExactly) {
    const std::string initial = "alpha\nbeta gamma\ndelta\n\xC3\xA9t\xC3\xA9 \xF0\x9F\x98\x80 end\n";
    auto document = Document::fromUtf8(initial);
    ASSERT_TRUE(document.has_value());
    const auto before = document->text();
    const std::vector<Edit> edits = {
        {.range = range(0, 5), .text = "ALPHA"},          // Same length.
        {.range = range(11, 16), .text = "g\nh"},         // Adds a line break.
        {.range = range(17, 23), .text = ""},             // Deletes the line "delta".
        {.range = range(29, 33), .text = "\xE2\x82\xAC"}, // Replaces the emoji (UTF-16: 2 units).
    };
    auto changes = document->edit(edits, EditContext{});
    ASSERT_TRUE(changes.has_value()) << changes.error().describe();
    const auto out = contentChanges(*changes, before, document->text(), GetParam());
    ASSERT_FALSE(out.empty());
    for (const auto& change : out) {
        EXPECT_TRUE(change.range.has_value()); // Incremental, not the full-text fallback.
    }
    EXPECT_EQ(replay(initial, out, GetParam()), document->text().text());
}

TEST_P(DocumentSync, UndoAndRedoReplayExactly) {
    auto document = Document::fromUtf8("one\ntwo\nthree\n");
    ASSERT_TRUE(document.has_value());
    std::string server = document->text().text();
    auto apply = [&](const frost::text::ChangeSet& changes, const frost::text::Snapshot& before) {
        server = replay(server, contentChanges(changes, before, document->text(), GetParam()), GetParam());
        EXPECT_EQ(server, document->text().text());
    };
    for (const auto& [at, text] : std::vector<std::pair<std::uint64_t, std::string>>{{4, "2"}, {0, "1\n"}, {13, "!"}}) {
        const auto before = document->text();
        document->breakCoalescing();
        auto changes = document->replace(range(at, at), text, EditContext{});
        ASSERT_TRUE(changes.has_value());
        apply(*changes, before);
    }
    for (int i = 0; i < 3; ++i) {
        const auto before = document->text();
        auto undone = document->undo();
        ASSERT_TRUE(undone.has_value()) << undone.error().describe();
        apply(*undone, before);
    }
    EXPECT_EQ(server, "one\ntwo\nthree\n");
    const auto before = document->text();
    auto redone = document->redo();
    ASSERT_TRUE(redone.has_value());
    apply(*redone, before);
}

TEST_P(DocumentSync, NoChangeMeansNoContentChanges) {
    auto document = Document::fromUtf8("x");
    ASSERT_TRUE(document.has_value());
    const auto before = document->text();
    auto changes = document->replace(range(1, 1), "", EditContext{}); // A no-op edit.
    ASSERT_TRUE(changes.has_value());
    EXPECT_EQ(changes->baseVersion, changes->version);
    EXPECT_TRUE(contentChanges(*changes, before, document->text(), GetParam()).empty());
}

INSTANTIATE_TEST_SUITE_P(Encodings, DocumentSync, ::testing::Values(PositionEncoding::Utf8, PositionEncoding::Utf16),
                         [](const auto& info) { return info.param == PositionEncoding::Utf8 ? "Utf8" : "Utf16"; });

TEST(DocumentSyncPositions, ConvertBothWays) {
    // "a€😀b": UTF-8 columns 0 1 4 8; UTF-16 columns 0 1 2 4.
    auto document = Document::fromUtf8("x\na\xE2\x82\xAC\xF0\x9F\x98\x80"
                                       "b");
    ASSERT_TRUE(document.has_value());
    const auto& snapshot = document->text();
    const auto utf8 = frost::lang::toLspPosition(snapshot, ByteOffset{10}, PositionEncoding::Utf8);
    EXPECT_EQ(utf8, (LspPosition{.line = 1, .character = 8}));
    const auto utf16 = frost::lang::toLspPosition(snapshot, ByteOffset{10}, PositionEncoding::Utf16);
    EXPECT_EQ(utf16, (LspPosition{.line = 1, .character = 4}));
    EXPECT_EQ(frost::lang::fromLspPosition(snapshot, utf16, PositionEncoding::Utf16).value(), ByteOffset{10});
    EXPECT_EQ(frost::lang::fromLspPosition(snapshot, utf8, PositionEncoding::Utf8).value(), ByteOffset{10});
    // Past the line end clamps; past the last line fails.
    EXPECT_EQ(frost::lang::fromLspPosition(snapshot, {.line = 0, .character = 99}, PositionEncoding::Utf8).value(),
              ByteOffset{1});
    EXPECT_FALSE(
        frost::lang::fromLspPosition(snapshot, {.line = 5, .character = 0}, PositionEncoding::Utf8).has_value());
}

TEST(DocumentSyncPositions, JsonShapes) {
    const ContentChange change{
        .range = frost::lang::LspRange{.start = {.line = 1, .character = 2}, .end = {.line = 3, .character = 4}},
        .text = "t"};
    EXPECT_EQ(frost::lang::toJson(change).dump(),
              R"({"range":{"start":{"line":1,"character":2},"end":{"line":3,"character":4}},"text":"t"})");
    EXPECT_EQ(frost::lang::toJson(ContentChange{.range = std::nullopt, .text = "all"}).dump(), R"({"text":"all"})");
    auto parsed = frost::lang::parseRange(frost::lang::toJson(*change.range));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, *change.range);
    EXPECT_EQ(frost::lang::languageIdForPath("a/b.c"), "c");
    EXPECT_EQ(frost::lang::languageIdForPath("a/b.H"), "cpp");
    EXPECT_EQ(frost::lang::languageIdForPath("a/b.cpp"), "cpp");
}

} // namespace
