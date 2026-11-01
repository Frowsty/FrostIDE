// SPDX-License-Identifier: Apache-2.0
//
// SumTree<Traits, B>: a persistent, reference-counted B+tree of items whose nodes cache the
// summaries of their subtrees (ADR 0012, D3). frost::text builds the piece tree on it; the display
// map (a later slice) reuses it. Header-only template.
/*
 * Design. Items live in leaves; every node caches the monoid summary of its subtree, and
 * internal nodes also keep their children's summaries in an array beside the child pointers, so
 * a descent scans contiguous summaries. Nodes hold B..2B entries (the root 1..2B, an internal root
 * at least 2), all leaves are at the same depth, and nodes are immutable once constructed: their
 * reference count is their only mutable field. Every update builds new nodes along the touched
 * paths and shares everything else, so a copy of a tree is an O(1) snapshot and older versions
 * stay valid. Split and concat normalise roots: a tree of zero items has no root, and an internal
 * root with one child is replaced by that child.
 *
 * Sources (clean room): the cached child summaries beside the child pointers, the seek by a
 * monotone predicate over running prefix summaries (Zed's "Dimension" seek), the height-aware
 * append that pushes the shorter tree down the taller tree's edge and splits overflowing nodes
 * upward (push_tree), and small fan-outs in tests (Zed uses TREE_BASE 2 in tests, 6 otherwise)
 * follow Zed's crates/sum_tree (https://github.com/zed-industries/zed, commit d544470, 2026-10-05,
 * whose crates/sum_tree/Cargo.toml declares license = "Apache-2.0"). The code here is written from
 * that description, not translated; ADR 0012 records the sources.
 *
 * Fan-out: B = 8 (8..16 entries) by default; tests also instantiate B = 2 and 3.
 *
 * Thread safety: const members are safe from any number of threads. Copies are O(1) and as
 * thread-safe as std::shared_ptr copies; never assign a SumTree object while another thread reads
 * that same object. Seeks and cursors take no references and touch no counters.
 *
 * Allocation failure terminates the process (the members are noexcept or only called from
 * noexcept code), as in frost::sched.
 */
#pragma once

#include "frost/core/assert.hpp"
#include "frost/core/error.hpp"
#include "frost/text/detail/ref.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace frost::text {

namespace detail {
// True when a value-initialised S is a constant expression (S is a literal type with a constexpr
// default constructor), so the empty tree's summary can be a static constexpr object.
template<class S>
concept ConstantInitializable = requires { typename std::bool_constant<(static_cast<void>(S{}), true)>; };
} // namespace detail

/*
 * Requirements on Traits:
 *   - Traits::Item is copyable and default-initializable (leaves hold arrays of items);
 *   - Traits::Summary is regular, trivially destructible and a literal type whose value-initialised
 *     object is a constant expression; that object is the identity of combine;
 *   - Traits::summary(item) returns the item's summary in O(1);
 *   - Traits::combine(a, b) is associative, with identity Summary{}.
 */
template<class T>
concept SumTreeTraits =
    requires {
        typename T::Item;
        typename T::Summary;
    } && std::copyable<typename T::Item> && std::default_initializable<typename T::Item> &&
    std::regular<typename T::Summary> && std::is_trivially_destructible_v<typename T::Summary> &&
    detail::ConstantInitializable<typename T::Summary> &&
    requires(const typename T::Item& item, const typename T::Summary& s) {
        { T::summary(item) } -> std::convertible_to<typename T::Summary>;
        { T::combine(s, s) } -> std::convertible_to<typename T::Summary>;
    };

namespace detail {

template<class Traits, int B>
struct SumTreeLeaf;
template<class Traits, int B>
struct SumTreeInternal;

// The header every node starts with: reference count, height (0 for a leaf), entry count and the
// subtree's summary. The node kind follows from the height; nodes have no virtual functions, so
// destroy() deletes the concrete kind. Exposed for the undo slice's structural diff (node
// identity) and for frost::text's own descents; not public API.
template<class Traits, int B>
struct SumTreeNode : RefCounted {
    using Summary = typename Traits::Summary;

