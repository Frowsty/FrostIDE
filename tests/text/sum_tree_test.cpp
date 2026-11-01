// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost::text::SumTree (ADR 0013, D3) at fan-outs B = 2, 3 and 8, with uint32 items and
// a non-commutative summary, so any reordering, duplication or loss of items changes the hash.
/*
 * The summary keeps {count, sum, max, first, last, hash, pow}: hash = sum of item_i * M^(count-1-i)
 * (mod 2^64) and pow = M^count, so combine(a, b).hash = a.hash * b.pow + b.hash. The value-
 * initialised summary (count 0) is the identity; combine returns the other side for it, which
 * keeps the operation associative although Summary{}.pow is 0, not 1.
 */

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/sum_tree.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace {

using frost::sched::detail::SplitMix64;

constexpr std::uint64_t kMultiplier = 0x100'0000'01B3ULL; // The 64-bit FNV prime; any odd constant works.

struct TestSummary {
    std::uint64_t count = 0;
    std::uint64_t sum = 0;
    std::uint32_t max = 0;
    std::uint32_t first = 0;
    std::uint32_t last = 0;
    std::uint64_t hash = 0;
    std::uint64_t pow = 0;

    friend constexpr bool operator==(const TestSummary&, const TestSummary&) noexcept = default;
};

struct TestTraits {
    using Item = std::uint32_t;
    using Summary = TestSummary;

    static Summary summary(const Item& item) noexcept { return {1, item, item, item, item, item, kMultiplier}; }
    static Summary combine(const Summary& a, const Summary& b) noexcept {
        if (a.count == 0) {
            return b;
        }
        if (b.count == 0) {
            return a;
        }
        return {a.count + b.count,       a.sum + b.sum, std::max(a.max, b.max), a.first, b.last,
                a.hash * b.pow + b.hash, a.pow * b.pow};
    }
};

TestSummary fold(std::span<const std::uint32_t> items) {
    TestSummary s{};
    for (const std::uint32_t item : items) {
        s = TestTraits::combine(s, TestTraits::summary(item));
    }
    return s;
}

template<int B>
using Tree = frost::text::SumTree<TestTraits, B>;

template<int B>
std::vector<std::uint32_t> itemsOf(const Tree<B>& tree) {
    std::vector<std::uint32_t> items;
    for (auto cursor = tree.first(); cursor.item() != nullptr; cursor.next()) {
        items.push_back(*cursor.item());
    }
    return items;
}

std::vector<std::uint32_t> iota(std::uint32_t from, std::size_t n) {
    std::vector<std::uint32_t> items(n);
    for (std::size_t i = 0; i < n; ++i) {
        items[i] = from + static_cast<std::uint32_t>(i);
    }
    return items;
}

// Checks invariants, contents and the cached summary of `tree` against `expected`.
template<int B>
::testing::AssertionResult matches(const Tree<B>& tree, const std::vector<std::uint32_t>& expected) {
    if (auto invariants = tree.checkInvariants(); !invariants) {
        return ::testing::AssertionFailure() << invariants.error().describe();
    }
    if (tree.empty() != expected.empty()) {
        return ::testing::AssertionFailure() << "empty() is " << tree.empty() << " for " << expected.size() << " items";
    }
    if (!(tree.summary() == fold(expected))) {
        return ::testing::AssertionFailure() << "summary differs from the fold of " << expected.size() << " items";
    }
    if (itemsOf(tree) != expected) {
        return ::testing::AssertionFailure() << "items differ (" << expected.size() << " expected)";
    }
    return ::testing::AssertionSuccess();
}

// Index-based split: the first k items and the rest.
template<int B>
std::pair<Tree<B>, Tree<B>> splitAt(const Tree<B>& tree, std::uint64_t k) {
    return tree.split([k](const TestSummary& s) { return s.count > k; });
}

template<class Fanout>
class SumTreeTest : public ::testing::Test {};

template<int N>
struct Fanout {
    static constexpr int value = N;
};
using Fanouts = ::testing::Types<Fanout<2>, Fanout<3>, Fanout<8>>;
TYPED_TEST_SUITE(SumTreeTest, Fanouts);

TYPED_TEST(SumTreeTest, EmptyTree) {
    constexpr int B = TypeParam::value;
    const Tree<B> tree;
    EXPECT_TRUE(tree.empty());
    EXPECT_EQ(tree.height(), 0u);
    EXPECT_EQ(tree.summary(), TestSummary{});
    EXPECT_EQ(tree.front(), nullptr);
    EXPECT_EQ(tree.back(), nullptr);
    EXPECT_EQ(tree.first().item(), nullptr);
    EXPECT_TRUE(tree.withoutFront().empty());
    EXPECT_TRUE(tree.withoutBack().empty());
    const auto [l, r] = splitAt(tree, 0);
    EXPECT_TRUE(l.empty());
    EXPECT_TRUE(r.empty());
    EXPECT_TRUE(Tree<B>::concat(tree, tree).empty());
    EXPECT_TRUE(tree.checkInvariants().has_value());
}

TYPED_TEST(SumTreeTest, FromItemsShapes) {
    constexpr int B = TypeParam::value;
    for (std::size_t n = 0; n <= 300; ++n) {
        const std::vector<std::uint32_t> items = iota(1000, n);
        const Tree<B> tree = Tree<B>::fromItems(items);
        ASSERT_TRUE(matches(tree, items)) << "n = " << n;
        // A single leaf holds up to 2B items; a balanced build adds a level only when needed.
        if (n <= static_cast<std::size_t>(2 * B)) {
            EXPECT_EQ(tree.height(), 0u) << "n = " << n;
        }
        std::uint64_t capacity = 2 * B;
        for (std::uint32_t h = 0; h < tree.height(); ++h) {
            capacity *= 2 * B;
        }
        EXPECT_LE(n, capacity) << "n = " << n;
        if (tree.height() > 0) {
            // One level less would have fit B..2B per node only if n <= (2B)^height.
            std::uint64_t lower = 2 * B;
            for (std::uint32_t h = 1; h < tree.height(); ++h) {
                lower *= 2 * B;
            }
            EXPECT_GT(n, lower) << "n = " << n << " has a needlessly tall tree";
        }
        if (n > 0) {
            EXPECT_EQ(*tree.front(), items.front());
            EXPECT_EQ(*tree.back(), items.back());
        }
    }
}

TYPED_TEST(SumTreeTest, ConcatMixedHeights) {
    constexpr int B = TypeParam::value;
    // Sizes cover the empty tree, underfull leaf roots (< B), full leaves, two-child internal roots
    // (2B + 1) and several heights.
    const std::vector<std::size_t> sizes = {0,   1,   2,  B - 1, B, B + 1, 2 * B, 2 * B + 1, 4 * B * B, 4 * B * B + 1,
                                            100, 257, 300};
    std::uint32_t next = 1;
    for (const std::size_t ln : sizes) {
        for (const std::size_t rn : sizes) {
            const std::vector<std::uint32_t> left = iota(next, ln);
            const std::vector<std::uint32_t> right = iota(next + 100'000, rn);
            next += 7;
            const Tree<B> l = Tree<B>::fromItems(left);
            const Tree<B> r = Tree<B>::fromItems(right);
            std::vector<std::uint32_t> both = left;
            both.insert(both.end(), right.begin(), right.end());
            const Tree<B> joined = Tree<B>::concat(l, r);
            ASSERT_TRUE(matches(joined, both)) << ln << " + " << rn;
            // The inputs are unchanged (persistence) and the result is no taller than needed + 1.
            ASSERT_TRUE(matches(l, left));
            ASSERT_TRUE(matches(r, right));
            EXPECT_LE(joined.height(), std::max(l.height(), r.height()) + 1);
        }
    }
    // Underfull internal roots from splits, joined on both sides.
    const std::vector<std::uint32_t> base = iota(1, 200);
    const Tree<B> tree = Tree<B>::fromItems(base);
    for (std::uint64_t k : {1u, 3u, 17u, 100u, 199u}) {
        const auto [a, b] = splitAt(tree, k);
        const Tree<B> small = Tree<B>::fromItems(iota(5000, 2 * B + 1));
        std::vector<std::uint32_t> expected(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(k));
        const std::vector<std::uint32_t> mid = iota(5000, 2 * B + 1);
        expected.insert(expected.end(), mid.begin(), mid.end());
        expected.insert(expected.end(), base.begin() + static_cast<std::ptrdiff_t>(k), base.end());
        ASSERT_TRUE(matches(Tree<B>::concat(Tree<B>::concat(a, small), b), expected)) << "k = " << k;
        ASSERT_TRUE(matches(Tree<B>::concat(a, Tree<B>::concat(small, b)), expected)) << "k = " << k;
    }
}

TYPED_TEST(SumTreeTest, SplitEveryIndex) {
    constexpr int B = TypeParam::value;
    for (std::size_t n = 0; n <= 40; ++n) {
        const std::vector<std::uint32_t> items = iota(10, n);
        const Tree<B> tree = Tree<B>::fromItems(items);
        for (std::size_t k = 0; k <= n; ++k) {
            const auto [l, r] = splitAt(tree, k);
            // matches() runs checkInvariants, which rejects an internal root with one child.
            ASSERT_TRUE(
                matches(l, std::vector<std::uint32_t>(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(k))))
                << n << " split at " << k;
            ASSERT_TRUE(
                matches(r, std::vector<std::uint32_t>(items.begin() + static_cast<std::ptrdiff_t>(k), items.end())))
                << n << " split at " << k;
            ASSERT_TRUE(matches(Tree<B>::concat(l, r), items));
        }
        // withoutFront and withoutBack.
        if (n > 0) {
            ASSERT_TRUE(matches(tree.withoutFront(), std::vector<std::uint32_t>(items.begin() + 1, items.end())));
            ASSERT_TRUE(matches(tree.withoutBack(), std::vector<std::uint32_t>(items.begin(), items.end() - 1)));
        }
        ASSERT_TRUE(matches(tree, items)) << "the source changed";
    }
}

TYPED_TEST(SumTreeTest, SeekMonotone) {
    constexpr int B = TypeParam::value;
    SplitMix64 rng(42);
    for (std::size_t n : {0u, 1u, 5u, 16u, 17u, 64u, 200u}) {
        std::vector<std::uint32_t> items(n);
        for (std::uint32_t& item : items) {
            item = static_cast<std::uint32_t>(rng.below(10)); // Zeros make runs of equal prefixes.
        }
        const Tree<B> tree = Tree<B>::fromItems(items);
        const std::uint64_t total = fold(items).sum;
        for (std::uint64_t target = 0; target <= total + 1; ++target) {
            const auto cursor = tree.seek([target](const TestSummary& s) { return s.sum > target; });
            // The first index whose running sum exceeds target.
            std::uint64_t running = 0;
            std::size_t expected = n;
            for (std::size_t i = 0; i < n; ++i) {
                running += items[i];
                if (running > target) {
                    expected = i;
                    break;
                }
            }
            if (expected == n) {
                ASSERT_EQ(cursor.item(), nullptr) << "target " << target;
                ASSERT_EQ(cursor.prefix(), tree.summary()) << "a miss reports the whole summary";
            } else {
                ASSERT_NE(cursor.item(), nullptr) << "target " << target;
                ASSERT_EQ(*cursor.item(), items[expected]);
                ASSERT_EQ(cursor.prefix(), fold(std::span<const std::uint32_t>(items).first(expected)))
                    << "target " << target;
            }
        }
    }
}

TYPED_TEST(SumTreeTest, CursorIteration) {
    constexpr int B = TypeParam::value;
    for (std::size_t n : {0u, 1u, 2u, 7u, 33u, 300u}) {
        const std::vector<std::uint32_t> items = iota(3, n);
        const Tree<B> tree = Tree<B>::fromItems(items);
        auto cursor = tree.first();
        for (std::size_t i = 0; i < n; ++i) {
            ASSERT_NE(cursor.item(), nullptr);
            ASSERT_EQ(*cursor.item(), items[i]);
            ASSERT_EQ(cursor.prefix(), fold(std::span<const std::uint32_t>(items).first(i)));
            ASSERT_EQ(cursor.next(), i + 1 < n);
        }
        EXPECT_EQ(cursor.item(), nullptr);
        EXPECT_EQ(cursor.prefix(), tree.summary());
        EXPECT_FALSE(cursor.next());
        // Iteration from a seek position visits the rest.
        for (std::size_t k = 0; k < n; k += 1 + n / 8) {
            auto from = tree.seek([k](const TestSummary& s) { return s.count > k; });
            std::vector<std::uint32_t> rest;
            for (; from.item() != nullptr; from.next()) {
                rest.push_back(*from.item());
            }
            EXPECT_EQ(rest, std::vector<std::uint32_t>(items.begin() + static_cast<std::ptrdiff_t>(k), items.end()));
        }
    }
}

TYPED_TEST(SumTreeTest, Persistence) {
    constexpr int B = TypeParam::value;
    SplitMix64 rng(7);
    std::vector<std::pair<Tree<B>, std::vector<std::uint32_t>>> versions;
    std::vector<std::uint32_t> mirror = iota(0, 50);
    Tree<B> tree = Tree<B>::fromItems(mirror);
    for (int op = 0; op < 400; ++op) {
        versions.emplace_back(tree, mirror);
        const std::uint64_t k = rng.below(mirror.size() + 1);
        const auto [l, r] = splitAt(tree, k);
        if (rng.below(2) == 0 || mirror.size() < 4) {
            const std::uint32_t item = static_cast<std::uint32_t>(rng.next());
            tree = Tree<B>::concat(Tree<B>::concat(l, Tree<B>::fromItems(std::span(&item, 1))), r);
            mirror.insert(mirror.begin() + static_cast<std::ptrdiff_t>(k), item);
        } else {
            tree = Tree<B>::concat(l, r.withoutFront());
            if (k < mirror.size()) {
                mirror.erase(mirror.begin() + static_cast<std::ptrdiff_t>(k));
            }
        }
    }
    for (const auto& [old, items] : versions) {
        ASSERT_TRUE(matches(old, items));
    }
    ASSERT_TRUE(matches(tree, mirror));
}

// 20k random ops at B = 2 (the smallest fan-out has the most rebalancing), invariants after each.
TEST(SumTree, RandomOpsVsVector) {
    constexpr int B = 2;
    constexpr int kOps = 20'000;
    constexpr std::size_t kMaxItems = 256;
    SplitMix64 rng(2026);
    std::vector<std::uint32_t> mirror;
    Tree<B> tree;
    std::uint32_t next = 1;
    for (int op = 0; op < kOps; ++op) {
        const std::uint64_t roll = rng.below(100);
        const std::uint64_t k = rng.below(mirror.size() + 1);
        if (roll < 45 && mirror.size() < kMaxItems) {
            // Insert a run of 1..2B+1 items at k.
            const std::size_t run = 1 + static_cast<std::size_t>(rng.below(2 * B + 1));
            const std::vector<std::uint32_t> items = iota(next, run);
            next += static_cast<std::uint32_t>(run);
            const auto [l, r] = splitAt(tree, k);
            tree = Tree<B>::concat(Tree<B>::concat(l, Tree<B>::fromItems(items)), r);
            mirror.insert(mirror.begin() + static_cast<std::ptrdiff_t>(k), items.begin(), items.end());
        } else if (roll < 85) {
            // Erase [k, k + len).
            const std::size_t len = std::min<std::size_t>(rng.below(9), mirror.size() - k);
            const auto [l, rest] = splitAt(tree, k);
            const auto [middle, r] = splitAt(rest, len);
            tree = Tree<B>::concat(l, r);
            mirror.erase(mirror.begin() + static_cast<std::ptrdiff_t>(k),
                         mirror.begin() + static_cast<std::ptrdiff_t>(k + len));
        } else if (roll < 90) {
            tree = tree.withoutFront();
            if (!mirror.empty()) {
                mirror.erase(mirror.begin());
            }
        } else if (roll < 95) {
            tree = tree.withoutBack();
            if (!mirror.empty()) {
                mirror.pop_back();
            }
        } else {
            // Rotate: move the first k items to the end.
            const auto [l, r] = splitAt(tree, k);
            tree = Tree<B>::concat(r, l);
            std::rotate(mirror.begin(), mirror.begin() + static_cast<std::ptrdiff_t>(k), mirror.end());
        }
        if (auto invariants = tree.checkInvariants(); !invariants) {
            FAIL() << "op " << op << ": " << invariants.error().describe();
        }
        ASSERT_EQ(tree.summary(), fold(mirror)) << "op " << op;
        if (op % 16 == 0) {
            ASSERT_EQ(itemsOf(tree), mirror) << "op " << op;
        }
    }
    ASSERT_TRUE(matches(tree, mirror));
}

} // namespace
