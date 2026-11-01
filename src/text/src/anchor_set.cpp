// SPDX-License-Identifier: Apache-2.0
//
// AnchorSet (frost/text/anchor_set.hpp): class-tree splices per edit step, the sweep path for
// large batches, label assignment with Bender et al.'s relabelling, moves and the invariants
// (ADR 0017, decisions 2 and 3).
/*
 * Order labels (decision 3). Labels are u64 values in [1, 2^64 - 2]; 0 and 2^64 - 1 stand for "no
 * neighbour". A new label between lo and hi (exclusive) is lo + min((hi - lo) / 2, 2^32); the cap
 * keeps append-heavy patterns (hi = 2^64 - 1) cheap for 2^32 appends. When hi - lo < 2, relabel()
 * applies the simplified order-maintenance algorithm of Bender, Cole, Demaine, Farach-Colton and
 * Zito ("Two Simplified Algorithms for Maintaining Order in a List", ESA 2002, LNCS 2461) with
 * T = 1.5: for i = 1, 2, ..., 64 take the block of 2^i labels around lo (lo with its low i bits
 * cleared, the whole space at i = 64) and count its entries n_i; at the first i with
 * n_i + 1 <= t_i = floor((4/3)^i), spread the block's entries and one free slot (right after lo)
 * evenly over the block. That gives amortised O(log n) relabelled entries per insertion. The code
 * here is written from the paper's description.
 *
 * Splices (section 4.2 of the slice spec). For one step [a, b) -> n:
 *   Left tree:  split at the first offset > a (anchors at a keep their offset); the middle,
 *               a < offset <= b, collapses to a; in the suffix (offset > b) only the first gap
 *               changes, to its new offset minus the new previous offset.
 *   Right tree: split at the first offset >= a; the middle, a <= offset < b, collapses to a + n;
 *               the suffix (offset >= b) shifts by n - (b - a), again by its first gap only.
 * A middle whose entries already share one offset (its total gap equals its first gap) only gets
 * a new first gap, so deleting the byte before a pile of anchors is O(B log n).
 */

#include "frost/text/anchor_set.hpp"

#include "frost/core/assert.hpp"

#include "batch_mapper.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <tuple>

