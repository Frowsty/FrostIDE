// SPDX-License-Identifier: Apache-2.0
//
// DisplayMap (ADR 0026): rows without and with soft wrap (by column and by width), the wrap rules
// (word boundaries, mid-word breaks, hanging spaces, tabs, wide, combining and joined characters),
// mapping round trips with soft wraps and block decorations, CRLF and lone CR, wraps and blocks
// after Document edits, undo and redo (each compared with a map built fresh over the result), row
// patches, hit testing, vertical caret movement and the error cases.

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/display_map.hpp"
#include "frost/text/document.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/text/text_shaper.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using frost::ByteOffset;
using frost::ErrorCode;
using frost::LineIndex;
using frost::Result;
using frost::sched::detail::SplitMix64;
using frost::text::Bias;
using frost::text::BlockDecoration;
using frost::text::BlockId;
using frost::text::BlockInfo;
using frost::text::BlockPlacement;
using frost::text::ByteRange;
using frost::text::ChangeSet;
using frost::text::DisplayColumn;
using frost::text::DisplayHit;
using frost::text::DisplayMap;
using frost::text::DisplayPoint;
using frost::text::DisplayRow;
using frost::text::DisplayRowCursor;
using frost::text::DisplayRowInfo;
using frost::text::Document;
using frost::text::Edit;
using frost::text::EditContext;
using frost::text::MonospaceTextShaper;
using frost::text::Point;
using frost::text::RowEdit;
using frost::text::RowKind;
using frost::text::Snapshot;
using frost::text::TextBuffer;
using frost::text::VerticalMove;
using frost::text::WrapMode;
using frost::text::WrapSettings;

// UTF-8 spellings used below (hex escapes end at a string boundary before any hex-digit letter).
constexpr std::string_view kNichi = "\xE6\x97\xA5";      // U+65E5, Wide
constexpr std::string_view kHon = "\xE6\x9C\xAC";        // U+672C, Wide
constexpr std::string_view kAcute = "\xCC\x81";          // U+0301 combining acute, Zero
constexpr std::string_view kZwj = "\xE2\x80\x8D";        // U+200D zero-width joiner
constexpr std::string_view kWoman = "\xF0\x9F\x91\xA9";  // U+1F469, Wide
constexpr std::string_view kLaptop = "\xF0\x9F\x92\xBB"; // U+1F4BB, Wide

const MonospaceTextShaper kCells; // One unit per cell, tab size 4.

WrapSettings columns(std::uint32_t n) { return WrapSettings{WrapMode::Column, n, 0}; }
ByteRange range(std::uint64_t a, std::uint64_t b) { return ByteRange{ByteOffset{a}, ByteOffset{b}}; }
DisplayPoint dp(std::uint32_t row, std::uint32_t column) {
    return DisplayPoint{DisplayRow{row}, DisplayColumn{column}};
}

Snapshot snap(std::string text) { return TextBuffer::fromUtf8(std::move(text)).value().snapshot(); }

std::string repeat(std::string_view piece, int times) {
    std::string out;
    for (int i = 0; i < times; ++i) {
        out += piece;
    }
    return out;
}

::testing::AssertionResult valid(const DisplayMap& map) {
    if (const Result<void> checked = map.checkInvariants(); !checked) {
        return ::testing::AssertionFailure() << checked.error().describe();
    }
    return ::testing::AssertionSuccess();
}

// Every row: a text row as its bytes, "~" marking a soft-wrap start or end; a block row as
// "#tag.row".
std::vector<std::string> rowsOf(const DisplayMap& map) {
    std::vector<std::string> out;
    DisplayRowCursor cursor = map.rows(DisplayRow{0});
    DisplayRowInfo row;
    while (cursor.next(row)) {
        if (row.kind == RowKind::Block) {
            out.push_back(std::format("#{}.{}", row.tag, row.blockRow));
        } else {
            out.push_back(
                std::format("{}{}{}", row.startsLine ? "" : "~", map.text().text(row.buffer), row.endsLine ? "" : "~"));
        }
    }
    return out;
}

