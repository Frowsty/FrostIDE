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
#include <array>
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

TEST(DisplayFold, NestedRangesAndWrapping) {
    const std::string source = "head {\n outer {\n inner\n }\n}\ntail\n";
    DisplayMap map(snap(source), kCells);
    ASSERT_TRUE(map.setFolds({range(6, 27), range(15, 25)}));
    ASSERT_EQ(map.folds().size(), 2U);
    ASSERT_EQ(map.rowCount(), 3U);
    const auto row = map.rowInfo(DisplayRow{0});
    ASSERT_EQ(row.segments.size(), 2U);
    EXPECT_EQ(row.segments[1].kind, frost::text::SegmentKind::FoldPlaceholder);
    EXPECT_EQ(row.segments[1].text, "...");
    EXPECT_EQ(map.toDisplayPoint(ByteOffset{15}, Bias::Left), dp(0, 6));
    EXPECT_EQ(map.toDisplayPoint(ByteOffset{15}, Bias::Right), dp(0, 9));
    EXPECT_EQ(map.toOffset(dp(0, 6)), ByteOffset{6});
    EXPECT_EQ(map.toOffset(dp(0, 7)), ByteOffset{6});
    EXPECT_EQ(map.toOffset(dp(0, 8)), ByteOffset{27});
    EXPECT_EQ(map.toOffset(dp(0, 9)), ByteOffset{27});
    ASSERT_TRUE(map.checkInvariants());
    (void)map.setWrap(columns(5));
    ASSERT_TRUE(map.checkInvariants());
    ASSERT_TRUE(map.setFolds({range(15, 25)}));
    ASSERT_TRUE(map.checkInvariants());
}

TEST(DisplayFold, RejectsCrossingAndSplitCharacters) {
    DisplayMap map(snap("ab\r\n\xC3\xA5 cdefgh"), kCells);
    EXPECT_FALSE(map.setFolds({range(0, 6), range(4, 9)}));
    EXPECT_FALSE(map.setFolds({range(3, 8)}));
    EXPECT_FALSE(map.setFolds({range(5, 9)}));
    EXPECT_TRUE(map.folds().empty());
}

TEST(DisplayFold, EditsAndUndoMatchFreshProjection) {
    Document doc = Document::fromUtf8("before\n{\n  alpha\n  beta\n}\nafter\n").value();
    DisplayMap map(doc.text(), kCells, columns(8));
    ASSERT_TRUE(map.setFolds({range(8, 25), range(12, 23)}));
    const auto compare = [&] {
        DisplayMap fresh(doc.text(), kCells, columns(8));
        ASSERT_TRUE(fresh.setFolds(map.folds()));
        ASSERT_EQ(map.rowCount(), fresh.rowCount());
        ASSERT_TRUE(map.checkInvariants());
        for (std::uint32_t i = 0; i < map.rowCount(); ++i) {
            const auto a = map.rowInfo(DisplayRow{i});
            const auto b = fresh.rowInfo(DisplayRow{i});
            ASSERT_EQ(a.buffer, b.buffer);
            ASSERT_EQ(a.segments.size(), b.segments.size());
            for (std::size_t j = 0; j < a.segments.size(); ++j) {
                EXPECT_EQ(a.segments[j].kind, b.segments[j].kind);
                EXPECT_EQ(a.segments[j].buffer, b.segments[j].buffer);
                EXPECT_EQ(a.segments[j].text, b.segments[j].text);
            }
        }
    };
    auto edit = doc.replace(range(1, 1), "X\n", EditContext{});
    ASSERT_TRUE(edit);
    ASSERT_TRUE(map.apply(*edit, doc.text()));
    compare();
    auto hidden = doc.replace(range(16, 18), "changed\ntext", EditContext{});
    ASSERT_TRUE(hidden);
    ASSERT_TRUE(map.apply(*hidden, doc.text()));
    compare();
    auto undo = doc.undo();
    ASSERT_TRUE(undo);
    ASSERT_TRUE(map.apply(*undo, doc.text()));
    compare();
}

TEST(DisplayFold, ProjectionMatchesPlainTextAcrossWrapWidths) {
    const std::string source = "a\xCC\x81 x\r\n{\n  one\n  two\n}\r\ntail\n";
    const std::string projected = source.substr(0, 10) + "..." + source.substr(22);
    for (std::uint32_t width = 1; width <= 16; ++width) {
        DisplayMap actual(snap(source), kCells, columns(width));
        ASSERT_TRUE(actual.setFolds({range(10, 22)}));
        DisplayMap expected(snap(projected), kCells, columns(width));
        ASSERT_TRUE(actual.checkInvariants());
        ASSERT_EQ(actual.rowCount(), expected.rowCount()) << width;
        for (std::uint32_t i = 0; i < actual.rowCount(); ++i) {
            const auto row = actual.rowInfo(DisplayRow{i});
            std::string text;
            for (const auto& segment : row.segments) {
                text += segment.kind == frost::text::SegmentKind::Text ? actual.text().text(segment.buffer)
                                                                       : std::string(segment.text);
            }
            EXPECT_LE(row.buffer.begin, row.buffer.end);
            EXPECT_EQ(text, expected.text().text(expected.rowInfo(DisplayRow{i}).buffer)) << width << ":" << i;
        }
    }
}