namespace frost::text {
namespace {

using detail::AnchorEntry;
using detail::AnchorIndex;
using detail::AnchorIndexEntry;
using detail::AnchorIndexSummary;
using detail::AnchorSummary;
using detail::AnchorTree;

constexpr std::uint64_t kNoLabelAbove = ~std::uint64_t{0}; // 2^64 - 1: no right neighbour.
constexpr std::uint64_t kMaxLabel = kNoLabelAbove - 1;     // 2^64 - 2.
constexpr std::uint64_t kLabelStepCap = std::uint64_t{1} << 32;

// t_i = floor((4/3)^i) for a block of 2^i labels (Bender et al., T = 1.5, so 2/T = 4/3). Computed
// at compile time; constexpr double arithmetic is IEEE and identical on both platforms, and the
// table only needs to be monotone.
constexpr std::array<std::uint64_t, 65> kThresholds = [] {
    std::array<std::uint64_t, 65> t{};
    double value = 1.0;
    for (std::uint64_t& entry : t) {
        entry = static_cast<std::uint64_t>(value);
        value = value * 4.0 / 3.0;
    }
    return t;
}();
static_assert(kThresholds[1] == 1 && kThresholds[8] == 9, "t_i = floor((4/3)^i)");
static_assert(kThresholds[64] > AnchorSet::kMaxAnchors, "a relabel of the whole label space always succeeds");

// ceil(log2(x)) for x >= 1.
constexpr std::uint64_t ceilLog2(std::uint64_t x) noexcept { return static_cast<std::uint64_t>(std::bit_width(x - 1)); }

constexpr std::uint64_t shifted(std::uint64_t x, std::int64_t delta) noexcept {
    return delta >= 0 ? x + static_cast<std::uint64_t>(delta) : x - static_cast<std::uint64_t>(-delta);
}

AnchorTree singleEntry(const AnchorEntry& entry) noexcept {
    return AnchorTree::fromItems(std::span<const AnchorEntry>(&entry, 1));
}
AnchorIndex singleIndexEntry(const AnchorIndexEntry& entry) noexcept {
    return AnchorIndex::fromItems(std::span<const AnchorIndexEntry>(&entry, 1));
}

// (entries with offset <= x, the rest).
std::pair<AnchorTree, AnchorTree> splitAfterOffset(const AnchorTree& tree, std::uint64_t x) noexcept {
    return tree.split([x](const AnchorSummary& s) { return s.bytes > x; });
}
// (entries with offset < x, the rest).
std::pair<AnchorTree, AnchorTree> splitBeforeOffset(const AnchorTree& tree, std::uint64_t x) noexcept {
    return tree.split([x](const AnchorSummary& s) { return s.bytes >= x; });
}
// (entries with label < label, the rest).
std::pair<AnchorTree, AnchorTree> splitBeforeLabel(const AnchorTree& tree, std::uint64_t label) noexcept {
    return tree.split([label](const AnchorSummary& s) { return s.maxLabel >= label; });
}
// The number of entries with label < label.
std::uint64_t countBelowLabel(const AnchorTree& tree, std::uint64_t label) noexcept {
    return tree.seek([label](const AnchorSummary& s) { return s.maxLabel >= label; }).prefix().count;
}

// `tree` with its first entry's gap set to `gap`.
AnchorTree withFrontGap(const AnchorTree& tree, std::uint64_t gap) noexcept {
    if (tree.empty() || tree.front()->gap == gap) {
        return tree;
    }
    AnchorEntry entry = *tree.front();
    entry.gap = gap;
    return AnchorTree::concat(singleEntry(entry), tree.withoutFront());
}

// Every entry of `middle` (non-empty) at one offset: the first gap becomes `firstGap`, the rest 0.
AnchorTree collapse(const AnchorTree& middle, std::uint64_t firstGap) noexcept {
    if (middle.summary().bytes == middle.front()->gap) {
        return withFrontGap(middle, firstGap); // Already one offset: O(B log n).
    }
    std::vector<AnchorEntry> items;
    items.reserve(middle.summary().count);
    for (AnchorTree::Cursor c = middle.first(); c.item() != nullptr; c.next()) {
        AnchorEntry entry = *c.item();
        entry.gap = items.empty() ? firstGap : 0;
        items.push_back(entry);
    }
    return AnchorTree::fromItems(items);
}

// One step of the Left class tree (header comment).
void stepLeft(AnchorTree& tree, std::uint64_t a, std::uint64_t b, std::uint64_t n) noexcept {
    if (tree.empty() || tree.summary().bytes <= a) {
        return; // Every entry is at or before a.
    }
    auto [left, rest] = splitAfterOffset(tree, a);
    const std::uint64_t base = left.summary().bytes;
    auto [middle, suffix] = splitAfterOffset(rest, b - base);
    const std::uint64_t oldMiddle = middle.summary().bytes;
    std::uint64_t end = base;
    if (!middle.empty()) {
        middle = collapse(middle, a - base);
        end = a;
    }
    if (!suffix.empty()) {
        const std::uint64_t oldFirst = base + oldMiddle + suffix.front()->gap;
        suffix = withFrontGap(suffix, oldFirst - (b - a) + n - end);
    }
    tree = AnchorTree::concat(AnchorTree::concat(left, middle), suffix);
}

// One step of the Right class tree (header comment).
void stepRight(AnchorTree& tree, std::uint64_t a, std::uint64_t b, std::uint64_t n) noexcept {
    if (tree.empty() || tree.summary().bytes < a) {
        return; // Every entry is before a.
    }
    auto [left, rest] = splitBeforeOffset(tree, a);
    const std::uint64_t base = left.summary().bytes;
    auto [middle, suffix] = splitBeforeOffset(rest, b - base);
    const std::uint64_t oldMiddle = middle.summary().bytes;
    std::uint64_t end = base;
    if (!middle.empty()) {
        middle = collapse(middle, a + n - base);
        end = a + n;
    }
    if (!suffix.empty()) {
        const std::uint64_t oldFirst = base + oldMiddle + suffix.front()->gap;
        suffix = withFrontGap(suffix, oldFirst - (b - a) + n - end);
    }
    tree = AnchorTree::concat(AnchorTree::concat(left, middle), suffix);
}

// Every entry mapped by `mapper` with `bias`, labels kept. O(n).
AnchorTree sweepTree(const AnchorTree& tree, Bias bias, detail::BatchMapper& mapper) noexcept {
    std::vector<AnchorEntry> items;
    items.reserve(tree.summary().count);
    std::uint64_t oldOffset = 0;
    std::uint64_t end = 0;
    for (AnchorTree::Cursor c = tree.first(); c.item() != nullptr; c.next()) {
        AnchorEntry entry = *c.item();
        oldOffset += entry.gap;
        const std::uint64_t offset = mapper.map(oldOffset, bias);
        entry.gap = offset - end;
        end = offset;
        items.push_back(entry);
    }
    return AnchorTree::fromItems(items);
}

// The index entry of `id`, or nullptr.
const AnchorIndexEntry* findIndexEntry(const AnchorIndex& index, std::uint64_t id) noexcept {
    const AnchorIndexEntry* entry = index.seek([id](const AnchorIndexSummary& s) { return s.maxId >= id; }).item();
    return entry != nullptr && entry->id == id ? entry : nullptr;
}

// The class entry with `label` and its offset (the entry must exist).
std::pair<AnchorEntry, std::uint64_t> findByLabel(const AnchorTree& tree, std::uint64_t label) noexcept {
    const AnchorTree::Cursor c = tree.seek([label](const AnchorSummary& s) { return s.maxLabel >= label; });
    FROST_ASSERT(c.item() != nullptr && c.item()->label == label);
    return {*c.item(), c.prefix().bytes + c.item()->gap};
}

// `index` with the label of `id` (which must exist) set to `label`. O(B log n).
AnchorIndex setIndexLabel(const AnchorIndex& index, std::uint64_t id, std::uint64_t label) noexcept {
    auto [left, right] = index.split([id](const AnchorIndexSummary& s) { return s.maxId >= id; });
    FROST_ASSERT(!right.empty() && right.front()->id == id);
    AnchorIndexEntry entry = *right.front();
    entry.label = label;
    return AnchorIndex::concat(AnchorIndex::concat(left, singleIndexEntry(entry)), right.withoutFront());
}

// `tree` without the entry with `label`; its gap goes to the next entry. O(B log n).
AnchorTree removeByLabel(const AnchorTree& tree, std::uint64_t label) noexcept {
    auto [left, right] = splitBeforeLabel(tree, label);
    FROST_ASSERT(!right.empty() && right.front()->label == label);
    const std::uint64_t gap = right.front()->gap;
    AnchorTree rest = right.withoutFront();
    if (!rest.empty()) {
        rest = withFrontGap(rest, rest.front()->gap + gap);
    }
    return AnchorTree::concat(left, rest);
}

bool byOffsetBiasId(const ResolvedAnchor& x, const ResolvedAnchor& y) noexcept {
    return std::tie(x.offset, x.bias, x.id) < std::tie(y.offset, y.bias, y.id);
}

} // namespace

AnchorSet::AnchorSet(std::uint64_t version, ByteOffset textSize) noexcept : m_version(version), m_textSize(textSize) {}

Result<ByteOffset> AnchorSet::resolve(AnchorId id) const noexcept {
    const AnchorIndexEntry* entry = findIndexEntry(m_index, id.value);
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound, std::format("no anchor with id {}", id.value));
    }
    return ByteOffset{findByLabel(tree(entry->bias), entry->label).second};
}

