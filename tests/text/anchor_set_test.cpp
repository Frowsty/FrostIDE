// SPDX-License-Identifier: Apache-2.0
//
// AnchorSet (ADR 0017, decisions 2 and 3): creation, resolution, release, the deletion-boundary
// tables, raw edits at a CR, piles of anchors, label exhaustion, the splice and sweep paths,
// moves, stats and a 200-seed comparison with a naive model.

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/anchor_set.hpp"
#include "frost/text/document.hpp"

#include "text/support/undo_oracle.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using frost::ByteOffset;
using frost::ErrorCode;
using frost::Result;
using frost::sched::detail::SplitMix64;
using frost::text::AnchorId;
using frost::text::AnchorSet;
using frost::text::Bias;
using frost::text::ByteRange;
using frost::text::Document;
using frost::text::EditContext;
using frost::text::RawEdit;
using frost::text::ResolvedAnchor;
using frost::text::test::oracleMapBatch;

RawEdit edit(std::uint64_t a, std::uint64_t b, std::uint64_t n) {
    return RawEdit{ByteRange{ByteOffset{a}, ByteOffset{b}}, n};
}
ByteRange range(std::uint64_t a, std::uint64_t b) { return ByteRange{ByteOffset{a}, ByteOffset{b}}; }

std::uint64_t at(const AnchorSet& set, AnchorId id) { return set.resolve(id).value().value(); }

TEST(AnchorSet, CreateResolveRelease) {
    AnchorSet set(3, ByteOffset{10});
    EXPECT_TRUE(set.empty());
    EXPECT_EQ(set.version(), 3u);
    EXPECT_EQ(set.textSize(), ByteOffset{10});
    auto [one, a] = set.withAnchor(ByteOffset{4}, Bias::Left);
    auto [two, b] = one.withAnchor(ByteOffset{4}, Bias::Right);
    auto [three, c] = two.withAnchor(ByteOffset{10}, Bias::Left);
    EXPECT_TRUE(set.empty()); // Values: the originals are unchanged.
    EXPECT_EQ(one.size(), 1u);
    EXPECT_EQ(three.size(), 3u);
    EXPECT_LT(a.value, b.value);
    EXPECT_LT(b.value, c.value);
    EXPECT_EQ(at(three, a), 4u);
    EXPECT_EQ(three.anchor(b).value(), (frost::text::Anchor{ByteOffset{4}, Bias::Right}));
    EXPECT_EQ(at(three, c), 10u);
    const auto released = three.without(b);
    ASSERT_TRUE(released.has_value());
    EXPECT_EQ(released->size(), 2u);
    EXPECT_FALSE(released->resolve(b).has_value());
    EXPECT_EQ(at(*released, c), 10u);
    EXPECT_TRUE(released->checkInvariants().has_value()) << released->checkInvariants().error().describe();
    // Ids are not reused within a lineage.
    auto [four, d] = released->withAnchor(ByteOffset{0}, Bias::Left);
    EXPECT_GT(d.value, c.value);
    EXPECT_TRUE(four.checkInvariants().has_value());
}

