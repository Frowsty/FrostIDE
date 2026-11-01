// SPDX-License-Identifier: Apache-2.0
//
// The undo tree's data operations (undo_tree.hpp; ADR 0017): region composition, the region and
// shape checks, node creation, the history cap and rollback's leaf removal.

#include "undo_tree.hpp"

#include "frost/core/assert.hpp"

#include "piece_tree.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace frost::text::detail {
namespace {

constexpr std::uint64_t shifted(std::uint64_t x, std::int64_t delta) noexcept {
    return delta >= 0 ? x + static_cast<std::uint64_t>(delta) : x - static_cast<std::uint64_t>(-delta);
}

std::int64_t lengthDelta(std::uint64_t newBytes, std::uint64_t oldBytes) noexcept {
    return static_cast<std::int64_t>(newBytes) - static_cast<std::int64_t>(oldBytes);
}

// Bytes [a, b) of `tree` as a tree (empty without any split when a == b). O(B log n + P).
PieceTree sliceOf(const PieceTree& tree, std::uint64_t a, std::uint64_t b) noexcept {
    if (a == b) {
        return {};
    }
    const PieceTree right = splitAtByte(tree, a).second;
    return splitAtByte(right, b - a).first;
}

} // namespace

/*
 * Section 4.4 of the slice spec. dE and dR are the size changes of the edits and regions in the
 * runs already passed; minY and maxY bound the current run in Y. For a run:
 *   before   = [minY - dR, maxY - dR - ΔR)   (X: no kept byte of the run is outside it)
 *   after    = [minY + dE, maxY + dE + ΔE)   (Z)
 *   removed  = X[before]
 *   inserted = Z[after)
 * Within a run every Y byte lies in a region or an edit (the closed intervals touch), and between
 * runs at least one Y byte lies in neither, so the output is canonical.
 *
 * The removed bytes equal the spec's Y[minY, r1.after.begin) + r1.removed + ... + rk.removed +
 * Y[rk.after.end, maxY) (Y's kept bytes are X's bytes), but are cut from X in one piece: building
 * them from Y's pieces would add a one-byte piece per coalesced backspace (a review found 1000
 * pieces for 1000 backspaces), which undo would then splice into the live tree.
 */
std::vector<Region> composeRegions(std::span<const Region> regions, std::span<const RawEdit> batch,
                                   const PieceTree& xTree, const PieceTree& zTree) noexcept {
    std::vector<Region> out;
    out.reserve(regions.size() + batch.size());
    std::size_t ri = 0;
    std::size_t ei = 0;
    std::int64_t dE = 0;
    std::int64_t dR = 0;
    // True when the next item (by begin) is a region.
    const auto regionNext = [&] {
        if (ri == regions.size()) {
            return false;
        }
        return ei == batch.size() || regions[ri].after.begin <= batch[ei].range.begin;
    };
    while (ri < regions.size() || ei < batch.size()) {
        const std::size_t r0 = ri;
        const std::size_t e0 = ei;
        std::uint64_t minY = 0;
        std::uint64_t maxY = 0;
        if (regionNext()) {
            minY = regions[ri].after.begin.value();
            maxY = regions[ri].after.end.value();
            ++ri;
        } else {
            FROST_ASSERT(!batch[ei].isNoOp());
            minY = batch[ei].range.begin.value();
            maxY = batch[ei].range.end.value();
            ++ei;
        }
        // Extend the run while the next item touches it (closed intervals).
        for (;;) {
            if (regionNext()) {
                if (regions[ri].after.begin.value() > maxY) {
                    break;
                }
                maxY = std::max(maxY, regions[ri].after.end.value());
                ++ri;
            } else if (ei < batch.size()) {
                if (batch[ei].range.begin.value() > maxY) {
                    break;
                }
                FROST_ASSERT(!batch[ei].isNoOp());
                maxY = std::max(maxY, batch[ei].range.end.value());
                ++ei;
            } else {
                break;
            }
        }
        if (ri - r0 == 1 && ei == e0) {
            Region kept = regions[r0];
            kept.after = ByteRange{ByteOffset{shifted(kept.after.begin.value(), dE)},
                                   ByteOffset{shifted(kept.after.end.value(), dE)}};
            dR += lengthDelta(kept.after.size(), kept.before.size());
            out.push_back(std::move(kept));
            continue;
        }
        std::int64_t deltaE = 0;
        for (std::size_t k = e0; k < ei; ++k) {
            deltaE += lengthDelta(batch[k].newBytes, batch[k].range.size());
        }
        std::int64_t deltaR = 0;
        for (std::size_t k = r0; k < ri; ++k) {
            deltaR += lengthDelta(regions[k].after.size(), regions[k].before.size());
        }
        const ByteRange before{ByteOffset{shifted(minY, -dR)}, ByteOffset{shifted(shifted(maxY, -dR), -deltaR)}};
        const ByteRange after{ByteOffset{shifted(minY, dE)}, ByteOffset{shifted(shifted(maxY, dE), deltaE)}};
        dE += deltaE;
        dR += deltaR;
        if (before.empty() && after.empty()) {
            continue; // The run netted to no change.
        }
        FROST_ASSERT(before.end.value() <= xTree.summary().bytes && after.end.value() <= zTree.summary().bytes);
        out.push_back(Region{before, after, sliceOf(xTree, before.begin.value(), before.end.value()),
                             sliceOf(zTree, after.begin.value(), after.end.value())});
    }
    return out;
}