    std::uint8_t height = 0;
    std::uint8_t count = 0;
    Summary summary{};

    // Deletes `node` as the leaf or internal node its height says it is. Called by Ref when the
    // last reference drops; the children of an internal node are released recursively (depth at
    // most the height).
    static void destroy(SumTreeNode* node) noexcept;
};

// A leaf: up to 2B items.
template<class Traits, int B>
struct SumTreeLeaf final : SumTreeNode<Traits, B> {
    using Item = typename Traits::Item;

    std::array<Item, static_cast<std::size_t>(2 * B)> items{};

    // Copies `source` (1..2B items) and folds their summaries. O(B).
    explicit SumTreeLeaf(std::span<const Item> source) noexcept {
        FROST_ASSERT(!source.empty() && source.size() <= items.size());
        typename Traits::Summary folded{};
        for (std::size_t i = 0; i < source.size(); ++i) {
            items[i] = source[i];
            folded = Traits::combine(folded, Traits::summary(source[i]));
        }
        this->height = 0;
        this->count = static_cast<std::uint8_t>(source.size());
        this->summary = folded;
    }
};

// An internal node: up to 2B children of equal height, with their summaries beside them.
template<class Traits, int B>
struct SumTreeInternal final : SumTreeNode<Traits, B> {
    using Node = SumTreeNode<Traits, B>;
    using Summary = typename Traits::Summary;

    std::array<Summary, static_cast<std::size_t>(2 * B)> childSummaries{};
    std::array<Ref<Node>, static_cast<std::size_t>(2 * B)> children{};

    // Takes the references in `source` (1..2B non-null children of one height, moved from) and
    // folds their summaries. O(B).
    explicit SumTreeInternal(std::span<Ref<Node>> source) noexcept {
        FROST_ASSERT(!source.empty() && source.size() <= children.size());
        const std::uint8_t childHeight = source[0]->height;
        typename Traits::Summary folded{};
        for (std::size_t i = 0; i < source.size(); ++i) {
            FROST_ASSERT(source[i] && source[i]->height == childHeight);
            childSummaries[i] = source[i]->summary;
            folded = Traits::combine(folded, childSummaries[i]);
            children[i] = std::move(source[i]);
        }
        this->height = static_cast<std::uint8_t>(childHeight + 1);
        this->count = static_cast<std::uint8_t>(source.size());
        this->summary = folded;
    }
};

template<class Traits, int B>
void SumTreeNode<Traits, B>::destroy(SumTreeNode* node) noexcept {
    if (node->height == 0) {
        delete static_cast<SumTreeLeaf<Traits, B>*>(node);
    } else {
        delete static_cast<SumTreeInternal<Traits, B>*>(node);
    }
}

} // namespace detail

template<SumTreeTraits Traits, int B = 8>
class SumTree {
    static_assert(B >= 2 && 2 * B <= 255, "SumTree: B must be at least 2 and 2B must fit an 8-bit count");

public:
    using Item = typename Traits::Item;
    using Summary = typename Traits::Summary;
    using Node = detail::SumTreeNode<Traits, B>;
    using Leaf = detail::SumTreeLeaf<Traits, B>;
    using Internal = detail::SumTreeInternal<Traits, B>;

    static constexpr int kMinEntries = B;
    static constexpr int kMaxEntries = 2 * B;
    // Cursors keep one frame per level in a fixed array of kMaxDepth frames, so a tree's height
    // is at most kMaxDepth - 1 (2^47 items even at B = 2).
    static constexpr std::uint32_t kMaxDepth = 48;

    /*
     * A forward cursor over the items, positioned by seek() or first(). It borrows: it holds no
     * reference and is valid only while the tree it came from, or a copy of it, is alive (never
     * seek on a temporary tree). Copying a cursor is O(depth) and touches no counter. Not
     * thread-safe as an object; distinct cursors over one tree may be used on any threads.
     */
    class Cursor {
    public:
        // The end cursor of an empty tree.
        Cursor() noexcept = default;