// buffer -> display -> buffer for every char boundary and both biases, and display -> buffer ->
// display for every column of every text row.
void expectRoundTrips(const DisplayMap& map) {
    const Snapshot& text = map.text();
    const std::uint64_t size = text.size().value();
    for (std::uint64_t x = 0; x <= size; ++x) {
        if (!text.isCharBoundary(ByteOffset{x})) {
            continue;
        }
        const bool crlfInterior =
            x > 0 && x < size && text.byteAt(ByteOffset{x - 1}) == '\r' && text.byteAt(ByteOffset{x}) == '\n';
        for (const Bias bias : {Bias::Left, Bias::Right}) {
            const DisplayPoint point = map.toDisplayPoint(ByteOffset{x}, bias);
            const ByteOffset back = map.toOffset(point);
            EXPECT_EQ(back, ByteOffset{crlfInterior ? x - 1 : x}) << "offset " << x;
            const DisplayRowInfo info = map.rowInfo(point.row);
            EXPECT_EQ(info.kind, RowKind::Text) << "offset " << x;
            EXPECT_LE(info.buffer.begin, back);
            EXPECT_LE(back, info.buffer.end);
        }
    }
    for (std::uint32_t r = 0; r < map.rowCount(); ++r) {
        const DisplayRowInfo info = map.rowInfo(DisplayRow{r});
        if (info.kind == RowKind::Block) {
            continue;
        }
        const auto length = static_cast<std::uint32_t>(info.buffer.size());
        for (std::uint32_t column = 0; column <= length; ++column) {
            const ByteOffset offset = map.toOffset(dp(r, column));
            if (offset != info.buffer.begin + column) {
                continue; // Inside a character: floors to its start.
            }
            const Bias bias = column == length && !info.endsLine ? Bias::Left : Bias::Right;
            EXPECT_EQ(map.toDisplayPoint(offset, bias), dp(r, column)) << "row " << r << " column " << column;
        }
    }
}

// The patch is sorted and accounts for the change in the row count.
void expectPatch(const std::vector<RowEdit>& patch, std::uint32_t oldRows, std::uint32_t newRows) {
    std::int64_t shift = 0;
    std::uint32_t lastOld = 0;
    for (const RowEdit& edit : patch) {
        EXPECT_LE(lastOld, edit.oldFirst.value());
        EXPECT_LE(edit.oldFirst, edit.oldEnd);
        EXPECT_LE(edit.newFirst, edit.newEnd);
        EXPECT_EQ(std::int64_t{edit.newFirst.value()} - edit.oldFirst.value(), shift);
        shift += (std::int64_t{edit.newEnd.value()} - edit.newFirst.value()) -
                 (std::int64_t{edit.oldEnd.value()} - edit.oldFirst.value());
        lastOld = edit.oldEnd.value();
    }
    EXPECT_EQ(std::int64_t{oldRows} + shift, std::int64_t{newRows});
}

// A view of a Document: applies every ChangeSet and checks the map against one built fresh over
// the same text, with the same blocks re-added at their current offsets in creation order.
struct View {
    explicit View(std::string initial, WrapSettings wrap = {})
        : doc(Document::fromUtf8(std::move(initial)).value()), map(doc.text(), kCells, wrap) {}

    void apply(const Result<ChangeSet>& changes) {
        ASSERT_TRUE(changes.has_value()) << changes.error().describe();
        const std::uint32_t oldRows = map.rowCount();
        const Result<std::vector<RowEdit>> patch = map.apply(*changes, doc.text());
        ASSERT_TRUE(patch.has_value()) << patch.error().describe();
        expectPatch(*patch, oldRows, map.rowCount());
        check();
    }
    void replace(std::uint64_t a, std::uint64_t b, std::string_view text) {
        apply(doc.replace(range(a, b), text, EditContext{}));
    }
    BlockId block(std::uint64_t at, BlockPlacement placement, std::uint32_t rows, std::uint64_t tag) {
        const Result<BlockId> id = map.addBlock(BlockDecoration{ByteOffset{at}, placement, rows, tag});
        EXPECT_TRUE(id.has_value());
        blocks.push_back(*id);
        check();
        return *id;
    }
    void check() {
        EXPECT_EQ(map.version(), doc.version());
        EXPECT_TRUE(valid(map));
        DisplayMap fresh(map.text(), kCells, map.wrap());
        for (const BlockId id : blocks) {
            const Result<BlockInfo> info = map.blockInfo(id);
            ASSERT_TRUE(info.has_value());
            ASSERT_TRUE(fresh.addBlock(info->decoration).has_value());
        }
        EXPECT_EQ(rowsOf(map), rowsOf(fresh));
        expectRoundTrips(map);
    }

    Document doc;
    DisplayMap map;
    std::vector<BlockId> blocks;
};

// --- Rows and wrapping ---------------------------------------------------------------------------