Result<Anchor> AnchorSet::anchor(AnchorId id) const noexcept {
    const AnchorIndexEntry* entry = findIndexEntry(m_index, id.value);
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound, std::format("no anchor with id {}", id.value));
    }
    return Anchor{ByteOffset{findByLabel(tree(entry->bias), entry->label).second}, entry->bias};
}

std::vector<ResolvedAnchor> AnchorSet::resolveRange(ByteRange range) const noexcept {
    FROST_PRE(range.begin <= range.end && range.end <= m_textSize);
    std::vector<ResolvedAnchor> out;
    const std::uint64_t begin = range.begin.value();
    const std::uint64_t end = range.end.value();
    for (const Bias bias : {Bias::Left, Bias::Right}) {
        const AnchorTree& t = tree(bias);
        for (AnchorTree::Cursor c = t.seek([begin](const AnchorSummary& s) { return s.bytes >= begin; });
             c.item() != nullptr; c.next()) {
            const std::uint64_t offset = c.prefix().bytes + c.item()->gap;
            if (offset > end) {
                break;
            }
            out.push_back(ResolvedAnchor{AnchorId{c.item()->id}, ByteOffset{offset}, bias});
        }
    }
    std::sort(out.begin(), out.end(), byOffsetBiasId);
    return out;
}