        // The current item, or nullptr at the end.
        [[nodiscard]] const Item* item() const noexcept { return m_item; }
        // The summary of every item before the current one (the whole tree's summary at the end).
        [[nodiscard]] const Summary& prefix() const noexcept { return m_prefix; }

        // Moves to the next item; returns false (and becomes the end cursor) when there is none.
        // Amortised O(1), worst case O(height); no allocation.
        bool next() noexcept {
            if (m_item == nullptr) {
                return false;
            }
            m_prefix = Traits::combine(m_prefix, Traits::summary(*m_item));
            Frame& leafFrame = m_frames[m_depth - 1];
            const auto* leaf = static_cast<const Leaf*>(leafFrame.node);
            if (++leafFrame.index < leaf->count) {
                m_item = &leaf->items[leafFrame.index];
                return true;
            }
            for (std::uint32_t level = m_depth - 1; level > 0;) {
                --level;
                Frame& frame = m_frames[level];
                const auto* internal = static_cast<const Internal*>(frame.node);
                if (++frame.index < internal->count) {
                    descendLeftmost(internal->children[frame.index].get(), level + 1);
                    return true;
                }
            }
            m_item = nullptr;
            m_depth = 0;
            return false;
        }

    private:
        friend class SumTree;

        struct Frame {
            const Node* node = nullptr;
            std::uint8_t index = 0;
        };

        // Pushes frames from `node` (at frame `level`) down its leftmost path to the first item.
        void descendLeftmost(const Node* node, std::uint32_t level) noexcept {
            while (node->height > 0) {
                FROST_ASSERT(level < kMaxDepth);
                m_frames[level++] = {node, 0};
                node = static_cast<const Internal*>(node)->children[0].get();
            }
            FROST_ASSERT(level < kMaxDepth);
            m_frames[level++] = {node, 0};
            m_depth = level;
            m_item = &static_cast<const Leaf*>(node)->items[0];
        }

        std::array<Frame, kMaxDepth> m_frames{};
        std::uint32_t m_depth = 0;
        const Item* m_item = nullptr;
        Summary m_prefix{};
    };

    // The empty tree. O(1).
    SumTree() noexcept = default;

    // A balanced tree of `items` in order, built bottom-up: every level is split as evenly as
    // possible into nodes of B..2B entries. O(n).
    [[nodiscard]] static SumTree fromItems(std::span<const Item> items) noexcept {
        if (items.empty()) {
            return {};
        }
        constexpr std::size_t kMax = static_cast<std::size_t>(kMaxEntries);
        std::vector<detail::Ref<Node>> level;
        const std::size_t n = items.size();
        const std::size_t leaves = (n + kMax - 1) / kMax;
        level.reserve(leaves);
        for (std::size_t j = 0; j < leaves; ++j) {
            const std::size_t begin = j * n / leaves;
            const std::size_t end = (j + 1) * n / leaves;
            level.push_back(makeLeaf(items.subspan(begin, end - begin)));
        }
        while (level.size() > 1) {
            const std::size_t m = level.size();
            const std::size_t parents = (m + kMax - 1) / kMax;
            std::vector<detail::Ref<Node>> up;
            up.reserve(parents);
            for (std::size_t j = 0; j < parents; ++j) {
                const std::size_t begin = j * m / parents;
                const std::size_t end = (j + 1) * m / parents;
                up.push_back(makeInternal(std::span<detail::Ref<Node>>(level).subspan(begin, end - begin)));
            }
            level = std::move(up);
        }
        return SumTree(std::move(level[0]));
    }

    [[nodiscard]] bool empty() const noexcept { return !m_root; }

    // The summary of all items; the empty tree returns a static constexpr Summary{}. O(1).
    [[nodiscard]] const Summary& summary() const noexcept { return m_root ? m_root->summary : kEmptySummary; }

    // 0 for the empty tree and for a single leaf. O(1).
    [[nodiscard]] std::uint32_t height() const noexcept { return m_root ? m_root->height : 0u; }