TEST(DisplayFold, EveryAsciiRangeMatchesPlainProjection) {
    const std::string source = "aa\r\nbb\nccc\rdd\nend";
    for (std::size_t begin = 0; begin < source.size(); ++begin) {
        for (std::size_t end = begin + 1; end <= source.size(); ++end) {
            if ((begin && begin < source.size() && source[begin - 1] == '\r' && source[begin] == '\n') ||
                (end && end < source.size() && source[end - 1] == '\r' && source[end] == '\n'))
                continue;
            const std::string projected = source.substr(0, begin) + "..." + source.substr(end);
            for (const auto width : {3U, 7U}) {
                DisplayMap actual(snap(source), kCells, columns(width));
                ASSERT_TRUE(actual.setFolds({range(begin, end)}));
                DisplayMap expected(snap(projected), kCells, columns(width));
                ASSERT_TRUE(actual.checkInvariants()) << begin << "," << end;
                ASSERT_EQ(actual.rowCount(), expected.rowCount()) << begin << "," << end;
                for (std::uint32_t i = 0; i < actual.rowCount(); ++i) {
                    const auto row = actual.rowInfo(DisplayRow{i});
                    std::string text;
                    for (const auto& segment : row.segments) {
                        text += segment.kind == frost::text::SegmentKind::Text ? actual.text().text(segment.buffer)
                                                                               : std::string(segment.text);
                    }
                    EXPECT_EQ(text, expected.text().text(expected.rowInfo(DisplayRow{i}).buffer))
                        << begin << "," << end << "," << width << "," << i;
                }
            }
        }
    }
}

TEST(DisplayFold, DisjointAndNestedFoldsShareLineProjection) {
    const std::string source = "a\nb\nc\nd\ne\nf\ng\n";
    DisplayMap map(snap(source), kCells, columns(4));
    ASSERT_TRUE(map.setFolds({range(1, 4), range(2, 3), range(7, 10)}));
    DisplayMap expected(snap("a...c\nd...f\ng\n"), kCells, columns(4));
    ASSERT_TRUE(map.checkInvariants());
    ASSERT_EQ(map.rowCount(), expected.rowCount());
    for (std::uint32_t i = 0; i < map.rowCount(); ++i) {
        std::string shown;
        for (const auto& segment : map.rowInfo(DisplayRow{i}).segments) {
            shown += segment.kind == frost::text::SegmentKind::Text ? map.text().text(segment.buffer)
                                                                    : std::string(segment.text);
        }
        EXPECT_EQ(shown, expected.text().text(expected.rowInfo(DisplayRow{i}).buffer));
    }
}

TEST(DisplayFold, NestedEndpointIndexUsesStrictContainment) {
    DisplayMap map(snap("0123456789abcdefghijkl"), kCells);
    ASSERT_TRUE(map.setFolds({range(1, 10), range(2, 9), range(3, 8), range(10, 16), range(11, 15)}));
    EXPECT_EQ(map.foldsContaining(ByteOffset{0}), std::vector<ByteRange>{});
    EXPECT_EQ(map.foldsContaining(ByteOffset{1}), std::vector<ByteRange>{});
    EXPECT_EQ(map.foldsContaining(ByteOffset{2}), (std::vector<ByteRange>{range(1, 10)}));
    EXPECT_EQ(map.foldsContaining(ByteOffset{4}), (std::vector<ByteRange>{range(3, 8), range(2, 9), range(1, 10)}));
    EXPECT_EQ(map.foldsContaining(ByteOffset{8}), (std::vector<ByteRange>{range(2, 9), range(1, 10)}));
    EXPECT_EQ(map.foldsContaining(ByteOffset{10}), std::vector<ByteRange>{});
    EXPECT_EQ(map.foldsContaining(ByteOffset{12}), (std::vector<ByteRange>{range(11, 15), range(10, 16)}));
    ASSERT_TRUE(map.checkInvariants());
}

TEST(DisplayFold, IncrementalAddRemovePromotesNestedFold) {
    const auto text = snap("a\nb\nc\nd\ne\n");
    DisplayMap map(text, kCells, columns(4));
    auto outer = map.addFold(range(1, 8));
    ASSERT_TRUE(outer);
    auto inner = map.addFold(range(3, 6));
    ASSERT_TRUE(inner);
    EXPECT_TRUE(map.checkInvariants());
    EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
    ASSERT_TRUE(map.removeFold(*outer));
    EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
    EXPECT_TRUE(map.checkInvariants());
    EXPECT_EQ(map.folds(), (std::vector<ByteRange>{range(3, 6)}));
    DisplayMap fresh(text, kCells, columns(4));
    ASSERT_TRUE(fresh.setFolds({range(3, 6)}));
    EXPECT_EQ(map.rowCount(), fresh.rowCount());
    ASSERT_TRUE(map.removeFold(*inner));
    EXPECT_EQ(map.rowCount(), text.lineCount());
    EXPECT_TRUE(map.checkInvariants());
    EXPECT_FALSE(map.removeFold(*inner));
}

TEST(DisplayFold, IncrementalEndpointsOrderAndRejectCrossings) {
    DisplayMap map(snap("0123456789abcdefghijkl"), kCells);
    const auto a = map.addFold(range(2, 12));
    const auto b = map.addFold(range(2, 10));
    const auto c = map.addFold(range(4, 10));
    const auto d = map.addFold(range(12, 18));
    ASSERT_TRUE(a && b && c && d);
    EXPECT_FALSE(map.addFold(range(2, 12)));
    EXPECT_FALSE(map.addFold(range(8, 14)));
    EXPECT_TRUE(map.checkInvariants());
    EXPECT_EQ(map.foldsContaining(ByteOffset{5}), (std::vector<ByteRange>{range(4, 10), range(2, 10), range(2, 12)}));
    ASSERT_TRUE(map.removeFold(*b));
    EXPECT_TRUE(map.checkInvariants());
    EXPECT_EQ(map.foldsContaining(ByteOffset{5}), (std::vector<ByteRange>{range(4, 10), range(2, 12)}));
    ASSERT_TRUE(map.removeFold(*a));
    EXPECT_TRUE(map.checkInvariants());
    EXPECT_TRUE(map.hasFoldBoundary(ByteOffset{12}, true));
    const auto added = map.addFold(range(19, 21));
    ASSERT_TRUE(added);
    EXPECT_GT(added->value, d->value);
    ASSERT_TRUE(map.removeFold(*d));
    ASSERT_TRUE(map.removeFold(*added));
    EXPECT_TRUE(map.checkInvariants());
}