std::vector<ResolvedAnchor> AnchorSet::all() const noexcept {
    return resolveRange(ByteRange{ByteOffset{0}, m_textSize});
}

void AnchorSet::updateIndexLabels(std::vector<std::pair<std::uint64_t, std::uint64_t>>& relabelled) noexcept {
    const std::uint64_t n = m_index.summary().count;
    if (relabelled.size() * ceilLog2(n + 2) <= n) {
        for (const auto& [id, label] : relabelled) {
            m_index = setIndexLabel(m_index, id, label);
        }
        return;
    }
    // Many entries: rebuild the index in one pass over the (id, label) pairs sorted by id.
    std::sort(relabelled.begin(), relabelled.end());
    std::vector<AnchorIndexEntry> items;
    items.reserve(static_cast<std::size_t>(n));
    std::size_t next = 0;
    for (AnchorIndex::Cursor c = m_index.first(); c.item() != nullptr; c.next()) {
        AnchorIndexEntry entry = *c.item();
        if (next < relabelled.size() && relabelled[next].first == entry.id) {
            entry.label = relabelled[next++].second;
        }
        items.push_back(entry);
    }
    FROST_ASSERT(next == relabelled.size());
    m_index = AnchorIndex::fromItems(items);
}

void AnchorSet::relabel(Bias bias, std::uint64_t lo) noexcept {
    AnchorTree& t = tree(bias);
    const std::uint64_t total = t.summary().count;
    for (unsigned i = 1; i <= 64; ++i) {
        std::uint64_t base = 0;
        std::uint64_t below = 0;   // Entries with labels before the block.
        std::uint64_t count = 0;   // Entries in the block.
        std::uint64_t spacing = 0; // Between consecutive slots.
        bool toTop = true;         // The block reaches the top of the label space.
        std::uint64_t blockEnd = 0;
        if (i < 64) {
            const std::uint64_t span = std::uint64_t{1} << i;
            base = lo & ~(span - 1);
            toTop = base > kNoLabelAbove - span; // base + span would be 2^64.
            blockEnd = toTop ? 0 : base + span;
            below = countBelowLabel(t, base);
            count = (toTop ? total : countBelowLabel(t, blockEnd)) - below;
            if (count + 1 > kThresholds[i]) {
                continue;
            }
            spacing = span / (count + 2);
        } else {
            count = total;
            FROST_ASSERT(count + 1 <= kThresholds[64]);
            spacing = kNoLabelAbove / (count + 2);
        }
        FROST_ASSERT(spacing >= 2);
        // The free slot goes right after `lo`: after the block entries with label <= lo.
        const std::uint64_t slot = lo == 0 ? 0 : countBelowLabel(t, lo + 1) - below;
        auto [before, rest] = splitBeforeLabel(t, base);
        AnchorTree block = rest;
        AnchorTree after;
        if (!toTop) {
            std::tie(block, after) = splitBeforeLabel(rest, blockEnd);
        }
        std::vector<AnchorEntry> items;
        items.reserve(static_cast<std::size_t>(count));
        std::vector<std::pair<std::uint64_t, std::uint64_t>> relabelled;
        relabelled.reserve(static_cast<std::size_t>(count));
        for (AnchorTree::Cursor c = block.first(); c.item() != nullptr; c.next()) {
            const std::uint64_t index = items.size();
            const std::uint64_t slotOfEntry = index < slot ? index : index + 1;
            AnchorEntry entry = *c.item();
            entry.label = base + (slotOfEntry + 1) * spacing;
            items.push_back(entry);
            relabelled.emplace_back(entry.id, entry.label);
        }
        FROST_ASSERT(items.size() == count);
        t = AnchorTree::concat(AnchorTree::concat(before, AnchorTree::fromItems(items)), after);
        ++m_stats.relabels;
        m_stats.relabelledEntries += count;
        updateIndexLabels(relabelled);
        return;
    }
    FROST_ASSERT(false); // Unreachable: the whole space (i = 64) always has room.
}