    // The items of `l` followed by the items of `r`. O(|h(l) - h(r)| + 1) node constructions of
    // O(B) each, so O(B log n); shares every untouched node of both trees.
    [[nodiscard]] static SumTree concat(const SumTree& l, const SumTree& r) noexcept {
        if (l.empty()) {
            return r;
        }
        if (r.empty()) {
            return l;
        }
        NodePair pair = l.height() >= r.height() ? appendRight(l.m_root, r.m_root) : prependLeft(r.m_root, l.m_root);
        if (!pair.second) {
            return SumTree(std::move(pair.first));
        }
        std::array<detail::Ref<Node>, 2> both{std::move(pair.first), std::move(pair.second)};
        return SumTree(makeInternal(both));
    }

    /*
     * Splits the items at the first one whose `pred(prefix through that item)` is true: the left
     * tree holds the items before it, the right tree that item and the rest. `pred` takes a const
     * Summary& and must be monotone along the sequence (false...false, true...true). Both results
     * are normalised. O(B log n).
     */
    template<class Pred>
    [[nodiscard]] std::pair<SumTree, SumTree> split(Pred pred) const noexcept {
        if (empty()) {
            return {};
        }
        return splitNode(m_root, pred, Summary{});
    }

    /*
     * A cursor at the first item whose `pred(prefix through that item)` is true, with prefix() the
     * summary before it; the end cursor (item() == nullptr, prefix() == summary()) when no item
     * matches. Same monotonicity requirement as split(). O(B log n); borrows (see Cursor).
     */
    template<class Pred>
    [[nodiscard]] Cursor seek(Pred pred) const noexcept {
        Cursor cursor;
        if (empty()) {
            return cursor;
        }
        if (!pred(m_root->summary)) {
            cursor.m_prefix = m_root->summary;
            return cursor;
        }
        FROST_ASSERT(m_root->height < kMaxDepth);
        const Node* node = m_root.get();
        Summary acc{};
        std::uint32_t level = 0;
        while (node->height > 0) {
            const auto* internal = static_cast<const Internal*>(node);
            int c = 0;
            for (; c + 1 < internal->count; ++c) {
                const Summary next = Traits::combine(acc, internal->childSummaries[static_cast<std::size_t>(c)]);
                if (pred(next)) {
                    break;
                }
                acc = next;
            }
            cursor.m_frames[level++] = {node, static_cast<std::uint8_t>(c)};
            node = internal->children[static_cast<std::size_t>(c)].get();
        }
        const auto* leaf = static_cast<const Leaf*>(node);
        int i = 0;
        for (; i + 1 < leaf->count; ++i) {
            const Summary next = Traits::combine(acc, Traits::summary(leaf->items[static_cast<std::size_t>(i)]));
            if (pred(next)) {
                break;
            }
            acc = next;
        }
        cursor.m_frames[level++] = {node, static_cast<std::uint8_t>(i)};
        cursor.m_depth = level;
        cursor.m_item = &leaf->items[static_cast<std::size_t>(i)];
        cursor.m_prefix = acc;
        return cursor;
    }

    // A cursor at the first item (the end cursor for the empty tree). O(height); borrows.
    [[nodiscard]] Cursor first() const noexcept {
        Cursor cursor;
        if (!empty()) {
            cursor.descendLeftmost(m_root.get(), 0);
        }
        return cursor;
    }

    // The first and last items, or nullptr for the empty tree. O(height).
    [[nodiscard]] const Item* front() const noexcept {
        if (empty()) {
            return nullptr;
        }
        const Node* node = m_root.get();
        while (node->height > 0) {
            node = static_cast<const Internal*>(node)->children[0].get();
        }
        return &static_cast<const Leaf*>(node)->items[0];
    }
    [[nodiscard]] const Item* back() const noexcept {
        if (empty()) {
            return nullptr;
        }
        const Node* node = m_root.get();
        while (node->height > 0) {
            const auto* internal = static_cast<const Internal*>(node);
            node = internal->children[internal->count - 1u].get();
        }
        const auto* leaf = static_cast<const Leaf*>(node);
        return &leaf->items[leaf->count - 1u];
    }