TEST(DisplayMap, WrapOffHasOneRowPerLine) {
    // LF, CRLF and lone CR each end a line.
    const DisplayMap map(snap("one\ntwo\r\nthree\rfour"), kCells);
    EXPECT_EQ(map.wrapWidth(), 0);
    EXPECT_EQ(map.rowCount(), 4u);
    EXPECT_EQ(rowsOf(map), (std::vector<std::string>{"one", "two", "three", "four"}));
    const DisplayRowInfo two = map.rowInfo(DisplayRow{1});
    EXPECT_EQ(two.line, LineIndex{1});
    EXPECT_EQ(two.buffer, range(4, 7));
    ASSERT_EQ(two.segments.size(), 1u);
    EXPECT_EQ(two.segments[0].buffer, range(4, 7));
    // The CRLF-interior offset 8 shows at the end of its line.
    EXPECT_EQ(map.toDisplayPoint(ByteOffset{8}), dp(1, 3));
    EXPECT_EQ(map.toDisplayPoint(ByteOffset{9}), dp(2, 0));
    EXPECT_EQ(map.toDisplayPoint(Point{LineIndex{3}, frost::ByteColumn{2}}).value(), dp(3, 2));
    EXPECT_EQ(map.toDisplayPoint(Point{LineIndex{4}, frost::ByteColumn{0}}).error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(map.toPoint(dp(2, 4)), (Point{LineIndex{2}, frost::ByteColumn{4}}));
    EXPECT_TRUE(valid(map));
    expectRoundTrips(map);
}

TEST(DisplayMap, EmptyTextAndTrailingBreak) {
    const DisplayMap empty(snap(""), kCells, columns(4));
    EXPECT_EQ(empty.rowCount(), 1u);
    EXPECT_EQ(rowsOf(empty), (std::vector<std::string>{""}));
    EXPECT_TRUE(empty.rowInfo(DisplayRow{0}).segments.empty());
    EXPECT_EQ(empty.toDisplayPoint(ByteOffset{0}), dp(0, 0));
    EXPECT_EQ(empty.hitTest(DisplayRow{0}, 100).offset, ByteOffset{0});
    EXPECT_TRUE(valid(empty));

    const DisplayMap trailing(snap("a\rb\n"), kCells, columns(4));
    EXPECT_EQ(rowsOf(trailing), (std::vector<std::string>{"a", "b", ""}));
    EXPECT_EQ(trailing.toDisplayPoint(ByteOffset{4}), dp(2, 0));
    EXPECT_TRUE(valid(trailing));
    expectRoundTrips(trailing);
}

TEST(DisplayMap, WrapsAtWordBoundaries) {
    const DisplayMap map(snap("hello world foo bar"), kCells, columns(8));
    EXPECT_EQ(map.wrapWidth(), 8);
    EXPECT_EQ(rowsOf(map), (std::vector<std::string>{"hello ~", "~world ~", "~foo bar"}));
    const DisplayRowInfo second = map.rowInfo(DisplayRow{1});
    EXPECT_EQ(second.line, LineIndex{0});
    EXPECT_EQ(second.buffer, range(6, 12));
    EXPECT_FALSE(second.startsLine);
    EXPECT_FALSE(second.endsLine);
    // A soft-wrap offset belongs to the upper row with Left and to the lower row with Right.
    EXPECT_EQ(map.toDisplayPoint(ByteOffset{6}, Bias::Left), dp(0, 6));
    EXPECT_EQ(map.toDisplayPoint(ByteOffset{6}, Bias::Right), dp(1, 0));
    EXPECT_EQ(map.toOffset(dp(0, 6)), ByteOffset{6});
    EXPECT_EQ(map.toOffset(dp(0, 99)), ByteOffset{6}); // Columns clamp to the row's end.
    EXPECT_EQ(map.toOffset(dp(9, 0)), ByteOffset{12}); // Rows clamp to the last row.
    EXPECT_TRUE(valid(map));
    expectRoundTrips(map);
}

TEST(DisplayMap, BreaksInsideWordsAndKeepsHangingSpaces) {
    const DisplayMap word(snap("abcdefghij"), kCells, columns(4));
    EXPECT_EQ(rowsOf(word), (std::vector<std::string>{"abcd~", "~efgh~", "~ij"}));
    // Spaces may run past the width; the next row starts at the next word.
    const DisplayMap spaces(snap("ab      cd"), kCells, columns(4));
    EXPECT_EQ(rowsOf(spaces), (std::vector<std::string>{"ab      ~", "~cd"}));
    // Width 1: one character per row.
    const DisplayMap narrow(snap("abc"), kCells, columns(0));
    EXPECT_EQ(rowsOf(narrow), (std::vector<std::string>{"a~", "~b~", "~c"}));
    EXPECT_TRUE(valid(word));
    EXPECT_TRUE(valid(spaces));
    EXPECT_TRUE(valid(narrow));
    expectRoundTrips(word);
    expectRoundTrips(narrow);
}

TEST(DisplayMap, WrapsByWidthAndFollowsTheShaper) {
    const MonospaceTextShaper wide(10);
    const MonospaceTextShaper narrow(5);
    DisplayMap map(snap("abcdefg"), wide, WrapSettings{WrapMode::Width, 0, 35});
    EXPECT_EQ(rowsOf(map), (std::vector<std::string>{"abc~", "~def~", "~g"}));
    const std::vector<RowEdit> edit = map.setShaper(narrow);
    EXPECT_EQ(edit, (std::vector<RowEdit>{RowEdit{DisplayRow{0}, DisplayRow{3}, DisplayRow{0}, DisplayRow{1}}}));
    EXPECT_EQ(rowsOf(map), (std::vector<std::string>{"abcdefg"}));
    map.setWrap(WrapSettings{});
    EXPECT_EQ(map.wrapWidth(), 0);
    EXPECT_TRUE(valid(map));
}

TEST(DisplayMap, WideCharacters) {
    const std::string text = std::string(kNichi) + std::string(kHon) + repeat(kNichi, 4) + std::string(kHon);
    const DisplayMap map(snap(text), kCells, columns(5));
    // Two Wide characters (4 cells) per 5-cell row; a break may fall between any two of them.
    ASSERT_EQ(map.rowCount(), 4u);
    EXPECT_EQ(map.rowInfo(DisplayRow{0}).buffer, range(0, 6));
    EXPECT_EQ(map.rowInfo(DisplayRow{3}).buffer, range(18, 21));
    EXPECT_EQ(map.xAt(dp(0, 3)), 2);
    EXPECT_EQ(map.xAt(dp(0, 6)), 4);
    // x 1 is the middle of the first character: from there on the caret goes after it.
    EXPECT_EQ(map.hitTest(DisplayRow{0}, 0).point, dp(0, 0));
    EXPECT_EQ(map.hitTest(DisplayRow{0}, 1).point, dp(0, 3));
    EXPECT_EQ(map.hitTest(DisplayRow{0}, 2).point, dp(0, 3));
    // Past the end of a soft-wrapped row: its end, with Left so the caret stays on it.
    const DisplayHit end = map.hitTest(DisplayRow{0}, 50);
    EXPECT_EQ(end.offset, ByteOffset{6});
    EXPECT_EQ(end.bias, Bias::Left);
    EXPECT_EQ(map.toDisplayPoint(end.offset, end.bias), dp(0, 6));
    EXPECT_TRUE(valid(map));
    expectRoundTrips(map);
}

TEST(DisplayMap, ZeroWidthCharactersStayWithTheirBase) {
    // Six e + combining acute clusters of 3 bytes: four fit in 4 cells, and the accent of the
    // fourth never moves to the next row.
    const DisplayMap map(snap(repeat(std::string("e") + std::string(kAcute), 6)), kCells, columns(4));
    ASSERT_EQ(map.rowCount(), 2u);
    EXPECT_EQ(map.rowInfo(DisplayRow{0}).buffer, range(0, 12));
    EXPECT_EQ(map.rowInfo(DisplayRow{1}).buffer, range(12, 18));
    // Hit tests never land between a base and its mark.
    const MonospaceTextShaper tens(10);
    const DisplayMap measured(snap(repeat(std::string("e") + std::string(kAcute), 3)), tens);
    EXPECT_EQ(measured.hitTest(DisplayRow{0}, 4).point, dp(0, 0));
    EXPECT_EQ(measured.hitTest(DisplayRow{0}, 15).point, dp(0, 6));
    EXPECT_EQ(measured.xAt(dp(0, 3)), 10);
    // A zero-width joiner holds its sequence together: two woman + ZWJ + laptop sequences of 11
    // bytes break between the sequences only.
    const std::string sequence = std::string(kWoman) + std::string(kZwj) + std::string(kLaptop);
    const DisplayMap joined(snap(sequence + sequence), kCells, columns(4));
    ASSERT_EQ(joined.rowCount(), 2u);
    EXPECT_EQ(joined.rowInfo(DisplayRow{0}).buffer, range(0, 11));
    EXPECT_TRUE(valid(map));
    EXPECT_TRUE(valid(joined));
    expectRoundTrips(map);
}

TEST(DisplayMap, TabsAdvanceToTabStopsFromTheRowStart) {
    const DisplayMap off(snap("\tab\tc"), kCells);
    EXPECT_EQ(off.xAt(dp(0, 0)), 0);
    EXPECT_EQ(off.xAt(dp(0, 1)), 4);
    EXPECT_EQ(off.xAt(dp(0, 2)), 5);
    EXPECT_EQ(off.xAt(dp(0, 3)), 6);
    EXPECT_EQ(off.xAt(dp(0, 4)), 8);
    EXPECT_EQ(off.xAt(dp(0, 5)), 9);
    EXPECT_EQ(off.hitTest(DisplayRow{0}, 1).point, dp(0, 0)); // Inside the first half of the tab.
    EXPECT_EQ(off.hitTest(DisplayRow{0}, 3).point, dp(0, 1));
    // Wrapped at 6 cells: the second tab hangs, and the next row lays out from x = 0.
    const DisplayMap wrapped(snap("\tab\tc\td"), kCells, columns(6));
    EXPECT_EQ(rowsOf(wrapped), (std::vector<std::string>{"\tab\t~", "~c\td"}));
    EXPECT_EQ(wrapped.xAt(dp(1, 2)), 4);
    EXPECT_TRUE(valid(wrapped));
    expectRoundTrips(wrapped);
}

TEST(DisplayMap, CrlfLinesWrap) {
    const DisplayMap map(snap("abcdef\r\nxy"), kCells, columns(3));
    EXPECT_EQ(rowsOf(map), (std::vector<std::string>{"abc~", "~def", "xy"}));
    EXPECT_TRUE(map.rowInfo(DisplayRow{1}).endsLine);
    EXPECT_EQ(map.toDisplayPoint(ByteOffset{6}), dp(1, 3));
    EXPECT_EQ(map.toDisplayPoint(ByteOffset{7}), dp(1, 3)); // CRLF-interior.
    EXPECT_EQ(map.toDisplayPoint(ByteOffset{8}), dp(2, 0));
    EXPECT_TRUE(valid(map));
    expectRoundTrips(map);
}

// --- Updates from Document ChangeSets -------------------------------------------------------------

TEST(DisplayMapEdits, WrapFollowsEdits) {
    View view("short line\nanother\n", columns(10));
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"short line", "another", ""}));
    view.replace(10, 10, " grows long now");
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"short line ~", "~grows long ~", "~now", "another", ""}));
    view.replace(5, 21, "");
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"short now", "another", ""}));
    view.replace(5, 5, "\n\n");
    view.replace(0, 13, "a much longer first line");
    view.replace(view.doc.text().size().value(), view.doc.text().size().value(), "tail without a break at all");
    // Undo and redo report precise changes; the map follows them too.
    for (int i = 0; i < 5; ++i) {
        view.apply(view.doc.undo());
    }
    EXPECT_EQ(view.doc.text().text(), "short line\nanother\n");
    for (int i = 0; i < 5; ++i) {
        view.apply(view.doc.redo());
    }
}