std::uint64_t AnchorSet::insertEntry(Bias bias, std::uint64_t offset, std::uint64_t id) noexcept {
    for (;;) {
        AnchorTree& t = tree(bias);
        auto [left, right] = splitAfterOffset(t, offset);
        const std::uint64_t lo = left.empty() ? 0 : left.back()->label;
        const std::uint64_t hi = right.empty() ? kNoLabelAbove : right.front()->label;
        if (hi - lo >= 2) {
            const std::uint64_t label = lo + std::min((hi - lo) / 2, kLabelStepCap);
            const std::uint64_t gap = offset - left.summary().bytes;
            if (!right.empty()) {
                right = withFrontGap(right, right.front()->gap - gap);
            }
            t = AnchorTree::concat(AnchorTree::concat(left, singleEntry(AnchorEntry{gap, label, id})), right);
            return label;
        }
        relabel(bias, lo); // Frees a label right after lo; the retry finds it.
    }
}

std::pair<AnchorSet, AnchorId> AnchorSet::withAnchor(ByteOffset offset, Bias bias) const noexcept {
    FROST_PRE(offset <= m_textSize && size() < kMaxAnchors);
    AnchorSet out = *this;
    const std::uint64_t id = out.m_nextId++;
    const std::uint64_t label = out.insertEntry(bias, offset.value(), id);
    // Ids increase, so the new index entry goes last.
    out.m_index = AnchorIndex::concat(out.m_index, singleIndexEntry(AnchorIndexEntry{id, label, bias}));
    return {std::move(out), AnchorId{id}};
}

Result<AnchorSet> AnchorSet::without(AnchorId id) const noexcept {
    const AnchorIndexEntry* entry = findIndexEntry(m_index, id.value);
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound, std::format("no anchor with id {}", id.value));
    }
    AnchorSet out = *this;
    out.tree(entry->bias) = removeByLabel(tree(entry->bias), entry->label);
    auto [left, right] = m_index.split([v = id.value](const AnchorIndexSummary& s) { return s.maxId >= v; });
    out.m_index = AnchorIndex::concat(left, right.withoutFront());
    return out;
}

AnchorSet AnchorSet::withMoved(std::span<const ResolvedAnchor> moves) const noexcept {
    if (moves.empty()) {
        return *this;
    }
    const std::uint64_t n = size();
    if (moves.size() * ceilLog2(n + 2) > n) {
        return movedBulk(moves);
    }
    AnchorSet out = *this;
    for (const ResolvedAnchor& move : moves) {
        FROST_PRE(move.offset <= m_textSize);
        const AnchorIndexEntry* entry = findIndexEntry(out.m_index, move.id.value);
        FROST_PRE(entry != nullptr);
        const Bias bias = entry->bias;
        const std::uint64_t label = entry->label;
        if (findByLabel(out.tree(bias), label).second == move.offset.value()) {
            continue; // Already there.
        }
        out.tree(bias) = removeByLabel(out.tree(bias), label);
        const std::uint64_t newLabel = out.insertEntry(bias, move.offset.value(), move.id.value);
        out.m_index = setIndexLabel(out.m_index, move.id.value, newLabel);
    }
    return out;
}

