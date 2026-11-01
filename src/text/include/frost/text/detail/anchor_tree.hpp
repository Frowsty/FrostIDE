// SPDX-License-Identifier: Apache-2.0
//
// The items, summaries and SumTree instances behind AnchorSet (ADR 0017, decision 2 and section
// 4.1 of the slice spec). Not public API: anchor_set.hpp needs the complete types for its members.
/*
 * One class tree per bias holds that class's anchors in offset order: an entry's offset is the sum
 * of the gaps up to and including it, and its order label increases along the tree. Within one
 * bias class the mapping rule is monotone and depends only on (offset, bias), so edits never
 * reorder entries and the order among anchors at one offset never matters. The id index, sorted by
 * id, finds an anchor's class and label; a label seek then finds the entry. Seeks (all monotone):
 *   by offset  prefix.bytes > x (the first entry after x) or >= x (the first at or after x);
 *   by label   prefix.maxLabel >= label;
 *   by id      prefix.maxId >= id.
 */
#pragma once

#include "frost/text/anchor.hpp"
#include "frost/text/sum_tree.hpp"

#include <algorithm>
#include <cstdint>

namespace frost::text::detail {

// An anchor in its class tree: the distance from the previous entry (or from offset 0), its order
// label and its id. 24 bytes.
struct AnchorEntry {
    std::uint64_t gap = 0;
    std::uint64_t label = 0;
    std::uint64_t id = 0;
};

// The summary of a run of entries: total gap (the last entry's offset relative to the run's
// start), the largest label (the last one, since labels increase) and the entry count.
struct AnchorSummary {
    std::uint64_t bytes = 0;
    std::uint64_t maxLabel = 0;
    std::uint32_t count = 0;

    friend constexpr bool operator==(const AnchorSummary&, const AnchorSummary&) noexcept = default;
};

struct AnchorTraits {
    using Item = AnchorEntry;
    using Summary = AnchorSummary;

    [[nodiscard]] static constexpr AnchorSummary summary(const AnchorEntry& entry) noexcept {
        return {entry.gap, entry.label, 1};
    }
    [[nodiscard]] static constexpr AnchorSummary combine(const AnchorSummary& a, const AnchorSummary& b) noexcept {
        return {a.bytes + b.bytes, std::max(a.maxLabel, b.maxLabel), a.count + b.count};
    }
};

// An anchor in the id index: its id, its label in the class tree of `bias`.
struct AnchorIndexEntry {
    std::uint64_t id = 0;
    std::uint64_t label = 0;
    Bias bias = Bias::Left;
};

struct AnchorIndexSummary {
    std::uint64_t maxId = 0;
    std::uint32_t count = 0;

    friend constexpr bool operator==(const AnchorIndexSummary&, const AnchorIndexSummary&) noexcept = default;
};

struct AnchorIndexTraits {
    using Item = AnchorIndexEntry;
    using Summary = AnchorIndexSummary;

    [[nodiscard]] static constexpr AnchorIndexSummary summary(const AnchorIndexEntry& entry) noexcept {
        return {entry.id, 1};
    }
    [[nodiscard]] static constexpr AnchorIndexSummary combine(const AnchorIndexSummary& a,
                                                              const AnchorIndexSummary& b) noexcept {
        return {std::max(a.maxId, b.maxId), a.count + b.count};
    }
};

// Fan-out 8 (8..16 entries per node), as the piece tree (ADR 0013).
inline constexpr int kAnchorTreeFanout = 8;
using AnchorTree = SumTree<AnchorTraits, kAnchorTreeFanout>;
using AnchorIndex = SumTree<AnchorIndexTraits, kAnchorTreeFanout>;

} // namespace frost::text::detail