TEST(AnchorSet, Errors) {
    auto [set, id] = AnchorSet(0, ByteOffset{5}).withAnchor(ByteOffset{2}, Bias::Left);
    const AnchorId missing{id.value + 7};
    EXPECT_EQ(set.resolve(missing).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(set.anchor(missing).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(set.without(missing).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(set.resolve(AnchorId{0}).error().code(), ErrorCode::NotFound);
    const auto released = set.without(id);
    ASSERT_TRUE(released.has_value());
    EXPECT_EQ(released->without(id).error().code(), ErrorCode::NotFound);

    // Document's createAnchor checks, in order: beyond the text, inside a sequence.
    Document doc = Document::fromUtf8("a\xC3\xA9z").value();
    EXPECT_EQ(doc.createAnchor(ByteOffset{5}, Bias::Left).error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(doc.createAnchor(ByteOffset{2}, Bias::Left).error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(doc.createAnchor(ByteOffset{3}, Bias::Left).has_value());
    EXPECT_EQ(doc.releaseAnchor(AnchorId{99}).error().code(), ErrorCode::NotFound);
}

// Anchors at every offset 0..10 of "0123456789" with both biases, through each edit.
TEST(AnchorSet, DeletionBoundaries) {
    struct Case {
        const char* name;
        std::vector<RawEdit> batch;
        std::vector<std::uint64_t> left;
        std::vector<std::uint64_t> right;
    };
    const Case cases[] = {
        {"delete [3,6)", {edit(3, 6, 0)}, {0, 1, 2, 3, 3, 3, 3, 4, 5, 6, 7}, {0, 1, 2, 3, 3, 3, 3, 4, 5, 6, 7}},
        {"insert ab at 3", {edit(3, 3, 2)}, {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12}, {0, 1, 2, 5, 6, 7, 8, 9, 10, 11, 12}},
        {"replace [3,6) by xy", {edit(3, 6, 2)}, {0, 1, 2, 3, 3, 3, 3, 6, 7, 8, 9}, {0, 1, 2, 5, 5, 5, 5, 6, 7, 8, 9}},
        {"delete [0,10)", {edit(0, 10, 0)}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {"insert ab at 0", {edit(0, 0, 2)}, {0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}},
        {"insert ab at 10", {edit(10, 10, 2)}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12}},
        {"[2,4) to empty plus [4,6) to Q",
         {edit(2, 4, 0), edit(4, 6, 1)},
         {0, 1, 2, 2, 2, 2, 2, 4, 5, 6, 7},
         {0, 1, 2, 2, 3, 3, 3, 4, 5, 6, 7}},
        {"two inserts at 5 (a, then bc)",
         {edit(5, 5, 1), edit(5, 5, 2)},
         {0, 1, 2, 3, 4, 5, 9, 10, 11, 12, 13},
         {0, 1, 2, 3, 4, 8, 9, 10, 11, 12, 13}},
    };
    AnchorSet base(0, ByteOffset{10});
    std::vector<AnchorId> left;
    std::vector<AnchorId> right;
    for (std::uint64_t x = 0; x <= 10; ++x) {
        auto [l, lid] = base.withAnchor(ByteOffset{x}, Bias::Left);
        auto [r, rid] = l.withAnchor(ByteOffset{x}, Bias::Right);
        base = r;
        left.push_back(lid);
        right.push_back(rid);
    }
    for (const Case& c : cases) {
        // The default path, both forced paths and the oracle's sequential rule agree with the table.
        for (int path = 0; path < 3; ++path) {
            const AnchorSet after =
                path == 0 ? base.transformed(c.batch, 1) : base.transformedForTesting(c.batch, 1, path == 2);
            ASSERT_TRUE(after.checkInvariants().has_value())
                << c.name << ": " << after.checkInvariants().error().describe();
            for (std::uint64_t x = 0; x <= 10; ++x) {
                EXPECT_EQ(at(after, left[x]), c.left[x]) << c.name << ", Left at " << x << ", path " << path;
                EXPECT_EQ(at(after, right[x]), c.right[x]) << c.name << ", Right at " << x << ", path " << path;
                EXPECT_EQ(oracleMapBatch(x, Bias::Left, c.batch), c.left[x]) << c.name;
                EXPECT_EQ(oracleMapBatch(x, Bias::Right, c.batch), c.right[x]) << c.name;
            }
        }
        // Through Document, with real bytes.
        Document doc = Document::fromUtf8("0123456789").value();
        std::vector<AnchorId> docLeft;
        std::vector<AnchorId> docRight;
        for (std::uint64_t x = 0; x <= 10; ++x) {
            docLeft.push_back(doc.createAnchor(ByteOffset{x}, Bias::Left).value());
            docRight.push_back(doc.createAnchor(ByteOffset{x}, Bias::Right).value());
        }
        std::vector<std::string> texts;
        std::vector<frost::text::Edit> edits;
        for (const RawEdit& e : c.batch) {
            texts.push_back(std::string(e.newBytes, 'Q'));
        }
        for (std::size_t i = 0; i < c.batch.size(); ++i) {
            edits.push_back(frost::text::Edit{c.batch[i].range, texts[i]});
        }
        ASSERT_TRUE(doc.edit(edits, EditContext{}).has_value()) << c.name;
        for (std::uint64_t x = 0; x <= 10; ++x) {
            EXPECT_EQ(at(doc.anchors(), docLeft[x]), c.left[x]) << c.name << ", document Left at " << x;
            EXPECT_EQ(at(doc.anchors(), docRight[x]), c.right[x]) << c.name << ", document Right at " << x;
        }
        // Undo puts every anchor back (decision 10).
        ASSERT_TRUE(doc.undo().has_value());
        for (std::uint64_t x = 0; x <= 10; ++x) {
            EXPECT_EQ(at(doc.anchors(), docLeft[x]), x) << c.name << ", after undo";
            EXPECT_EQ(at(doc.anchors(), docRight[x]), x) << c.name << ", after undo";
        }
        EXPECT_TRUE(doc.checkInvariants().has_value()) << doc.checkInvariants().error().describe();
    }
}

// An insert between CR and LF: D8 widens the TextChange to start at the CR, but anchors follow the
// raw edit, so a Left anchor at 2 stays and a Right one moves past the insert.
TEST(AnchorSet, CrlfInteriorUsesRawEdits) {
    Document doc = Document::fromUtf8("a\r\nb").value();
    const AnchorId left = doc.createAnchor(ByteOffset{2}, Bias::Left).value();
    const AnchorId right = doc.createAnchor(ByteOffset{2}, Bias::Right).value();
    const AnchorId cr = doc.createAnchor(ByteOffset{1}, Bias::Right).value();
    const auto change = doc.replace(range(2, 2), "x", EditContext{});
    ASSERT_TRUE(change.has_value());
    ASSERT_EQ(change->changes.size(), 1u);
    EXPECT_EQ(change->changes[0].start, ByteOffset{1}); // Widened to the CR.
    EXPECT_EQ(change->edits, (std::vector{edit(2, 2, 1)}));
    EXPECT_EQ(at(doc.anchors(), left), 2u);
    EXPECT_EQ(at(doc.anchors(), right), 3u);
    EXPECT_EQ(at(doc.anchors(), cr), 1u); // Before the widened start: untouched.
}

TEST(AnchorSet, ManyAnchorsAtOneOffset) {
    constexpr std::uint64_t kPile = 10'000;
    AnchorSet set(0, ByteOffset{20});
    std::vector<AnchorId> ids;
    for (std::uint64_t i = 0; i < kPile; ++i) {
        auto [next, id] = set.withAnchor(ByteOffset{5}, i % 2 == 0 ? Bias::Left : Bias::Right);
        set = next;
        ids.push_back(id);
    }
    ASSERT_TRUE(set.checkInvariants().has_value()) << set.checkInvariants().error().describe();
    // Delete the byte before the pile: every anchor moves to 4 (the first-gap shortcut).
    const AnchorSet deleted = set.transformed(std::vector{edit(4, 5, 0)}, 1);
    ASSERT_TRUE(deleted.checkInvariants().has_value()) << deleted.checkInvariants().error().describe();
    for (const AnchorId id : ids) {
        ASSERT_EQ(at(deleted, id), 4u);
    }
    // Insert at the pile: Left anchors stay, Right ones move past the insert.
    const AnchorSet inserted = deleted.transformed(std::vector{edit(4, 4, 3)}, 2);
    ASSERT_TRUE(inserted.checkInvariants().has_value());
    for (std::uint64_t i = 0; i < kPile; ++i) {
        ASSERT_EQ(at(inserted, ids[i]), i % 2 == 0 ? 4u : 7u);
    }
    EXPECT_EQ(inserted.resolveRange(range(4, 4)).size(), kPile / 2);
    EXPECT_EQ(inserted.resolveRange(range(7, 7)).size(), kPile / 2);
    // Delete across the pile from before it: everything collapses to 2.
    const AnchorSet collapsed = inserted.transformed(std::vector{edit(2, 9, 0)}, 3);
    ASSERT_TRUE(collapsed.checkInvariants().has_value());
    EXPECT_EQ(collapsed.resolveRange(range(2, 2)).size(), kPile);
    EXPECT_EQ(collapsed.textSize(), ByteOffset{15}); // 20 - 1 + 3 - 7.
}

TEST(AnchorSet, LabelExhaustionPatterns) {
    constexpr std::uint64_t kInserts = 10'000;
    const auto check = [](const AnchorSet& set, const std::vector<std::pair<AnchorId, std::uint64_t>>& expected,
                          const char* pattern) {
        ASSERT_TRUE(set.checkInvariants().has_value()) << pattern << ": " << set.checkInvariants().error().describe();
        for (const auto& [id, offset] : expected) {
            ASSERT_EQ(at(set, id), offset) << pattern;
        }
        const std::uint64_t n = set.size();
        const std::uint64_t bound = 64 * n * static_cast<std::uint64_t>(std::bit_width(n - 1));
        EXPECT_LE(set.statsForTesting().relabelledEntries, bound) << pattern;
        std::printf("AnchorSet.LabelExhaustionPatterns: %s: %llu anchors, %llu relabels, %llu relabelled entries\n",
                    pattern, static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(set.statsForTesting().relabels),
                    static_cast<unsigned long long>(set.statsForTesting().relabelledEntries));
    };
    {
        // One tie position before a fixed neighbour: every insert lands right after the previous one.
        AnchorSet set(0, ByteOffset{10});
        auto [s, sentinel] = set.withAnchor(ByteOffset{6}, Bias::Left);
        set = s;
        std::vector<std::pair<AnchorId, std::uint64_t>> expected{{sentinel, 6}};
        for (std::uint64_t i = 0; i < kInserts; ++i) {
            auto [next, id] = set.withAnchor(ByteOffset{5}, Bias::Left);
            set = next;
            expected.emplace_back(id, 5);
        }
        EXPECT_GT(set.statsForTesting().relabels, 0u);
        check(set, expected, "one tie position");
    }
    {
        // Alternating head and tail of the class.
        AnchorSet set(0, ByteOffset{10});
        std::vector<std::pair<AnchorId, std::uint64_t>> expected;
        for (std::uint64_t i = 0; i < kInserts; ++i) {
            const std::uint64_t offset = i % 2 == 0 ? 0 : 10;
            auto [next, id] = set.withAnchor(ByteOffset{offset}, Bias::Right);
            set = next;
            expected.emplace_back(id, offset);
        }
        check(set, expected, "alternating head and tail");
    }
    {
        // Always between the same two anchors (at 3 and 4): the gap halves each time.
        AnchorSet set(0, ByteOffset{10});
        auto [s1, p] = set.withAnchor(ByteOffset{3}, Bias::Right);
        auto [s2, q] = s1.withAnchor(ByteOffset{4}, Bias::Right);
        set = s2;
        std::vector<std::pair<AnchorId, std::uint64_t>> expected{{p, 3}, {q, 4}};
        for (std::uint64_t i = 0; i < kInserts; ++i) {
            auto [next, id] = set.withAnchor(ByteOffset{3}, Bias::Right);
            set = next;
            expected.emplace_back(id, 3);
        }
        EXPECT_GT(set.statsForTesting().relabels, 0u);
        check(set, expected, "between two anchors");
    }
}

// Random sets and batches: the splice and sweep paths build equal sets, and both match the rule.
TEST(AnchorSet, SpliceAndSweepAgree) {
    SplitMix64 rng(0x5EED);
    for (int round = 0; round < 300; ++round) {
        const std::uint64_t size = 1 + rng.below(64);
        AnchorSet set(0, ByteOffset{size});
        std::vector<std::tuple<AnchorId, std::uint64_t, Bias>> anchors;
        const std::uint64_t count = rng.below(80);
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint64_t offset = rng.below(4) == 0 ? size / 2 : rng.below(size + 1);
            const Bias bias = rng.below(2) == 0 ? Bias::Left : Bias::Right;
            auto [next, id] = set.withAnchor(ByteOffset{offset}, bias);
            set = next;
            anchors.emplace_back(id, offset, bias);
        }
        std::vector<RawEdit> batch;
        std::uint64_t cursor = 0;
        while (cursor <= size && batch.size() < 6 && rng.below(5) != 0) {
            const std::uint64_t a = std::min(size, cursor + rng.below(8));
            const std::uint64_t b = std::min(size, a + rng.below(6));
            batch.push_back(edit(a, b, rng.below(4)));
            cursor = b;
        }
        const AnchorSet splice = set.transformedForTesting(batch, 1, false);
        const AnchorSet sweep = set.transformedForTesting(batch, 1, true);
        ASSERT_TRUE(splice.checkInvariants().has_value()) << splice.checkInvariants().error().describe();
        ASSERT_TRUE(sweep.checkInvariants().has_value()) << sweep.checkInvariants().error().describe();
        ASSERT_TRUE(splice.sameStructureForTesting(sweep)) << "round " << round;
        for (const auto& [id, offset, bias] : anchors) {
            ASSERT_EQ(at(splice, id), oracleMapBatch(offset, bias, batch)) << "round " << round;
        }
    }
}

TEST(AnchorSet, ResolveRangeOrder) {
    AnchorSet set(0, ByteOffset{10});
    std::vector<AnchorId> ids;
    const std::pair<std::uint64_t, Bias> placements[] = {{5, Bias::Right}, {5, Bias::Left}, {3, Bias::Right},
                                                         {7, Bias::Left},  {5, Bias::Left}, {3, Bias::Left},
                                                         {8, Bias::Right}, {2, Bias::Left}};
    for (const auto& [offset, bias] : placements) {
        auto [next, id] = set.withAnchor(ByteOffset{offset}, bias);
        set = next;
        ids.push_back(id);
    }
    const std::vector<ResolvedAnchor> found = set.resolveRange(range(3, 7));
    const std::vector<ResolvedAnchor> expected{
        {ids[5], ByteOffset{3}, Bias::Left}, {ids[2], ByteOffset{3}, Bias::Right}, {ids[1], ByteOffset{5}, Bias::Left},
        {ids[4], ByteOffset{5}, Bias::Left}, {ids[0], ByteOffset{5}, Bias::Right}, {ids[3], ByteOffset{7}, Bias::Left}};
    EXPECT_EQ(found, expected);
    EXPECT_EQ(set.resolveRange(range(4, 4)).size(), 0u);
    EXPECT_EQ(set.all().size(), std::size(placements));
    EXPECT_EQ(set.resolveRange(range(8, 10)).size(), 1u);
}

TEST(AnchorSet, WithMovedKeepsIdsAndSkipsNoOps) {
    AnchorSet set(0, ByteOffset{100});
    std::vector<AnchorId> ids;
    for (std::uint64_t i = 0; i < 50; ++i) {
        auto [next, id] = set.withAnchor(ByteOffset{i * 2}, i % 3 == 0 ? Bias::Right : Bias::Left);
        set = next;
        ids.push_back(id);
    }
    // A move to the current offset changes nothing at all.
    const std::vector<ResolvedAnchor> same{{ids[4], ByteOffset{8}, Bias::Left}};
    EXPECT_TRUE(set.withMoved(same).sameStructureForTesting(set));
    // A few moves (the per-move path) and many moves (the bulk path) give equal results.
    std::vector<ResolvedAnchor> few{{ids[1], ByteOffset{99}, Bias::Left}, {ids[7], ByteOffset{0}, Bias::Left}};
    const AnchorSet moved = set.withMoved(few);
    ASSERT_TRUE(moved.checkInvariants().has_value()) << moved.checkInvariants().error().describe();
    EXPECT_EQ(at(moved, ids[1]), 99u);
    EXPECT_EQ(at(moved, ids[7]), 0u);
    EXPECT_EQ(moved.anchor(ids[1])->bias, Bias::Left);
    EXPECT_EQ(moved.size(), set.size());
    std::vector<ResolvedAnchor> many;
    for (std::uint64_t i = 0; i < 50; i += 2) {
        many.push_back(ResolvedAnchor{ids[i], ByteOffset{100 - i}, Bias::Left});
    }
    const AnchorSet bulk = set.withMoved(many);
    ASSERT_TRUE(bulk.checkInvariants().has_value()) << bulk.checkInvariants().error().describe();
    for (std::uint64_t i = 0; i < 50; ++i) {
        EXPECT_EQ(at(bulk, ids[i]), i % 2 == 0 ? 100 - i : i * 2) << i;
        EXPECT_EQ(bulk.anchor(ids[i])->bias, set.anchor(ids[i])->bias);
    }
    // New ids continue after the moved set's.
    auto [after, fresh] = bulk.withAnchor(ByteOffset{1}, Bias::Left);
    EXPECT_GT(fresh.value, ids.back().value);
    EXPECT_TRUE(after.checkInvariants().has_value());
}

TEST(AnchorSet, StatsAreValues) {
    AnchorSet set(0, ByteOffset{10});
    auto [s, sentinel] = set.withAnchor(ByteOffset{6}, Bias::Left);
    set = s;
    for (int i = 0; i < 200; ++i) {
        set = set.withAnchor(ByteOffset{5}, Bias::Left).first;
    }
    const AnchorSet::Stats before = set.statsForTesting();
    EXPECT_GT(before.relabels, 0u);
    AnchorSet copy = set;
    for (int i = 0; i < 200; ++i) {
        copy = copy.withAnchor(ByteOffset{5}, Bias::Left).first;
    }
    EXPECT_GT(copy.statsForTesting().relabels, before.relabels);
    EXPECT_EQ(set.statsForTesting(), before); // The original's counters did not move.
    EXPECT_EQ(AnchorSet(0, ByteOffset{1}).statsForTesting(), AnchorSet::Stats{});
}

// 200 seeds x 2000 ops of create, release, transform and move against a naive vector.
// One seed of MatchesNaiveModel: 2000 random ops against a naive vector model; "" or the first
// difference.
std::string runNaiveSeed(std::uint64_t seed) {
    struct Naive {
        AnchorId id;
        std::uint64_t offset;
        Bias bias;
    };
    SplitMix64 rng(seed * 31 + 7);
    std::uint64_t size = rng.below(200);
    AnchorSet set(0, ByteOffset{size});
    std::vector<Naive> model;
    std::uint64_t version = 0;
    for (int op = 0; op < 2000; ++op) {
        const std::uint64_t r = rng.below(100);
        if (r < 40 && model.size() < 300) {
            const std::uint64_t offset =
                rng.below(3) == 0 && !model.empty() ? model[rng.below(model.size())].offset : rng.below(size + 1);
            const Bias bias = rng.below(2) == 0 ? Bias::Left : Bias::Right;
            auto [next, id] = set.withAnchor(ByteOffset{offset}, bias);
            set = next;
            model.push_back(Naive{id, offset, bias});
        } else if (r < 50 && !model.empty()) {
            const std::size_t index = rng.below(model.size());
            set = set.without(model[index].id).value();
            model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
        } else if (r < 85) {
            std::vector<RawEdit> batch;
            std::uint64_t cursor = rng.below(size + 1);
            for (std::uint64_t i = 0, count = 1 + rng.below(4); i < count && cursor <= size; ++i) {
                const std::uint64_t a = std::min(size, cursor + (rng.below(2) == 0 ? 0 : rng.below(20)));
                const std::uint64_t b = std::min(size, a + rng.below(rng.below(4) == 0 ? 60 : 6));
                batch.push_back(edit(a, b, rng.below(4) == 0 ? 0 : rng.below(30)));
                cursor = b;
            }
            std::int64_t delta = 0;
            for (const RawEdit& e : batch) {
                delta += static_cast<std::int64_t>(e.newBytes) - static_cast<std::int64_t>(e.range.size());
            }
            for (Naive& a : model) {
                a.offset = oracleMapBatch(a.offset, a.bias, batch);
            }
            size = static_cast<std::uint64_t>(static_cast<std::int64_t>(size) + delta);
            set = set.transformed(batch, ++version);
        } else if (!model.empty()) {
            std::vector<ResolvedAnchor> moves;
            const std::uint64_t count = rng.below(8) == 0 ? model.size() : 1 + rng.below(3);
            std::vector<std::size_t> picked;
            for (std::size_t i = 0; i < model.size() && picked.size() < count; ++i) {
                if (count == model.size() || rng.below(model.size()) < count) {
                    picked.push_back(i);
                }
            }
            for (const std::size_t i : picked) {
                model[i].offset = rng.below(size + 1);
                moves.push_back(ResolvedAnchor{model[i].id, ByteOffset{model[i].offset}, model[i].bias});
            }
            set = set.withMoved(moves);
        }
        const auto where = [&] { return std::format("seed {} op {}", seed, op); };
        if (op % 100 == 99 || op == 1999) {
            if (const Result<void> checked = set.checkInvariants(); !checked) {
                return where() + ": " + checked.error().describe();
            }
        }
        if (set.size() != model.size() || set.textSize() != ByteOffset{size}) {
            return where() + ": size or text size differs";
        }
        // Every 10 ops all() against the sorted model (one traversal); every 100 ops and at the
        // end each anchor through resolve() as well.
        if (op % 10 == 0 || op == 1999) {
            std::vector<ResolvedAnchor> expected;
            for (const Naive& a : model) {
                expected.push_back(ResolvedAnchor{a.id, ByteOffset{a.offset}, a.bias});
            }
            std::sort(expected.begin(), expected.end(), [](const ResolvedAnchor& x, const ResolvedAnchor& y) {
                return std::tie(x.offset, x.bias, x.id) < std::tie(y.offset, y.bias, y.id);
            });
            if (set.all() != expected) {
                return where() + ": all() differs from the sorted model";
            }
        }
        if (op % 100 == 0 || op == 1999) {
            for (const Naive& a : model) {
                const Result<ByteOffset> got = set.resolve(a.id);
                if (!got || got->value() != a.offset) {
                    return std::format("{}: anchor {} at {}, expected {}", where(), a.id.value,
                                       got ? got->value() : ~std::uint64_t{0}, a.offset);
                }
            }
        }
    }
    return {};
}

// 200 seeds x 2000 ops, on up to four threads (seeds are independent and deterministic).
TEST(AnchorSet, MatchesNaiveModel) {
    constexpr std::uint64_t kSeeds = 200;
    const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    std::atomic<std::uint64_t> next{0};
    std::mutex mutex;
    std::map<std::uint64_t, std::string> failures;
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            for (std::uint64_t seed = next.fetch_add(1); seed < kSeeds; seed = next.fetch_add(1)) {
                if (std::string failure = runNaiveSeed(seed); !failure.empty()) {
                    const std::lock_guard lock(mutex);
                    failures.emplace(seed, std::move(failure));
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    EXPECT_TRUE(failures.empty()) << failures.size() << " seeds failed; the first: " << failures.begin()->second;
}

} // namespace