TEST(DisplayMapEdits, RowPatchCoversTheChangedRows) {
    View view("one\ntwo\nthree", columns(4));
    const ChangeSet changes = view.doc.replace(range(4, 7), "a longer line", EditContext{}).value();
    const std::vector<RowEdit> patch = view.map.apply(changes, view.doc.text()).value();
    // Line 1 (one row) became four rows at 4 cells; "longer" breaks inside the word.
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"one", "a ~", "~long~", "~er ~", "~line", "thre~", "~e"}));
    ASSERT_EQ(patch.size(), 1u);
    EXPECT_EQ(patch[0].oldFirst, DisplayRow{1});
    EXPECT_EQ(patch[0].oldEnd, DisplayRow{2});
    EXPECT_EQ(patch[0].newFirst, DisplayRow{1});
    EXPECT_EQ(patch[0].newEnd, DisplayRow{view.map.rowCount() - 2});
    view.check();
}

TEST(DisplayMapEdits, MultiEditBatchesAndLineJoins) {
    View view("aaa\nbbb\nccc\nddd\n", columns(3));
    const std::vector<Edit> edits{Edit{range(0, 3), "x\ny"}, Edit{range(8, 11), "a longer one"}};
    view.apply(view.doc.edit(edits, EditContext{}));
    // Join two lines by deleting a break, then split one with a CRLF.
    view.replace(3, 4, "");
    view.replace(2, 2, "\r\n");
    // A lone CR followed by an inserted LF becomes one CRLF break.
    view.replace(1, 1, "\r");
    view.replace(2, 2, "\n");
    // Undoing the batch replays its two regions in one change set.
    for (int i = 0; i < 5; ++i) {
        view.apply(view.doc.undo());
    }
    EXPECT_EQ(view.doc.text().text(), "aaa\nbbb\nccc\nddd\n");
}

