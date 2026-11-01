// SPDX-License-Identifier: Apache-2.0
//
// UndoCompose (white-box; ADR 0017, decision 5): region composition against the alignment
// definition. Each case feeds batches into one group transaction and compares the node's regions
// and their slices with the oracle's alignment-derived regions after every batch.

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/document.hpp"

#include "text/support/text_ops.hpp"
#include "text/support/undo_oracle.hpp"
#include "undo_tree.hpp"
#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace {

using frost::ByteOffset;
using frost::sched::detail::SplitMix64;
using frost::text::ByteRange;
using frost::text::Document;
using frost::text::Edit;
using frost::text::EditContext;
using frost::text::detail::checkRegions;
using frost::text::detail::PieceTree;
using frost::text::detail::Region;
using frost::text::test::OracleRegion;
using frost::text::test::oracleRegions;

struct Step {
    std::uint64_t a;
    std::uint64_t b;
    std::string text;
};
using Batch = std::vector<Step>;

std::string sliceText(const PieceTree& slice) {
    std::string out;
    for (PieceTree::Cursor c = slice.first(); c.item() != nullptr; c.next()) {
        out += c.item()->view();
    }
    return out;
}

// Runs `batches` in one group over `x` and checks the regions after each; returns the final count.
::testing::AssertionResult composeMatches(const std::string& x, const std::vector<Batch>& batches,
                                          std::size_t* finalRegions = nullptr) {
    Document doc = Document::fromUtf8(x).value();
    if (!doc.beginGroup(EditContext{}).has_value()) {
        return ::testing::AssertionFailure() << "beginGroup failed";
    }
    std::vector<std::int32_t> origin(x.size());
    std::iota(origin.begin(), origin.end(), 0);
    std::string y = x;
    for (std::size_t k = 0; k < batches.size(); ++k) {
        std::vector<Edit> edits;
        for (const Step& s : batches[k]) {
            edits.push_back(Edit{ByteRange{ByteOffset{s.a}, ByteOffset{s.b}}, s.text});
        }
        if (const auto result = doc.edit(edits, EditContext{}); !result) {
            return ::testing::AssertionFailure() << "batch " << k << ": " << result.error().describe();
        }
        for (std::size_t i = batches[k].size(); i-- > 0;) {
            const Step& s = batches[k][i];
            origin.erase(origin.begin() + static_cast<std::ptrdiff_t>(s.a),
                         origin.begin() + static_cast<std::ptrdiff_t>(s.b));
            origin.insert(origin.begin() + static_cast<std::ptrdiff_t>(s.a), s.text.size(), -1);
            y.replace(s.a, s.b - s.a, s.text);
        }
        const std::vector<OracleRegion> expected = oracleRegions(origin, x.size());
        const auto& tree = doc.undoTreeForTesting();
        const std::vector<Region>* regions = tree.current == tree.root ? nullptr : &tree.currentNode().regions;
        const std::size_t count = regions != nullptr ? regions->size() : 0;
        if (count != expected.size()) {
            return ::testing::AssertionFailure()
                   << "batch " << k << ": " << count << " regions, expected " << expected.size();
        }
        for (std::size_t i = 0; i < count; ++i) {
            const Region& r = (*regions)[i];
            const OracleRegion& e = expected[i];
            if (r.before.begin.value() != e.beforeBegin || r.before.end.value() != e.beforeEnd ||
                r.after.begin.value() != e.afterBegin || r.after.end.value() != e.afterEnd) {
                return ::testing::AssertionFailure()
                       << "batch " << k << ", region " << i << ": [" << r.before.begin.value() << ", "
                       << r.before.end.value() << ") -> [" << r.after.begin.value() << ", " << r.after.end.value()
                       << "), expected [" << e.beforeBegin << ", " << e.beforeEnd << ") -> [" << e.afterBegin << ", "
                       << e.afterEnd << ")";
            }
            if (sliceText(r.removed) != x.substr(e.beforeBegin, e.beforeEnd - e.beforeBegin) ||
                sliceText(r.inserted) != y.substr(e.afterBegin, e.afterEnd - e.afterBegin)) {
                return ::testing::AssertionFailure() << "batch " << k << ", region " << i << ": wrong slice bytes";
            }
        }
        if (regions != nullptr) {
            if (const auto checked = checkRegions(*regions, x.size(), y.size()); !checked) {
                return ::testing::AssertionFailure() << "batch " << k << ": " << checked.error().describe();
            }
        }
    }
    if (finalRegions != nullptr) {
        const auto& tree = doc.undoTreeForTesting();
        *finalRegions = tree.current == tree.root ? 0 : tree.currentNode().regions.size();
    }
    // Rolling back replays the composed regions: the text returns to x.
    if (!doc.rollbackGroup().has_value() || doc.text().text() != x || !doc.checkInvariants().has_value()) {
        return ::testing::AssertionFailure() << "rollback did not restore the text";
    }
    return ::testing::AssertionSuccess();
}