TEST(DisplayFold, SurvivingIdsRemainStableAcrossBoundaryDeletion) {
    Document doc = Document::fromUtf8("0123456789abcdef").value();
    DisplayMap map(doc.text(), kCells);
    const auto removed = map.addFold(range(1, 4));
    const auto survivor = map.addFold(range(8, 12));
    ASSERT_TRUE(removed && survivor);
    const auto edit = doc.replace(range(1, 5), "", EditContext{});
    ASSERT_TRUE(edit);
    ASSERT_TRUE(map.apply(*edit, doc.text()));
    EXPECT_FALSE(map.removeFold(*removed));
    ASSERT_TRUE(map.removeFold(*survivor));
    EXPECT_TRUE(map.checkInvariants());
}

TEST(DisplayFold, ReplacedOrResetFoldsDoNotReuseIds) {
    const Snapshot text = snap("0123456789");
    DisplayMap map(text, kCells);
    const auto old = map.addFold(range(1, 4));
    ASSERT_TRUE(old);
    ASSERT_TRUE(map.setFolds({range(2, 5)}));
    const auto replacement = map.firstFoldStartingIn(range(2, 2));
    ASSERT_TRUE(replacement);
    EXPECT_GT(replacement->id.value, old->value);
    EXPECT_FALSE(map.removeFold(*old));
    (void)map.reset(text);
    const auto next = map.addFold(range(3, 6));
    ASSERT_TRUE(next);
    EXPECT_GT(next->value, replacement->id.value);
    EXPECT_FALSE(map.removeFold(replacement->id));
}

TEST(DisplayFold, RemovingOuterFoldSkipsDeepDescendants) {
    DisplayMap map(snap(std::string(600, 'x')), kCells);
    std::vector<ByteRange> folds;
    for (std::uint64_t i = 0; i < 256; ++i)
        folds.push_back(range(i + 1, 599 - i));
    ASSERT_TRUE(map.setFolds(folds));
    const auto outer = map.firstFoldStartingIn(range(1, 1));
    ASSERT_TRUE(outer);
    ASSERT_TRUE(map.removeFold(outer->id));
    EXPECT_LE(map.foldUpdateStatsForTesting().visitedFolds, 3U);
    EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
    EXPECT_TRUE(map.checkInvariants());
    EXPECT_EQ(map.foldsContaining(ByteOffset{300}).size(), 255U);
}

TEST(DisplayFold, IncrementalTogglesKeepBlockAnchors) {
    DisplayMap map(snap("a\nb\nc\nd\n"), kCells, columns(4));
    const auto block = map.addBlock({ByteOffset{4}, BlockPlacement::Below, 2, 9});
    ASSERT_TRUE(block);
    auto fold = map.addFold(range(1, 6));
    ASSERT_TRUE(fold);
    EXPECT_TRUE(map.checkInvariants());
    EXPECT_TRUE(map.blockInfo(*block));
    ASSERT_TRUE(map.removeFold(*fold));
    EXPECT_TRUE(map.checkInvariants());
    EXPECT_TRUE(map.blockInfo(*block));
}

TEST(DisplayFold, BlocksAtAndInsideFoldBoundariesRemainAttached) {
    const auto text = snap("a\nb\nc\nd\ne\n");
    for (std::uint64_t at = 0; at <= text.size().value(); ++at) {
        for (const auto placement : {BlockPlacement::Above, BlockPlacement::Below}) {
            DisplayMap map(text, kCells, columns(4));
            const auto block = map.addBlock({ByteOffset{at}, placement, 1, 17});
            ASSERT_TRUE(block);
            const auto fold = map.addFold(range(2, 8));
            ASSERT_TRUE(fold) << at;
            ASSERT_TRUE(map.checkInvariants()) << at;
            ASSERT_TRUE(map.blockInfo(*block));
            ASSERT_TRUE(map.removeFold(*fold)) << at;
            ASSERT_TRUE(map.checkInvariants()) << at;
            EXPECT_EQ(map.blockInfo(*block)->decoration.at, ByteOffset{at});
        }
    }
}

TEST(DisplayFold, BlocksFollowEditsWhileFolded) {
    Document doc = Document::fromUtf8("one\n{\ninside\n}\nend\n").value();
    DisplayMap map(doc.text(), kCells, columns(6));
    ASSERT_TRUE(map.setFolds({range(5, 14)}));
    ASSERT_TRUE(map.addBlock({ByteOffset{9}, BlockPlacement::Below, 2, 17}));
    ASSERT_TRUE(map.checkInvariants());
    auto edit = doc.replace(range(1, 1), "\nnew", EditContext{});
    ASSERT_TRUE(edit);
    ASSERT_TRUE(map.apply(*edit, doc.text()));
    ASSERT_TRUE(map.checkInvariants());
}

TEST(DisplayFold, HiddenInteriorEditDoesNotRewrap) {
    Document doc = Document::fromUtf8("before {\n  hidden\n}\nafter\n").value();
    DisplayMap map(doc.text(), kCells, columns(5));
    ASSERT_TRUE(map.setFolds({range(8, 20)}));
    auto edit = doc.replace(range(12, 15), "replacement\nwith lines", EditContext{});
    ASSERT_TRUE(edit);
    const auto patch = map.apply(*edit, doc.text());
    ASSERT_TRUE(patch);
    EXPECT_TRUE(patch->empty());
    EXPECT_TRUE(map.checkInvariants());
}