TEST(DisplayMapEdits, SeededEditsMatchAFreshMap) {
    // A short smoke over mixed edits, undo and redo with wraps and blocks; each step is compared
    // with a map built fresh over the same text (View::check). Not the M1 exit property test.
    const std::string_view snippets[] = {"a",    "bc", " ",     "  ",   "\t",   "\n",
                                         "\r\n", "\r", "word ", kNichi, kAcute, "long-ish words here "};
    const WrapSettings wraps[] = {WrapSettings{}, columns(3), columns(5), columns(8)};
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        SplitMix64 rng(seed);
        View view("first line\nsecond\r\nthird one here\n", wraps[seed % 4]);
        const auto anyOffset = [&] {
            const Snapshot& text = view.doc.text();
            return text.clip(ByteOffset{rng.below(text.size().value() + 1)}).value();
        };
        for (std::uint64_t i = 0; i < 3; ++i) {
            const BlockPlacement placement = rng.below(2) == 0 ? BlockPlacement::Above : BlockPlacement::Below;
            view.block(anyOffset(), placement, 1 + static_cast<std::uint32_t>(rng.below(2)), 100 + i);
        }
        for (int step = 0; step < 60 && !::testing::Test::HasFailure(); ++step) {
            const std::uint64_t kind = rng.below(10);
            if (kind == 0 && view.doc.undoState().canUndo) {
                view.apply(view.doc.undo());
                continue;
            }
            if (kind == 1 && view.doc.undoState().canRedo) {
                view.apply(view.doc.redo());
                continue;
            }
            const std::uint64_t a = anyOffset();
            std::uint64_t b = kind < 5 ? a : std::max(a, anyOffset()); // Mostly inserts.
            if (b - a > 12) {
                b = view.doc.text().clip(ByteOffset{a + 12}).value();
            }
            const std::string_view insert = kind == 9 ? std::string_view{} : snippets[rng.below(std::size(snippets))];
            if (a != b || !insert.empty()) {
                view.replace(a, b, insert);
            }
            EXPECT_FALSE(::testing::Test::HasFailure()) << "seed " << seed << " step " << step;
        }
    }
}