    // The tree without its first or last item (the empty tree stays empty). O(B log n).
    [[nodiscard]] SumTree withoutFront() const noexcept { return empty() ? SumTree() : removeFirst(m_root); }
    [[nodiscard]] SumTree withoutBack() const noexcept { return empty() ? SumTree() : removeLast(m_root); }

    /*
     * Checks the structural invariants and returns ErrorCode::Internal naming the first violation:
     *   S1 every leaf is at depth root.height, and each child's height is its parent's minus one;
     *   S2 a non-root node holds B..2B entries; the root 1..2B, an internal root at least 2;
     *   S3 each node's summary is the fold of its entries, and childSummaries[i] equals
     *      children[i]->summary;
     *   S4 every reachable node has refs >= 1.
     * O(n); safe from any thread. For tests and fuzzing.
     */
    [[nodiscard]] Result<void> checkInvariants() const {
        if (empty()) {
            return {};
        }
        if (m_root->height >= kMaxDepth) {
            return fail(ErrorCode::Internal,
                        std::format("S1: root height {} exceeds the cursor depth", m_root->height));
        }
        if (m_root->height > 0 && m_root->count < 2) {
            return fail(ErrorCode::Internal, "S2: internal root with fewer than 2 children");
        }
        return checkNode(m_root.get(), m_root->height, 0, true);
    }

    // Node access for frost::text's own descents and the undo slice's structural diff (node
    // identity). Not public API: nullptr for the empty tree.
    [[nodiscard]] const Node* rootNode() const noexcept { return m_root.get(); }

    // Two trees are identical when they share the same root node (or are both empty). O(1).
    [[nodiscard]] bool sharesRootWith(const SumTree& other) const noexcept { return m_root == other.m_root; }

private:
    using NodeRef = detail::Ref<Node>;

    // One or two sibling nodes of one height, left to right (second may be null).
    struct NodePair {
        NodeRef first;
        NodeRef second;
    };

    explicit SumTree(NodeRef root) noexcept : m_root(std::move(root)) {}

    static NodeRef makeLeaf(std::span<const Item> items) noexcept { return NodeRef::template make<Leaf>(items); }
    static NodeRef makeInternal(std::span<NodeRef> children) noexcept {
        return NodeRef::template make<Internal>(children);
    }

    static const Leaf& asLeaf(const NodeRef& node) noexcept { return *static_cast<const Leaf*>(node.get()); }
    static const Internal& asInternal(const NodeRef& node) noexcept {
        return *static_cast<const Internal*>(node.get());
    }

    // Children [from, to) of `node` as a normalised tree: none is empty, one is that child.
    static SumTree childrenTree(const Internal& node, int from, int to) noexcept {
        if (from >= to) {
            return {};
        }
        if (to - from == 1) {
            return SumTree(node.children[static_cast<std::size_t>(from)]);
        }
        std::array<NodeRef, static_cast<std::size_t>(2 * B)> copies;
        for (int i = from; i < to; ++i) {
            copies[static_cast<std::size_t>(i - from)] = node.children[static_cast<std::size_t>(i)];
        }
        return SumTree(makeInternal(std::span<NodeRef>(copies.data(), static_cast<std::size_t>(to - from))));
    }

    // Items [from, to) of `leaf` as a tree.
    static SumTree itemsTree(const Leaf& leaf, int from, int to) noexcept {
        if (from >= to) {
            return {};
        }
        return SumTree(makeLeaf(std::span<const Item>(leaf.items.data() + from, static_cast<std::size_t>(to - from))));
    }

    // Packs 1..2B+1 sibling nodes into one parent, or two parents of B..B+1 when there are 2B+1.
    static NodePair pack(std::span<NodeRef> nodes) noexcept {
        if (nodes.size() <= static_cast<std::size_t>(kMaxEntries)) {
            return {makeInternal(nodes), {}};
        }
        const std::size_t half = nodes.size() / 2;
        return {makeInternal(nodes.first(half)), makeInternal(nodes.subspan(half))};
    }