TEST(DisplayFold, HiddenNestedBoundaryEditKeepsOuterProjection) {
    Document doc = Document::fromUtf8("0123456789abcdef").value();
    DisplayMap map(doc.text(), kCells, columns(5));
    const auto outer = map.addFold(range(1, 14));
    const auto inner = map.addFold(range(3, 8));
    ASSERT_TRUE(outer && inner);
    const auto change = doc.replace(range(3, 4), "X", EditContext{});
    ASSERT_TRUE(change);
    const auto patch = map.apply(*change, doc.text());
    ASSERT_TRUE(patch);
    EXPECT_TRUE(patch->empty());
    EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
    ASSERT_TRUE(map.removeFold(*outer));
    ASSERT_TRUE(map.removeFold(*inner));
    EXPECT_TRUE(map.checkInvariants());
}

TEST(DisplayFold, HiddenNestedDeletionDoesNotRewrapOuterFold) {
    Document doc = Document::fromUtf8("0123456789abcdef").value();
    DisplayMap map(doc.text(), kCells, columns(5));
    const auto outer = map.addFold(range(1, 14));
    const auto inner = map.addFold(range(3, 8));
    ASSERT_TRUE(outer && inner);
    const auto change = doc.replace(range(3, 8), "", EditContext{});
    ASSERT_TRUE(change);
    const auto patch = map.apply(*change, doc.text());
    ASSERT_TRUE(patch);
    EXPECT_TRUE(patch->empty());
    ASSERT_TRUE(map.removeFold(*outer));
    EXPECT_FALSE(map.removeFold(*inner));
    EXPECT_TRUE(map.checkInvariants());
}

TEST(DisplayFold, DistantFoldsAreNotEnumeratedForPrefixEdit) {
    std::string source = "head\n";
    std::vector<ByteRange> folds;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        const auto begin = static_cast<std::uint64_t>(source.size());
        source += "abcde\n";
        folds.push_back(range(begin, begin + 3));
    }
    Document doc = Document::fromUtf8(source).value();
    DisplayMap map(doc.text(), kCells);
    ASSERT_TRUE(map.setFolds(folds));
    auto edit = doc.replace(range(1, 1), "X", EditContext{});
    ASSERT_TRUE(edit);
    ASSERT_TRUE(map.apply(*edit, doc.text()));
    EXPECT_EQ(map.foldUpdateStatsForTesting().replacedRuns, 1U);
    EXPECT_EQ(map.foldUpdateStatsForTesting().visitedFolds, 0U);
    EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
    EXPECT_TRUE(map.hasFoldBoundary(ByteOffset{6}, true));
    EXPECT_TRUE(map.foldStartsIn(range(6, 10)));
    EXPECT_EQ(map.foldsContaining(ByteOffset{7}), (std::vector<ByteRange>{range(6, 9)}));
    EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
    EXPECT_TRUE(map.checkInvariants());
}

TEST(DisplayFold, BoundaryQueriesSkipOppositeKindAtCrowdedEndpoint) {
    DisplayMap map(snap(std::string(220, 'x')), kCells);
    std::vector<ByteRange> folds;
    for (std::uint64_t i = 1; i <= 100; ++i)
        folds.push_back(range(i, 200));
    folds.push_back(range(200, 210));
    ASSERT_TRUE(map.setFolds(folds));
    ASSERT_TRUE(map.hasFoldBoundary(ByteOffset{200}, true));
    ASSERT_TRUE(map.hasFoldBoundary(ByteOffset{200}, false));
    ASSERT_TRUE(map.foldStartsIn(range(200, 200)));
    ASSERT_TRUE(map.firstFoldStartingIn(range(200, 200)));
    EXPECT_EQ(map.firstFoldStartingIn(range(200, 200))->range, range(200, 210));
    EXPECT_EQ(map.foldAtBoundary(ByteOffset{200}, true)->range, range(200, 210));
    EXPECT_FALSE(map.hasFoldBoundary(ByteOffset{199}, true));
    EXPECT_FALSE(map.foldStartsIn(range(199, 199)));
}

TEST(DisplayFold, BoundaryQueryIncludesBothEndsAndAdjacentFolds) {
    Document doc = Document::fromUtf8("abcdefghijkl").value();
    DisplayMap map(doc.text(), kCells, columns(80));
    ASSERT_TRUE(map.setFolds({range(1, 5), range(2, 4), range(5, 8)}));
    const auto atFive = map.foldInfosWithBoundaryIn(range(5, 5));
    ASSERT_EQ(atFive.size(), 2U);
    EXPECT_EQ(atFive[0].range, range(1, 5));
    EXPECT_EQ(atFive[1].range, range(5, 8));
    const auto inside = map.foldInfosWithBoundaryIn(range(2, 4));
    ASSERT_EQ(inside.size(), 2U);
    EXPECT_EQ(inside[0].range, range(2, 4));
    EXPECT_EQ(inside[1].range, range(2, 4));
}

TEST(DisplayFold, DistantBatchEditsKeepSeparatePatches) {
    std::string source = "prefix\n";
    std::vector<ByteRange> folds;
    for (std::uint64_t i = 0; i < 100; ++i) {
        const auto begin = static_cast<std::uint64_t>(source.size());
        source += "abcde\n";
        folds.push_back(range(begin, begin + 3));
    }
    source += "suffix\n";
    Document doc = Document::fromUtf8(source).value();
    DisplayMap map(doc.text(), kCells, columns(8));
    ASSERT_TRUE(map.setFolds(folds));
    const std::array edits{Edit{range(2, 2), "A"}, Edit{range(source.size() - 3, source.size() - 3), "B"}};
    const auto change = doc.edit(edits, EditContext{});
    ASSERT_TRUE(change);
    const auto patches = map.apply(*change, doc.text());
    ASSERT_TRUE(patches);
    EXPECT_EQ(patches->size(), 2U);
    EXPECT_EQ(map.foldUpdateStatsForTesting().replacedRuns, 2U);
    EXPECT_EQ(map.foldUpdateStatsForTesting().visitedFolds, 0U);
    EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
    EXPECT_TRUE(map.checkInvariants());
}