AnchorSet AnchorSet::movedBulk(std::span<const ResolvedAnchor> moves) const noexcept {
    // The moves by id, merged with the index (also by id) to find each one's class and label.
    std::vector<ResolvedAnchor> byId(moves.begin(), moves.end());
    std::sort(byId.begin(), byId.end(), [](const ResolvedAnchor& x, const ResolvedAnchor& y) { return x.id < y.id; });
    struct Moved {
        std::uint64_t label;
        std::uint64_t offset;
        std::uint64_t id;
    };
    std::array<std::vector<Moved>, 2> moved;
    std::size_t next = 0;
    for (AnchorIndex::Cursor c = m_index.first(); c.item() != nullptr && next < byId.size(); c.next()) {
        if (c.item()->id == byId[next].id.value) {
            FROST_PRE(byId[next].offset <= m_textSize);
            FROST_PRE(next + 1 == byId.size() || byId[next + 1].id != byId[next].id);
            moved[static_cast<std::size_t>(c.item()->bias)].push_back(
                Moved{c.item()->label, byId[next].offset.value(), c.item()->id});
            ++next;
        }
    }
    FROST_PRE(next == byId.size()); // Every moved id exists.

    AnchorSet out = *this;
    std::vector<AnchorIndexEntry> indexItems;
    indexItems.reserve(static_cast<std::size_t>(size()));
    for (const Bias bias : {Bias::Left, Bias::Right}) {
        std::vector<Moved>& movedHere = moved[static_cast<std::size_t>(bias)];
        // The kept entries in tree (label) order, skipping the moved labels.
        std::sort(movedHere.begin(), movedHere.end(), [](const Moved& x, const Moved& y) { return x.label < y.label; });
        struct Placed {
            std::uint64_t offset;
            std::uint64_t id;
        };
        std::vector<Placed> kept;
        kept.reserve(tree(bias).summary().count);
        std::uint64_t offset = 0;
        std::size_t skip = 0;
        for (AnchorTree::Cursor c = tree(bias).first(); c.item() != nullptr; c.next()) {
            offset += c.item()->gap;
            if (skip < movedHere.size() && movedHere[skip].label == c.item()->label) {
                ++skip;
                continue;
            }
            kept.push_back(Placed{offset, c.item()->id});
        }
        // Merge with the moved entries by new offset; a moved entry goes after the kept ones at its
        // offset, as withAnchor would place it.
        std::stable_sort(movedHere.begin(), movedHere.end(),
                         [](const Moved& x, const Moved& y) { return x.offset < y.offset; });
        std::vector<Placed> merged;
        merged.reserve(kept.size() + movedHere.size());
        std::size_t k = 0;
        for (const Moved& m : movedHere) {
            while (k < kept.size() && kept[k].offset <= m.offset) {
                merged.push_back(kept[k++]);
            }
            merged.push_back(Placed{m.offset, m.id});
        }
        merged.insert(merged.end(), kept.begin() + static_cast<std::ptrdiff_t>(k), kept.end());
        // Fresh, evenly spread labels for the whole class.
        const std::uint64_t spacing = kNoLabelAbove / (merged.size() + 1);
        std::vector<AnchorEntry> items;
        items.reserve(merged.size());
        std::uint64_t end = 0;
        for (const Placed& p : merged) {
            const std::uint64_t label = (items.size() + 1) * spacing;
            items.push_back(AnchorEntry{p.offset - end, label, p.id});
            indexItems.push_back(AnchorIndexEntry{p.id, label, bias});
            end = p.offset;
        }
        out.tree(bias) = AnchorTree::fromItems(items);
        if (!items.empty()) {
            ++out.m_stats.relabels;
            out.m_stats.relabelledEntries += items.size();
        }
    }
    std::sort(indexItems.begin(), indexItems.end(),
              [](const AnchorIndexEntry& x, const AnchorIndexEntry& y) { return x.id < y.id; });
    out.m_index = AnchorIndex::fromItems(indexItems);
    return out;
}

AnchorSet AnchorSet::transformImpl(std::span<const RawEdit> batch, std::uint64_t newVersion, int path) const noexcept {
    AnchorSet out = *this;
    out.m_version = newVersion;
    std::int64_t delta = 0;
    std::uint64_t steps = 0;
    std::uint64_t previousEnd = 0;
    for (const RawEdit& edit : batch) {
        FROST_PRE(edit.range.begin <= edit.range.end && edit.range.end <= m_textSize &&
                  previousEnd <= edit.range.begin.value());
        previousEnd = edit.range.end.value();
        if (!edit.isNoOp()) {
            ++steps;
            delta += static_cast<std::int64_t>(edit.newBytes) - static_cast<std::int64_t>(edit.range.size());
        }
    }
    out.m_textSize = ByteOffset{shifted(m_textSize.value(), delta)};
    if (steps == 0 || empty()) {
        return out;
    }
    const std::uint64_t n = size();
    const bool sweep = path == 2 || (path == 0 && steps * ceilLog2(n + 2) > n);
    if (sweep) {
        detail::BatchMapper leftMapper(batch);
        detail::BatchMapper rightMapper = leftMapper;
        out.m_left = sweepTree(m_left, Bias::Left, leftMapper);
        out.m_right = sweepTree(m_right, Bias::Right, rightMapper);
        return out;
    }
    // Last to first: the steps still to apply lie at or before this one's range, whose
    // coordinates the later steps left unchanged.
    for (std::size_t i = batch.size(); i-- > 0;) {
        const RawEdit& edit = batch[i];
        if (edit.isNoOp()) {
            continue;
        }
        const std::uint64_t a = edit.range.begin.value();
        const std::uint64_t b = edit.range.end.value();
        stepLeft(out.m_left, a, b, edit.newBytes);
        stepRight(out.m_right, a, b, edit.newBytes);
    }
    return out;
}