TEST(DisplayMapEdits, ApplyRejectsMismatchedVersions) {
    Document doc = Document::fromUtf8("text").value();
    DisplayMap map(doc.text(), kCells);
    const Snapshot before = doc.text();
    const ChangeSet first = doc.replace(range(0, 0), "a", EditContext{}).value();
    const ChangeSet second = doc.replace(range(0, 0), "b", EditContext{}).value();
    EXPECT_EQ(map.apply(second, doc.text()).error().code(), ErrorCode::InvalidArgument); // Skips `first`.
    EXPECT_EQ(map.apply(first, before).error().code(), ErrorCode::InvalidArgument);      // Wrong snapshot.
    EXPECT_EQ(map.version(), before.version());
    // An empty change set changes nothing.
    ChangeSet nothing;
    nothing.baseVersion = map.version();
    nothing.version = map.version();
    EXPECT_TRUE(map.apply(nothing, before).value().empty());
    // reset() starts over on any text.
    const std::vector<RowEdit> edit = map.reset(doc.text());
    EXPECT_EQ(edit.size(), 1u);
    EXPECT_EQ(map.version(), doc.version());
    EXPECT_TRUE(valid(map));
}

// --- Block decorations ----------------------------------------------------------------------------

TEST(DisplayMapBlocks, PlacementAndOrder) {
    View view("a\nb\nc");
    const BlockId above = view.block(2, BlockPlacement::Above, 2, 7); // Above line 1.
    view.block(3, BlockPlacement::Below, 1, 8);                       // Below line 1.
    view.block(0, BlockPlacement::Above, 1, 9);                       // Above line 0.
    view.block(5, BlockPlacement::Below, 1, 10);                      // Below the last line.
    // Below line 0 shares its boundary with the Above-line-1 block and goes first, although it is
    // created later.
    view.block(1, BlockPlacement::Below, 1, 11);
    EXPECT_EQ(rowsOf(view.map),
              (std::vector<std::string>{"#9.0", "a", "#11.0", "#7.0", "#7.1", "b", "#8.0", "c", "#10.0"}));
    EXPECT_EQ(view.map.rowCount(), 9u);
    EXPECT_EQ(view.map.blockCount(), 5u);
    EXPECT_EQ(view.map.toDisplayPoint(ByteOffset{2}), dp(5, 0));
    EXPECT_EQ(view.map.toOffset(dp(3, 0)), ByteOffset{2}); // An Above block maps to its line's start.
    EXPECT_EQ(view.map.toOffset(dp(6, 0)), ByteOffset{3}); // A Below block to its line's end.
    const BlockInfo info = view.map.blockInfo(above).value();
    EXPECT_EQ(info.row, DisplayRow{3});
    EXPECT_EQ(info.line, LineIndex{1});
    EXPECT_EQ(info.decoration, (BlockDecoration{ByteOffset{2}, BlockPlacement::Above, 2, 7}));
    const DisplayRowInfo blockRow = view.map.rowInfo(DisplayRow{4});
    EXPECT_EQ(blockRow.kind, RowKind::Block);
    EXPECT_EQ(blockRow.block, above);
    EXPECT_EQ(blockRow.blockRow, 1u);
    EXPECT_EQ(blockRow.line, LineIndex{1});
    const DisplayHit hit = view.map.hitTest(DisplayRow{4}, 3);
    EXPECT_EQ(hit.block, above);
    EXPECT_EQ(hit.offset, ByteOffset{2});
    EXPECT_EQ(view.map.xAt(dp(4, 0)), 0);

    ASSERT_TRUE(view.map.removeBlock(above).has_value());
    std::erase(view.blocks, above);
    view.check();
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"#9.0", "a", "#11.0", "b", "#8.0", "c", "#10.0"}));
    EXPECT_EQ(view.map.removeBlock(above).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(view.map.blockInfo(above).error().code(), ErrorCode::NotFound);
}