TEST(DisplayFold, DeletingManyNestedFoldsRewritesOneEventInterval) {
    Document doc = Document::fromUtf8(std::string(500, 'x')).value();
    DisplayMap map(doc.text(), kCells, columns(8));
    std::vector<ByteRange> folds;
    for (std::uint64_t i = 1; i <= 100; ++i)
        folds.push_back(range(5, 300 - i));
    folds.push_back(range(400, 450));
    ASSERT_TRUE(map.setFolds(folds));
    const auto survivor = map.firstFoldStartingIn(range(400, 400));
    ASSERT_TRUE(survivor);
    const auto change = doc.replace(range(0, 310), "", EditContext{});
    ASSERT_TRUE(change);
    ASSERT_TRUE(map.apply(*change, doc.text()));
    EXPECT_EQ(map.foldUpdateStatsForTesting().eventIntervalsRewritten, 1U);
    EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
    ASSERT_TRUE(map.checkInvariants());
    const auto after = map.firstFoldStartingIn(range(90, 90));
    ASSERT_TRUE(after);
    EXPECT_EQ(after->id, survivor->id);
    EXPECT_EQ(after->range, range(90, 140));
}

TEST(DisplayFold, HiddenSharedStartsCoalesceWithoutRepeatedGroupScans) {
    Document doc = Document::fromUtf8(std::string(500, 'x')).value();
    DisplayMap map(doc.text(), kCells, columns(8));
    std::vector<ByteRange> folds{range(1, 400)};
    for (std::uint64_t i = 0; i < 100; ++i)
        folds.push_back(range(50, 299 - i));
    ASSERT_TRUE(map.setFolds(folds));
    const auto outermostChild = map.firstFoldStartingIn(range(50, 50));
    ASSERT_TRUE(outermostChild);
    const auto change = doc.replace(range(190, 310), "Q", EditContext{});
    ASSERT_TRUE(change);
    const auto patches = map.apply(*change, doc.text());
    ASSERT_TRUE(patches);
    EXPECT_TRUE(patches->empty());
    EXPECT_LE(map.foldUpdateStatsForTesting().eventItemsScanned, 200U);
    EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
    ASSERT_TRUE(map.checkInvariants());
    const auto child = map.firstFoldStartingIn(range(50, 50));
    ASSERT_TRUE(child);
    EXPECT_EQ(child->id, outermostChild->id);
    EXPECT_EQ(child->range, range(50, 190));
    EXPECT_EQ(map.folds().size(), 2U);
}

TEST(DisplayFold, SharedHiddenEndsAreRewrittenOncePerEndpointGroup) {
    Document doc = Document::fromUtf8(std::string(500, 'x')).value();
    DisplayMap map(doc.text(), kCells, columns(8));
    std::vector<ByteRange> folds{range(1, 400)};
    for (std::uint64_t i = 0; i < 100; ++i)
        folds.push_back(range(50 + i, 300));
    folds.push_back(range(420, 450));
    ASSERT_TRUE(map.setFolds(folds));
    const auto outer = map.firstFoldStartingIn(range(1, 1));
    const auto distant = map.firstFoldStartingIn(range(420, 420));
    ASSERT_TRUE(outer && distant);
    const auto change = doc.replace(range(299, 301), "Q", EditContext{});
    ASSERT_TRUE(change);
    const auto patches = map.apply(*change, doc.text());
    ASSERT_TRUE(patches);
    EXPECT_TRUE(patches->empty());
    EXPECT_LE(map.foldUpdateStatsForTesting().eventItemsScanned, 202U);
    EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
    ASSERT_TRUE(map.checkInvariants());
    ASSERT_TRUE(map.firstFoldStartingIn(range(1, 1)));
    EXPECT_EQ(map.firstFoldStartingIn(range(1, 1))->id, outer->id);
    ASSERT_TRUE(map.firstFoldStartingIn(range(419, 419)));
    EXPECT_EQ(map.firstFoldStartingIn(range(419, 419))->id, distant->id);
    EXPECT_EQ(map.folds().size(), folds.size());
}

TEST(DisplayFold, ReplacingSharedOpeningMovesTheGroupWithoutRecreatingFolds) {
    for (const bool hidden : {false, true}) {
        Document doc = Document::fromUtf8(std::string(500, 'x')).value();
        DisplayMap map(doc.text(), kCells, columns(8));
        std::vector<ByteRange> folds;
        if (hidden)
            folds.push_back(range(1, 320));
        for (std::uint64_t i = 0; i < 100; ++i)
            folds.push_back(range(50, 300 - i));
        folds.push_back(range(350, 400));
        ASSERT_TRUE(map.setFolds(folds));
        const auto first = map.firstFoldStartingIn(range(50, 50));
        const auto distant = map.firstFoldStartingIn(range(350, 350));
        ASSERT_TRUE(first && distant);
        const auto change = doc.replace(range(50, 52), "A\nB", EditContext{});
        ASSERT_TRUE(change);
        ASSERT_TRUE(map.apply(*change, doc.text()));
        EXPECT_EQ(map.foldUpdateStatsForTesting().eventIntervalsRewritten, 1U);
        EXPECT_EQ(map.foldUpdateStatsForTesting().visitedFolds, 0U);
        EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
        ASSERT_TRUE(map.checkInvariants());
        const auto moved = map.firstFoldStartingIn(range(53, 53));
        const auto shifted = map.firstFoldStartingIn(range(351, 351));
        ASSERT_TRUE(moved && shifted);
        EXPECT_EQ(moved->id, first->id);
        EXPECT_EQ(shifted->id, distant->id);
        DisplayMap fresh(doc.text(), kCells, columns(8));
        ASSERT_TRUE(fresh.setFolds(map.folds()));
        ASSERT_EQ(map.rowCount(), fresh.rowCount());
        for (std::uint32_t row = 0; row < map.rowCount(); ++row)
            EXPECT_EQ(map.rowInfo(DisplayRow{row}).buffer, fresh.rowInfo(DisplayRow{row}).buffer);
    }
}