Result<void> checkRegions(std::span<const Region> regions, std::uint64_t xSize, std::uint64_t ySize) noexcept {
    std::int64_t delta = 0;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const Region& r = regions[i];
        if (r.before.begin > r.before.end || r.after.begin > r.after.end || r.before.end.value() > xSize ||
            r.after.end.value() > ySize) {
            return fail(ErrorCode::Internal,
                        std::format("U4: region {} [{}, {}) -> [{}, {}) is not inside X ({}) and "
                                    "Y ({})",
                                    i, r.before.begin, r.before.end, r.after.begin, r.after.end, xSize, ySize));
        }
        if (i > 0 && (r.before.begin <= regions[i - 1].before.end || r.after.begin <= regions[i - 1].after.end)) {
            return fail(ErrorCode::Internal,
                        std::format("U4: region {} is not separated from region {} by a kept byte", i, i - 1));
        }
        if (r.after.begin.value() != shifted(r.before.begin.value(), delta)) {
            return fail(ErrorCode::Internal, std::format("U4: region {} starts at {} in Y, expected {}", i,
                                                         r.after.begin, shifted(r.before.begin.value(), delta)));
        }
        if (r.before.empty() && r.after.empty()) {
            return fail(ErrorCode::Internal, std::format("U4: region {} changes nothing", i));
        }
        if (r.removed.summary().bytes != r.before.size() || r.inserted.summary().bytes != r.after.size()) {
            return fail(ErrorCode::Internal, std::format("U4: region {} has slices of {} and {} bytes for ranges of "
                                                         "{} and {}",
                                                         i, r.removed.summary().bytes, r.inserted.summary().bytes,
                                                         r.before.size(), r.after.size()));
        }
        delta += lengthDelta(r.after.size(), r.before.size());
    }
    if (shifted(xSize, delta) != ySize) {
        return fail(ErrorCode::Internal, std::format("U4: the regions change {} bytes into {}, not {}", xSize,
                                                     shifted(xSize, delta), ySize));
    }
    return {};
}

UndoTree::UndoTree() noexcept {
    UndoNode rootNode;
    rootNode.id = 0;
    nodes.emplace(0, std::move(rootNode));
}

UndoNode& UndoTree::node(std::uint64_t id) noexcept {
    const auto found = nodes.find(id);
    FROST_ASSERT(found != nodes.end());
    return found->second;
}

const UndoNode& UndoTree::node(std::uint64_t id) const noexcept {
    const auto found = nodes.find(id);
    FROST_ASSERT(found != nodes.end());
    return found->second;
}

UndoNode& UndoTree::createChild(std::uint64_t parent) noexcept {
    UndoNode child;
    child.id = nextId++;
    child.parent = parent;
    child.sequence = nextSequence++;
    const std::uint64_t id = child.id;
    UndoNode& created = nodes.emplace(id, std::move(child)).first->second;
    UndoNode& parentNode = node(parent);
    parentNode.children.push_back(id);
    parentNode.redoChild = id;
    ++nodeCount;
    return created;
}

void UndoTree::eraseSubtree(std::uint64_t id) noexcept {
    std::vector<std::uint64_t> pending{id};
    while (!pending.empty()) {
        const std::uint64_t next = pending.back();
        pending.pop_back();
        NodeMap::node_type doomed = nodes.extract(next);
        FROST_ASSERT(!doomed.empty());
        pending.insert(pending.end(), doomed.mapped().children.begin(), doomed.mapped().children.end());
        dropped.push_back(std::move(doomed)); // Freed later (releaseDropped).
        --nodeCount;
    }
}

void UndoTree::releaseDropped(std::size_t count) noexcept {
    for (; count > 0 && !dropped.empty(); --count) {
        dropped.pop_back();
    }
}