TEST(DisplayMapBlocks, AddErrors) {
    DisplayMap map(snap("a\xC3\xA9"), kCells);
    EXPECT_EQ(map.addBlock(BlockDecoration{ByteOffset{0}, BlockPlacement::Above, 0, 0}).error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(map.addBlock(BlockDecoration{ByteOffset{4}, BlockPlacement::Above, 1, 0}).error().code(),
              ErrorCode::OutOfRange);
    EXPECT_EQ(map.addBlock(BlockDecoration{ByteOffset{2}, BlockPlacement::Above, 1, 0}).error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(map.blockCount(), 0u);
    EXPECT_TRUE(valid(map));
}

TEST(DisplayMapBlocks, BlocksFollowEdits) {
    View view("l0\nl1\nl2\nl3");
    view.block(6, BlockPlacement::Above, 1, 1); // Above l2.
    view.block(5, BlockPlacement::Below, 1, 2); // Below l1.
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"l0", "l1", "#2.0", "#1.0", "l2", "l3"}));
    view.replace(0, 0, "x\n");
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"x", "l0", "l1", "#2.0", "#1.0", "l2", "l3"}));
    view.replace(10, 10, "!!"); // Typing at the end of l2.
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"x", "l0", "l1", "#2.0", "#1.0", "l2!!", "l3"}));
    view.replace(8, 13, ""); // Delete "l2!!\n": the Above block goes to the line at the deletion point.
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"x", "l0", "l1", "#2.0", "#1.0", "l3"}));
    view.replace(7, 7, "\n"); // Enter at the end of l1: the Below block stays with l1.
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"x", "l0", "l1", "#2.0", "", "#1.0", "l3"}));
    view.replace(0, 6, ""); // Delete across the first lines: "x\nl0\nl" goes.
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"1", "#2.0", "", "#1.0", "l3"}));
    // Replace everything: the Below block collapses to the start, the Above block to the end of the
    // new text, and both end up attached to its only line.
    view.replace(0, view.doc.text().size().value(), "all new");
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"#1.0", "all new", "#2.0"}));
    for (int i = 0; i < 6; ++i) {
        view.apply(view.doc.undo());
    }
    EXPECT_EQ(view.doc.text().text(), "l0\nl1\nl2\nl3");
}

TEST(DisplayMapBlocks, AdjacentRegionsShareABoundary) {
    // One batch edits lines 0 and 1, two touching regions; the blocks at their shared boundary
    // (below line 0, above line 1) are placed again by the region of their own line.
    View view("aa\nbb\ncc", columns(3));
    view.block(3, BlockPlacement::Above, 1, 1); // Above "bb".
    view.block(2, BlockPlacement::Below, 1, 2); // Below "aa".
    const std::vector<Edit> edits{Edit{range(0, 2), "AAA"}, Edit{range(3, 5), "BBBB"}};
    view.apply(view.doc.edit(edits, EditContext{}));
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"AAA", "#2.0", "#1.0", "BBB~", "~B", "cc"}));
    view.apply(view.doc.undo());
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"aa", "#2.0", "#1.0", "bb", "cc"}));
}