TEST(DisplayFold, ReplacingConsumedNestedFoldsKeepsDistantProjection) {
    for (const bool hidden : {false, true}) {
        Document doc = Document::fromUtf8("0123456789abcdefghijklmnopqrstuvwxyz").value();
        DisplayMap map(doc.text(), kCells, columns(7));
        std::vector<ByteRange> folds{range(4, 12), range(5, 10), range(25, 30)};
        if (hidden)
            folds.insert(folds.begin(), range(1, 20));
        ASSERT_TRUE(map.setFolds(folds));
        const auto distant = map.firstFoldStartingIn(range(25, 25));
        ASSERT_TRUE(distant);
        const auto change = doc.replace(range(3, 13), "XY\nZ", EditContext{});
        ASSERT_TRUE(change);
        ASSERT_TRUE(map.apply(*change, doc.text()));
        EXPECT_EQ(map.foldUpdateStatsForTesting().eventIntervalsRewritten, 1U);
        EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
        ASSERT_TRUE(map.checkInvariants());
        const auto survivor = map.firstFoldStartingIn(range(19, 19));
        ASSERT_TRUE(survivor);
        EXPECT_EQ(survivor->id, distant->id);
        DisplayMap fresh(doc.text(), kCells, columns(7));
        ASSERT_TRUE(fresh.setFolds(map.folds()));
        ASSERT_EQ(map.rowCount(), fresh.rowCount());
        for (std::uint32_t row = 0; row < map.rowCount(); ++row)
            EXPECT_EQ(map.rowInfo(DisplayRow{row}).buffer, fresh.rowInfo(DisplayRow{row}).buffer);
    }
}

TEST(DisplayFold, DeletingOneFoldDoesNotEnumerateDistantFolds) {
    std::string source = "prefix\n";
    std::vector<ByteRange> folds;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        const auto begin = static_cast<std::uint64_t>(source.size());
        source += "abcde\n";
        folds.push_back(range(begin, begin + 3));
    }
    Document doc = Document::fromUtf8(source).value();
    DisplayMap map(doc.text(), kCells, columns(8));
    ASSERT_TRUE(map.setFolds(folds));
    const auto change = doc.replace(folds.front(), "", EditContext{});
    ASSERT_TRUE(change);
    ASSERT_TRUE(map.apply(*change, doc.text()));
    EXPECT_LE(map.foldUpdateStatsForTesting().visitedFolds, 3U);
    EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
    EXPECT_TRUE(map.checkInvariants());
    EXPECT_EQ(map.folds().size(), 999U);
}

TEST(DisplayFold, BoundaryReplacementKeepsIdWithoutEnumeratingDistantFolds) {
    for (const std::string_view replacement : {std::string_view(""), std::string_view("xy")}) {
        std::string source = "prefix\n";
        std::vector<ByteRange> folds;
        for (std::uint64_t i = 0; i < 1000; ++i) {
            const auto begin = static_cast<std::uint64_t>(source.size());
            source += "abcde\n";
            folds.push_back(range(begin, begin + 3));
        }
        Document doc = Document::fromUtf8(source).value();
        DisplayMap map(doc.text(), kCells, columns(8));
        ASSERT_TRUE(map.setFolds(folds));
        const auto original = map.firstFoldStartingIn(range(7, 7));
        ASSERT_TRUE(original);
        const auto change = doc.replace(range(7, 8), replacement, EditContext{});
        ASSERT_TRUE(change);
        ASSERT_TRUE(map.apply(*change, doc.text()));
        EXPECT_LE(map.foldUpdateStatsForTesting().visitedFolds, 3U);
        EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
        const auto start = 7U + static_cast<unsigned>(replacement.size());
        const auto restored = map.firstFoldStartingIn(range(start, start));
        ASSERT_TRUE(restored);
        EXPECT_EQ(restored->id, original->id);
        EXPECT_TRUE(map.checkInvariants());
    }
}

TEST(DisplayFold, DistantBoundaryBatchKeepsSeparatePatchesAndIds) {
    std::string source = "prefix\n";
    std::vector<ByteRange> folds;
    for (std::uint64_t i = 0; i < 200; ++i) {
        const auto begin = static_cast<std::uint64_t>(source.size());
        source += "abcde\n";
        folds.push_back(range(begin, begin + 3));
    }
    Document doc = Document::fromUtf8(source).value();
    DisplayMap map(doc.text(), kCells, columns(8));
    ASSERT_TRUE(map.setFolds(folds));
    const auto first = map.firstFoldStartingIn({folds.front().begin, folds.front().begin});
    const auto last = map.firstFoldStartingIn({folds.back().begin, folds.back().begin});
    ASSERT_TRUE(first && last);
    const std::array edits{Edit{{folds.front().begin, folds.front().begin + 1}, "XY"},
                           Edit{{folds.back().begin, folds.back().begin + 1}, "Z"}};
    const auto change = doc.edit(edits, EditContext{});
    ASSERT_TRUE(change);
    const auto patches = map.apply(*change, doc.text());
    ASSERT_TRUE(patches);
    EXPECT_EQ(patches->size(), 2U);
    EXPECT_LE(map.foldUpdateStatsForTesting().visitedFolds, 6U);
    EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
    ASSERT_TRUE(map.checkInvariants());
    ASSERT_TRUE(map.removeFold(first->id));
    ASSERT_TRUE(map.removeFold(last->id));
}