    /*
     * Two adjacent nodes a, b of one height as one or two valid siblings: unchanged when both
     * already hold at least B entries, merged into one node when the entries fit in 2B, otherwise
     * redistributed evenly into two nodes (each then holds more than B, since there are > 2B).
     */
    static NodePair mergeSiblings(const NodeRef& a, const NodeRef& b) noexcept {
        if (a->count >= kMinEntries && b->count >= kMinEntries) {
            return {a, b};
        }
        const std::size_t total = static_cast<std::size_t>(a->count) + b->count;
        const std::size_t half = total <= static_cast<std::size_t>(kMaxEntries) ? total : total / 2;
        if (a->height == 0) {
            std::array<Item, static_cast<std::size_t>(4 * B)> items;
            std::size_t k = 0;
            for (std::size_t i = 0; i < a->count; ++i) {
                items[k++] = asLeaf(a).items[i];
            }
            for (std::size_t i = 0; i < b->count; ++i) {
                items[k++] = asLeaf(b).items[i];
            }
            const std::span<const Item> all(items.data(), total);
            if (half == total) {
                return {makeLeaf(all), {}};
            }
            return {makeLeaf(all.first(half)), makeLeaf(all.subspan(half))};
        }
        std::array<NodeRef, static_cast<std::size_t>(4 * B)> children;
        std::size_t k = 0;
        for (std::size_t i = 0; i < a->count; ++i) {
            children[k++] = asInternal(a).children[i];
        }
        for (std::size_t i = 0; i < b->count; ++i) {
            children[k++] = asInternal(b).children[i];
        }
        const std::span<NodeRef> all(children.data(), total);
        if (half == total) {
            return {makeInternal(all), {}};
        }
        return {makeInternal(all.first(half)), makeInternal(all.subspan(half))};
    }

    // Appends `other` (height <= node's) after the last leaf of `node`'s subtree: walks down the
    // right spine to other's height, merges there and propagates overflow upward. Returns one or
    // two nodes at node's height.
    static NodePair appendRight(const NodeRef& node, const NodeRef& other) noexcept {
        if (node->height == other->height) {
            return mergeSiblings(node, other);
        }
        const Internal& internal = asInternal(node);
        const std::size_t count = internal.count;
        NodePair below = appendRight(internal.children[count - 1], other);
        std::array<NodeRef, static_cast<std::size_t>(2 * B + 1)> list;
        std::size_t k = 0;
        for (std::size_t i = 0; i + 1 < count; ++i) {
            list[k++] = internal.children[i];
        }
        list[k++] = std::move(below.first);
        if (below.second) {
            list[k++] = std::move(below.second);
        }
        return pack(std::span<NodeRef>(list.data(), k));
    }

    // The mirror image: prepends `other` (height <= node's) before the first leaf of `node`.
    static NodePair prependLeft(const NodeRef& node, const NodeRef& other) noexcept {
        if (node->height == other->height) {
            return mergeSiblings(other, node);
        }
        const Internal& internal = asInternal(node);
        const std::size_t count = internal.count;
        NodePair below = prependLeft(internal.children[0], other);
        std::array<NodeRef, static_cast<std::size_t>(2 * B + 1)> list;
        std::size_t k = 0;
        list[k++] = std::move(below.first);
        if (below.second) {
            list[k++] = std::move(below.second);
        }
        for (std::size_t i = 1; i < count; ++i) {
            list[k++] = internal.children[i];
        }
        return pack(std::span<NodeRef>(list.data(), k));
    }