void UndoTree::prune(std::uint32_t maxTransactions) noexcept {
    while (nodeCount - 1 > maxTransactions) {
        UndoNode& rootNode = node(root);
        if (rootNode.children.size() >= 2) {
            // The root's child on the current node's path survives; children are in sequence order.
            std::uint64_t onPath = current;
            FROST_ASSERT(onPath != root);
            while (node(onPath).parent != root) {
                onPath = node(onPath).parent;
            }
            const std::uint64_t victim = rootNode.children[0] != onPath ? rootNode.children[0] : rootNode.children[1];
            std::erase(rootNode.children, victim);
            eraseSubtree(victim);
            if (rootNode.redoChild == victim) {
                rootNode.redoChild = rootNode.children.back();
            }
            continue;
        }
        // Re-root: the only child becomes the root and its transaction data is dropped.
        FROST_ASSERT(rootNode.children.size() == 1);
        const std::uint64_t child = rootNode.children[0];
        FROST_ASSERT(child != current);
        nodes.erase(root);
        --nodeCount;
        UndoNode& newRoot = node(child);
        newRoot.parent = kNoNode;
        newRoot.kind = EditKind::Other;
        newRoot.regions.clear();
        newRoot.selectionsBefore = {};
        newRoot.selectionsAfter = {};
        newRoot.anchorsBefore = {};
        newRoot.anchorsAfter = {};
        newRoot.hasAnchorsAfter = false;
        newRoot.lastEdit = {};
        newRoot.open = false;
        root = child;
    }
}

void UndoTree::removeLeaf(std::uint64_t id, std::uint64_t previousRedoChild) noexcept {
    FROST_ASSERT(id != root && node(id).children.empty());
    const std::uint64_t parent = node(id).parent;
    nodes.erase(id);
    --nodeCount;
    UndoNode& parentNode = node(parent);
    std::erase(parentNode.children, id);
    if (previousRedoChild != kNoNode && contains(previousRedoChild) && node(previousRedoChild).parent == parent) {
        parentNode.redoChild = previousRedoChild;
    } else {
        parentNode.redoChild = parentNode.children.empty() ? kNoNode : parentNode.children.back();
    }
}

std::uint32_t UndoTree::currentDepth() const noexcept {
    std::uint32_t depth = 0;
    for (std::uint64_t at = current; at != root; at = node(at).parent) {
        ++depth;
    }
    return depth;
}

Result<void> UndoTree::checkShape(std::uint32_t maxTransactions) const noexcept {
    if (!contains(root) || node(root).parent != kNoNode) {
        return fail(ErrorCode::Internal, "U3: the root is missing or has a parent");
    }
    std::vector<std::uint64_t> ids;
    std::vector<std::uint64_t> stateIds;
    std::vector<std::uint64_t> pending{root};
    bool currentFound = false;
    while (!pending.empty()) {
        const std::uint64_t at = pending.back();
        pending.pop_back();
        if (!contains(at)) {
            return fail(ErrorCode::Internal, std::format("U3: node {} is listed but missing", at));
        }
        const UndoNode& n = node(at);
        if (n.id != at) {
            return fail(ErrorCode::Internal, std::format("U3: node {} is stored under id {}", n.id, at));
        }
        ids.push_back(at);
        stateIds.push_back(n.stateId);
        currentFound = currentFound || at == current;
        if (n.children.empty() != (n.redoChild == kNoNode) ||
            (n.redoChild != kNoNode &&
             std::find(n.children.begin(), n.children.end(), n.redoChild) == n.children.end())) {
            return fail(ErrorCode::Internal, std::format("U3: node {} has redo child {} for {} children", at,
                                                         n.redoChild, n.children.size()));
        }
        std::uint64_t previousSequence = 0;
        for (std::size_t i = 0; i < n.children.size(); ++i) {
            const std::uint64_t c = n.children[i];
            if (!contains(c) || node(c).parent != at) {
                return fail(ErrorCode::Internal,
                            std::format("U3: child {} of node {} does not name it as parent", c, at));
            }
            if (i > 0 && node(c).sequence <= previousSequence) {
                return fail(ErrorCode::Internal, std::format("U3: the children of node {} are out of sequence", at));
            }
            previousSequence = node(c).sequence;
            pending.push_back(c);
        }
        if (ids.size() > nodes.size()) {
            return fail(ErrorCode::Internal, "U3: the tree has a cycle");
        }
    }
    if (!currentFound) {
        return fail(ErrorCode::Internal, std::format("U3: the current node {} is not reachable", current));
    }
    if (ids.size() != nodes.size() || ids.size() != nodeCount) {
        return fail(ErrorCode::Internal,
                    std::format("U3: {} reachable nodes, {} stored, count {}", ids.size(), nodes.size(), nodeCount));
    }
    if (nodeCount - 1 > maxTransactions) {
        return fail(ErrorCode::Internal,
                    std::format("U3: {} transactions exceed the cap of {}", nodeCount - 1, maxTransactions));
    }
    if (dropped.size() > maxTransactions) {
        return fail(ErrorCode::Internal, std::format("U3: {} dropped nodes wait to be freed, more than the cap of {}",
                                                     dropped.size(), maxTransactions));
    }
    std::sort(ids.begin(), ids.end());
    std::sort(stateIds.begin(), stateIds.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end() ||
        std::adjacent_find(stateIds.begin(), stateIds.end()) != stateIds.end()) {
        return fail(ErrorCode::Internal, "U3: node ids or stateIds repeat");
    }
    return {};
}

} // namespace frost::text::detail