TEST(DisplayFold, BoundaryInsertionsPreserveAdjacentFoldEndpoints) {
    for (const auto at : {1U, 4U, 7U}) {
        Document doc = Document::fromUtf8("abcdefghi").value();
        DisplayMap map(doc.text(), kCells, columns(4));
        ASSERT_TRUE(map.setFolds({range(1, 4), range(4, 7)}));
        const auto change = doc.replace(range(at, at), "XY", EditContext{});
        ASSERT_TRUE(change);
        ASSERT_TRUE(map.apply(*change, doc.text()));
        EXPECT_EQ(map.foldUpdateStatsForTesting().visitedFolds, 0U);
        EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
        const auto before = range(at <= 1 ? 3 : 1, at < 4 ? 6 : 4);
        const auto after = range(at <= 4 ? 6 : 4, at < 7 ? 9 : 7);
        EXPECT_EQ(map.folds(), (std::vector<ByteRange>{before, after}));
        EXPECT_TRUE(map.checkInvariants());
        DisplayMap fresh(doc.text(), kCells, columns(4));
        ASSERT_TRUE(fresh.setFolds(map.folds()));
        EXPECT_EQ(map.rowCount(), fresh.rowCount());
    }
}

TEST(DisplayFold, StartAndEndInsertionsDoNotEnumerateFolds) {
    for (const auto at : {0U, 6U}) {
        Document doc = Document::fromUtf8("abcdef").value();
        DisplayMap map(doc.text(), kCells);
        ASSERT_TRUE(map.setFolds({range(1, 4)}));
        const auto change = doc.replace(range(at, at), "x", EditContext{});
        ASSERT_TRUE(change);
        ASSERT_TRUE(map.apply(*change, doc.text()));
        EXPECT_EQ(map.foldUpdateStatsForTesting().visitedFolds, 0U);
        EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
        EXPECT_TRUE(map.checkInvariants());
    }
}

TEST(DisplayFold, InsertionsAroundCrlfMatchProjectedText) {
    const std::string source = "a\r\nbc\nde\rfg";
    for (std::size_t at = 0; at <= source.size(); ++at) {
        for (const std::string_view inserted : {std::string_view("X"), std::string_view("\n")}) {
            Document doc = Document::fromUtf8(source).value();
            DisplayMap map(doc.text(), kCells, columns(4));
            ASSERT_TRUE(map.setFolds({range(3, 5)}));
            const auto change = doc.replace(range(at, at), inserted, EditContext{});
            ASSERT_TRUE(change);
            ASSERT_TRUE(map.apply(*change, doc.text())) << at;
            ASSERT_TRUE(map.checkInvariants()) << at;
            const auto& folds = map.folds();
            std::string projected = doc.text().text();
            if (!folds.empty())
                projected.replace(static_cast<std::size_t>(folds[0].begin.value()),
                                  static_cast<std::size_t>(folds[0].size()), "...");
            DisplayMap expected(snap(projected), kCells, columns(4));
            ASSERT_EQ(map.rowCount(), expected.rowCount()) << at;
            for (std::uint32_t row = 0; row < map.rowCount(); ++row) {
                std::string shown;
                for (const auto& segment : map.rowInfo(DisplayRow{row}).segments)
                    shown += segment.kind == frost::text::SegmentKind::Text ? map.text().text(segment.buffer)
                                                                            : std::string(segment.text);
                EXPECT_EQ(shown, expected.text().text(expected.rowInfo(DisplayRow{row}).buffer)) << at << "," << row;
            }
        }
    }
}

TEST(DisplayFold, ExhaustiveSmallReplacementsStayLocalAcrossNestedAndAdjacentFolds) {
    constexpr std::string_view source = "abcdefghijkl";
    for (std::uint64_t begin = 0; begin <= source.size(); ++begin) {
        for (std::uint64_t end = begin; end <= source.size(); ++end) {
            for (const std::string_view replacement :
                 {std::string_view(""), std::string_view("x"), std::string_view("\r\n")}) {
                if (begin == end && replacement.empty())
                    continue;
                SCOPED_TRACE(::testing::Message() << begin << "," << end << " => " << replacement.size());
                Document doc = Document::fromUtf8(std::string(source)).value();
                DisplayMap map(doc.text(), kCells, columns(4));
                ASSERT_TRUE(map.setFolds({range(1, 6), range(2, 4), range(6, 9), range(9, 11)}));
                const auto change = doc.replace(range(begin, end), replacement, EditContext{});
                ASSERT_TRUE(change);
                ASSERT_TRUE(map.apply(*change, doc.text()));
                EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
                ASSERT_TRUE(map.checkInvariants());
                DisplayMap fresh(doc.text(), kCells, columns(4));
                ASSERT_TRUE(fresh.setFolds(map.folds()));
                ASSERT_EQ(map.rowCount(), fresh.rowCount());
                for (std::uint32_t row = 0; row < map.rowCount(); ++row) {
                    const auto actual = map.rowInfo(DisplayRow{row});
                    const auto expected = fresh.rowInfo(DisplayRow{row});
                    ASSERT_EQ(actual.buffer, expected.buffer);
                    ASSERT_EQ(actual.segments.size(), expected.segments.size());
                    for (std::size_t i = 0; i < actual.segments.size(); ++i) {
                        EXPECT_EQ(actual.segments[i].kind, expected.segments[i].kind);
                        EXPECT_EQ(actual.segments[i].text, expected.segments[i].text);
                    }
                }
            }
        }
    }
}