    // split() below `node`, whose items follow `prefix`. Left = children before the matching one
    // concatenated with the left part of the recursion; right = the recursion's right part
    // concatenated with the children after it. The concat costs telescope to O(B log n).
    template<class Pred>
    static std::pair<SumTree, SumTree> splitNode(const NodeRef& node, Pred& pred, const Summary& prefix) noexcept {
        if (node->height == 0) {
            const Leaf& leaf = asLeaf(node);
            Summary acc = prefix;
            int i = 0;
            for (; i < leaf.count; ++i) {
                acc = Traits::combine(acc, Traits::summary(leaf.items[static_cast<std::size_t>(i)]));
                if (pred(acc)) {
                    break;
                }
            }
            if (i == 0) {
                return {SumTree(), SumTree(node)};
            }
            if (i == leaf.count) {
                return {SumTree(node), SumTree()};
            }
            return {itemsTree(leaf, 0, i), itemsTree(leaf, i, leaf.count)};
        }
        const Internal& internal = asInternal(node);
        Summary acc = prefix;
        int c = 0;
        for (; c < internal.count; ++c) {
            const Summary next = Traits::combine(acc, internal.childSummaries[static_cast<std::size_t>(c)]);
            if (pred(next)) {
                break;
            }
            acc = next;
        }
        if (c == internal.count) {
            return {SumTree(node), SumTree()};
        }
        auto [left, right] = splitNode(internal.children[static_cast<std::size_t>(c)], pred, acc);
        if (c == 0 && left.empty()) {
            return {SumTree(), SumTree(node)};
        }
        return {concat(childrenTree(internal, 0, c), left),
                concat(right, childrenTree(internal, c + 1, internal.count))};
    }

    static SumTree removeFirst(const NodeRef& node) noexcept {
        if (node->height == 0) {
            const Leaf& leaf = asLeaf(node);
            return itemsTree(leaf, 1, leaf.count);
        }
        const Internal& internal = asInternal(node);
        return concat(removeFirst(internal.children[0]), childrenTree(internal, 1, internal.count));
    }

    static SumTree removeLast(const NodeRef& node) noexcept {
        if (node->height == 0) {
            const Leaf& leaf = asLeaf(node);
            return itemsTree(leaf, 0, leaf.count - 1);
        }
        const Internal& internal = asInternal(node);
        const int count = internal.count;
        return concat(childrenTree(internal, 0, count - 1),
                      removeLast(internal.children[static_cast<std::size_t>(count - 1)]));
    }

    static Result<void> checkNode(const Node* node, std::uint32_t expectedHeight, std::uint32_t depth, bool isRoot) {
        if (node->m_refs.load(std::memory_order_relaxed) < 1) {
            return fail(ErrorCode::Internal, std::format("S4: node at depth {} has no references", depth));
        }
        if (node->height != expectedHeight) {
            return fail(ErrorCode::Internal, std::format("S1: node at depth {} has height {}, expected {}", depth,
                                                         node->height, expectedHeight));
        }
        const int minEntries = isRoot ? 1 : kMinEntries;
        if (node->count < minEntries || node->count > kMaxEntries) {
            return fail(ErrorCode::Internal, std::format("S2: node at depth {} holds {} entries, expected {}..{}",
                                                         depth, node->count, minEntries, kMaxEntries));
        }
        Summary folded{};
        if (node->height == 0) {
            const auto* leaf = static_cast<const Leaf*>(node);
            for (std::size_t i = 0; i < leaf->count; ++i) {
                folded = Traits::combine(folded, Traits::summary(leaf->items[i]));
            }
        } else {
            const auto* internal = static_cast<const Internal*>(node);
            for (std::size_t i = 0; i < internal->count; ++i) {
                const Node* child = internal->children[i].get();
                if (child == nullptr) {
                    return fail(ErrorCode::Internal, std::format("S1: null child {} at depth {}", i, depth));
                }
                if (!(internal->childSummaries[i] == child->summary)) {
                    return fail(
                        ErrorCode::Internal,
                        std::format("S3: childSummaries[{}] differs from the child's summary at depth {}", i, depth));
                }
                folded = Traits::combine(folded, child->summary);
                if (auto result = checkNode(child, expectedHeight - 1, depth + 1, false); !result) {
                    return result;
                }
            }
        }
        if (!(folded == node->summary)) {
            return fail(ErrorCode::Internal,
                        std::format("S3: node summary at depth {} differs from the fold of its entries", depth));
        }
        return {};
    }

    static constexpr Summary kEmptySummary{};

    NodeRef m_root;
};

} // namespace frost::text