AnchorSet AnchorSet::transformed(std::span<const RawEdit> batch, std::uint64_t newVersion) const noexcept {
    return transformImpl(batch, newVersion, 0);
}

AnchorSet AnchorSet::transformedForTesting(std::span<const RawEdit> batch, std::uint64_t newVersion,
                                           bool forceSweep) const noexcept {
    return transformImpl(batch, newVersion, forceSweep ? 2 : 1);
}

Result<void> AnchorSet::checkInvariants() const noexcept {
    for (const auto* t : {&m_left, &m_right}) {
        if (auto checked = t->checkInvariants(); !checked) {
            return fail(ErrorCode::Internal, "A1: class tree: " + checked.error().message());
        }
    }
    if (auto checked = m_index.checkInvariants(); !checked) {
        return fail(ErrorCode::Internal, "A1: index: " + checked.error().message());
    }
    for (const Bias bias : {Bias::Left, Bias::Right}) {
        std::uint64_t previous = 0;
        std::uint64_t offset = 0;
        for (AnchorTree::Cursor c = tree(bias).first(); c.item() != nullptr; c.next()) {
            const AnchorEntry& entry = *c.item();
            if (entry.label <= previous || entry.label > kMaxLabel) {
                return fail(ErrorCode::Internal,
                            std::format("A2: label {} of anchor {} after label {}", entry.label, entry.id, previous));
            }
            previous = entry.label;
            offset += entry.gap;
            if (offset > m_textSize.value()) {
                return fail(ErrorCode::Internal, std::format("A6: anchor {} at {} is beyond the text's {} bytes",
                                                             entry.id, offset, m_textSize));
            }
        }
    }
    std::uint64_t previousId = 0;
    for (AnchorIndex::Cursor c = m_index.first(); c.item() != nullptr; c.next()) {
        const AnchorIndexEntry& entry = *c.item();
        if (entry.id <= previousId || entry.id >= m_nextId) {
            return fail(ErrorCode::Internal,
                        std::format("A3: id {} after {} (next id {})", entry.id, previousId, m_nextId));
        }
        previousId = entry.id;
        const AnchorTree::Cursor found =
            tree(entry.bias).seek([label = entry.label](const AnchorSummary& s) { return s.maxLabel >= label; });
        if (found.item() == nullptr || found.item()->label != entry.label || found.item()->id != entry.id) {
            return fail(ErrorCode::Internal,
                        std::format("A5: index entry of anchor {} does not find its class entry", entry.id));
        }
    }
    if (m_index.summary().count != m_left.summary().count + m_right.summary().count) {
        return fail(ErrorCode::Internal,
                    std::format("A4: the index holds {} anchors, the class trees {} + {}", m_index.summary().count,
                                m_left.summary().count, m_right.summary().count));
    }
    return {};
}

bool AnchorSet::sameStructureForTesting(const AnchorSet& other) const noexcept {
    if (m_version != other.m_version || m_textSize != other.m_textSize || m_nextId != other.m_nextId) {
        return false;
    }
    const auto sameTree = [](const AnchorTree& x, const AnchorTree& y) {
        AnchorTree::Cursor a = x.first();
        AnchorTree::Cursor b = y.first();
        for (; a.item() != nullptr && b.item() != nullptr; a.next(), b.next()) {
            if (a.item()->gap != b.item()->gap || a.item()->label != b.item()->label || a.item()->id != b.item()->id) {
                return false;
            }
        }
        return a.item() == nullptr && b.item() == nullptr;
    };
    const auto sameIndex = [](const AnchorIndex& x, const AnchorIndex& y) {
        AnchorIndex::Cursor a = x.first();
        AnchorIndex::Cursor b = y.first();
        for (; a.item() != nullptr && b.item() != nullptr; a.next(), b.next()) {
            if (a.item()->id != b.item()->id || a.item()->label != b.item()->label ||
                a.item()->bias != b.item()->bias) {
                return false;
            }
        }
        return a.item() == nullptr && b.item() == nullptr;
    };
    return sameTree(m_left, other.m_left) && sameTree(m_right, other.m_right) && sameIndex(m_index, other.m_index);
}

} // namespace frost::text