TEST(DisplayFold, SeededEditsMatchFreshFoldedMap) {
    constexpr std::string_view source = "aa\nbb\ncc\ndd\nee\nff\n";
    SplitMix64 random(0x45fa128cULL);
    for (int iteration = 0; iteration < 200; ++iteration) {
        const auto a = random.below(source.size() + 1);
        const auto b = a + random.below(source.size() - a + 1);
        const std::array replacements{std::string_view(""), std::string_view("X"), std::string_view("\n"),
                                      std::string_view("\r\n")};
        const auto replacement = replacements[static_cast<std::size_t>(random.below(replacements.size()))];
        if (a == b && replacement.empty())
            continue;
        SCOPED_TRACE(::testing::Message() << iteration << ": " << a << "," << b);
        Document doc = Document::fromUtf8(std::string(source)).value();
        DisplayMap map(doc.text(), kCells, columns(5));
        ASSERT_TRUE(map.setFolds({range(2, 12), range(4, 8), range(14, 17)}));
        const auto change = doc.replace(range(a, b), replacement, EditContext{});
        ASSERT_TRUE(change);
        ASSERT_TRUE(map.apply(*change, doc.text()));
        EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
        ASSERT_TRUE(map.checkInvariants());
        DisplayMap fresh(doc.text(), kCells, columns(5));
        ASSERT_TRUE(fresh.setFolds(map.folds()));
        ASSERT_EQ(map.rowCount(), fresh.rowCount());
        for (std::uint32_t i = 0; i < map.rowCount(); ++i) {
            const auto actual = map.rowInfo(DisplayRow{i});
            const auto expected = fresh.rowInfo(DisplayRow{i});
            ASSERT_EQ(actual.buffer, expected.buffer);
            ASSERT_EQ(actual.segments.size(), expected.segments.size());
            for (std::size_t j = 0; j < actual.segments.size(); ++j) {
                EXPECT_EQ(actual.segments[j].kind, expected.segments[j].kind);
                EXPECT_EQ(actual.segments[j].buffer, expected.segments[j].buffer);
                EXPECT_EQ(actual.segments[j].text, expected.segments[j].text);
            }
        }
    }
}

TEST(DisplayFold, SeededBatchesMatchFreshFoldedMap) {
    constexpr std::string_view source = "aa\nbb\ncc\ndd\nee\nff\n";
    SplitMix64 random(0xb8a7d31ULL);
    for (int iteration = 0; iteration < 150; ++iteration) {
        std::array<std::uint64_t, 4> points{random.below(source.size() + 1), random.below(source.size() + 1),
                                            random.below(source.size() + 1), random.below(source.size() + 1)};
        std::ranges::sort(points);
        SCOPED_TRACE(::testing::Message()
                     << iteration << ": " << points[0] << "," << points[1] << "," << points[2] << "," << points[3]);
        Document doc = Document::fromUtf8(std::string(source)).value();
        DisplayMap map(doc.text(), kCells, columns(5));
        ASSERT_TRUE(map.setFolds({range(2, 12), range(4, 8), range(14, 17)}));
        const std::array edits{Edit{range(points[0], points[1]), "X"}, Edit{range(points[2], points[3]), "Y\n"}};
        const auto change = doc.edit(edits, EditContext{});
        ASSERT_TRUE(change);
        ASSERT_TRUE(map.apply(*change, doc.text()));
        EXPECT_EQ(map.foldUpdateStatsForTesting().materializedFoldLists, 0U);
        ASSERT_TRUE(map.checkInvariants());
        DisplayMap fresh(doc.text(), kCells, columns(5));
        ASSERT_TRUE(fresh.setFolds(map.folds()));
        ASSERT_EQ(map.rowCount(), fresh.rowCount());
        for (std::uint32_t i = 0; i < map.rowCount(); ++i) {
            const auto actual = map.rowInfo(DisplayRow{i});
            const auto expected = fresh.rowInfo(DisplayRow{i});
            ASSERT_EQ(actual.buffer, expected.buffer);
            ASSERT_EQ(actual.segments.size(), expected.segments.size());
            for (std::size_t j = 0; j < actual.segments.size(); ++j) {
                EXPECT_EQ(actual.segments[j].kind, expected.segments[j].kind);
                EXPECT_EQ(actual.segments[j].buffer, expected.segments[j].buffer);
                EXPECT_EQ(actual.segments[j].text, expected.segments[j].text);
            }
        }
    }
}

TEST(DisplayFold, InsertionsMatchFreshProjection) {
    const std::string source = "aa\nbb\ncc\ndd\n";
    for (std::size_t begin = 0; begin < source.size(); ++begin) {
        for (std::size_t end = begin + 1; end <= source.size(); ++end) {
            for (const std::size_t position : {std::size_t{0}, begin, end, source.size()}) {
                Document doc = Document::fromUtf8(source).value();
                DisplayMap actual(doc.text(), kCells, columns(4));
                ASSERT_TRUE(actual.setFolds({range(begin, end)}));
                auto edit = doc.replace(range(position, position), "x\n", EditContext{});
                ASSERT_TRUE(edit);
                ASSERT_TRUE(actual.apply(*edit, doc.text())) << begin << "," << end << "," << position;
                ASSERT_TRUE(actual.checkInvariants()) << begin << "," << end << "," << position;
                DisplayMap fresh(doc.text(), kCells, columns(4));
                ASSERT_TRUE(fresh.setFolds(actual.folds()));
                ASSERT_EQ(actual.rowCount(), fresh.rowCount()) << begin << "," << end << "," << position;
                for (std::uint32_t row = 0; row < actual.rowCount(); ++row) {
                    const auto a = actual.rowInfo(DisplayRow{row});
                    const auto b = fresh.rowInfo(DisplayRow{row});
                    EXPECT_EQ(a.buffer, b.buffer) << begin << "," << end << "," << position << "," << row;
                    EXPECT_EQ(a.segments.size(), b.segments.size());
                }
            }
        }
    }
}

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
    const Snapshot afterFirst = doc.text();
    const ChangeSet second = doc.replace(range(0, 0), "b", EditContext{}).value();
    EXPECT_EQ(map.apply(second, doc.text()).error().code(), ErrorCode::InvalidArgument); // Skips `first`.
    EXPECT_EQ(map.apply(first, before).error().code(), ErrorCode::InvalidArgument);      // Wrong snapshot.
    EXPECT_EQ(map.version(), before.version());
    ChangeSet invalid = first;
    invalid.edits.clear();
    EXPECT_EQ(map.apply(invalid, afterFirst).error().code(), ErrorCode::InvalidArgument);
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