TEST(DisplayMapBlocks, BlocksWithSoftWrap) {
    View view("first\nsecond\nthird", columns(6));
    view.block(12, BlockPlacement::Below, 2, 5); // Below "second".
    view.block(13, BlockPlacement::Above, 1, 6); // Above "third".
    // Type at the end of "second" until it wraps: the Below block stays after its last row.
    for (int i = 0; i < 8; ++i) {
        view.replace(12 + static_cast<std::uint64_t>(i), 12 + static_cast<std::uint64_t>(i), i == 3 ? " " : "x");
    }
    EXPECT_EQ(rowsOf(view.map),
              (std::vector<std::string>{"first", "second~", "~xxx ~", "~xxxx", "#5.0", "#5.1", "#6.0", "third"}));
    // Toggling the wrap keeps the blocks in place.
    view.map.setWrap(WrapSettings{});
    view.check();
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"first", "secondxxx xxxx", "#5.0", "#5.1", "#6.0", "third"}));
    view.map.setWrap(columns(3));
    view.check();
}

// --- Caret movement -----------------------------------------------------------------------------

TEST(DisplayMapMovement, VerticalMovesKeepTheGoalX) {
    const DisplayMap map(snap("aaaa bbbb cccc\nxy"), kCells, columns(5));
    EXPECT_EQ(rowsOf(map), (std::vector<std::string>{"aaaa ~", "~bbbb ~", "~cccc", "xy"}));
    VerticalMove move = map.moveVertically(ByteOffset{2}, Bias::Right, std::nullopt, 1);
    EXPECT_EQ(move.offset, ByteOffset{7});
    EXPECT_EQ(move.goalX, 2);
    move = map.moveVertically(move.offset, move.bias, move.goalX, 1);
    EXPECT_EQ(move.offset, ByteOffset{12});
    move = map.moveVertically(move.offset, move.bias, move.goalX, 1);
    EXPECT_EQ(move.offset, ByteOffset{17}); // "xy" is shorter: its end.
    EXPECT_EQ(move.point, dp(3, 2));
    move = map.moveVertically(move.offset, move.bias, move.goalX, 1); // Past the last row: stays.
    EXPECT_EQ(move.offset, ByteOffset{17});
    move = map.moveVertically(move.offset, move.bias, move.goalX, -3);
    EXPECT_EQ(move.offset, ByteOffset{2});
    // From the end of a soft-wrapped row (Left) to the end of the next one, still on that row.
    move = map.moveVertically(ByteOffset{5}, Bias::Left, std::nullopt, 1);
    EXPECT_EQ(move.goalX, 5);
    EXPECT_EQ(move.offset, ByteOffset{10});
    EXPECT_EQ(move.bias, Bias::Left);
    EXPECT_EQ(move.point, dp(1, 5));
    // Page-sized moves clamp to the first and last rows.
    EXPECT_EQ(map.moveVertically(ByteOffset{16}, Bias::Right, std::nullopt, -100).offset, ByteOffset{1});
    EXPECT_EQ(map.moveVertically(ByteOffset{1}, Bias::Right, std::nullopt, 100).offset, ByteOffset{16});
}

TEST(DisplayMapMovement, VerticalMovesSkipBlocks) {
    View view("ab\ncd\nef");
    view.block(3, BlockPlacement::Above, 3, 1); // Three rows above "cd".
    view.block(0, BlockPlacement::Above, 1, 2); // One row above "ab".
    EXPECT_EQ(rowsOf(view.map), (std::vector<std::string>{"#2.0", "ab", "#1.0", "#1.1", "#1.2", "cd", "ef"}));
    VerticalMove move = view.map.moveVertically(ByteOffset{1}, Bias::Right, std::nullopt, 1);
    EXPECT_EQ(move.offset, ByteOffset{4});
    EXPECT_EQ(move.point, dp(5, 1));
    move = view.map.moveVertically(move.offset, move.bias, move.goalX, -1);
    EXPECT_EQ(move.offset, ByteOffset{1});
    // Up from the first text row lands in the block above it, so it goes back down to that row.
    move = view.map.moveVertically(ByteOffset{1}, Bias::Right, std::nullopt, -1);
    EXPECT_EQ(move.offset, ByteOffset{1});
    EXPECT_EQ(move.point, dp(1, 1));
}

} // namespace