TEST(UndoCompose, Cases) {
    const std::string x = "0123456789";
    struct Case {
        const char* name;
        std::vector<Batch> batches;
        std::size_t regions;
    };
    const Case cases[] = {
        {"touching at the start", {{{3, 5, "ab"}}, {{3, 3, "X"}}}, 1},
        {"touching at the end", {{{3, 5, "ab"}}, {{5, 5, "Y"}}}, 1},
        {"touching at both ends", {{{3, 5, "ab"}}, {{3, 3, "X"}, {5, 5, "Y"}}}, 1},
        {"an edit inside a region", {{{2, 2, "abcdef"}}, {{3, 5, "Q"}}}, 1},
        {"an edit spanning two regions", {{{1, 2, "a"}, {5, 6, "b"}}, {{1, 6, ""}}}, 1},
        {"two regions stay apart", {{{1, 2, "a"}, {5, 6, "b"}}, {{3, 3, "c"}}}, 3},
        {"a net no-op is dropped", {{{4, 4, "xy"}}, {{4, 6, ""}}}, 0},
        {"a net no-op beside a region", {{{1, 2, "a"}, {4, 4, "xy"}}, {{4, 6, ""}}}, 1},
        {"adjacent batch edits", {{{2, 4, "A"}, {4, 6, "B"}}}, 1},
        {"two inserts at one offset", {{{3, 3, "a"}, {3, 3, "b"}}}, 1},
        {"a deletion extended by an insert", {{{3, 5, ""}}, {{3, 3, "Z"}}}, 1},
        {"a deletion extended by a deletion before it", {{{3, 5, ""}}, {{1, 3, ""}}}, 1},
        {"identical bytes still replace", {{{3, 4, "3"}}}, 1},
        {"an edit one byte past a region", {{{3, 5, "ab"}}, {{6, 7, "Z"}}}, 2},
    };
    for (const Case& c : cases) {
        std::size_t regions = 0;
        EXPECT_TRUE(composeMatches(x, c.batches, &regions)) << c.name;
        EXPECT_EQ(regions, c.regions) << c.name;
    }
}

TEST(UndoCompose, RandomMatchesAlignment) {
    SplitMix64 rng(0xC0DE);
    frost::text::test::SplitMixSource source(99);
    for (int round = 0; round < 2000; ++round) {
        std::string x = frost::text::test::tokenText(source, rng.below(48));
        std::vector<Batch> batches;
        std::uint64_t size = x.size();
        std::string y = x;
        for (std::uint64_t k = 0, count = 1 + rng.below(6); k < count; ++k) {
            Batch batch;
            std::uint64_t cursor = 0;
            for (std::uint64_t i = 0, edits = 1 + rng.below(3); i < edits && cursor <= size; ++i) {
                const auto boundary = [&](std::uint64_t at) {
                    at = std::min(at, size);
                    while (at > 0 && at < y.size() && (static_cast<unsigned char>(y[at]) & 0xC0u) == 0x80u) {
                        --at;
                    }
                    return at;
                };
                const std::uint64_t a = std::max(cursor, boundary(cursor + rng.below(8)));
                const std::uint64_t b = std::max(a, boundary(a + rng.below(6)));
                if (a < cursor) {
                    break;
                }
                batch.push_back(
                    Step{a, b, rng.below(3) == 0 ? std::string() : frost::text::test::tokenText(source, rng.below(5))});
                cursor = b;
            }
            for (std::size_t i = batch.size(); i-- > 0;) {
                y.replace(batch[i].a, batch[i].b - batch[i].a, batch[i].text);
            }
            size = y.size();
            batches.push_back(std::move(batch));
        }
        ASSERT_TRUE(composeMatches(x, batches)) << "round " << round;
    }
}

} // namespace
