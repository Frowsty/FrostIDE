// SPDX-License-Identifier: Apache-2.0
//
// The block-decoration layer (display_layers.hpp): runs of input rows and block items in one sum
// tree, cuts at input-row boundaries, block placement from anchors and the incremental splice.

#include "display_layers.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <optional>

namespace frost::text::detail {

namespace {

// Where a cut at an input-row boundary falls among the blocks at that boundary.
enum class Cut : std::uint8_t {
    BeforeBlocks,  // Before every block at the boundary.
    BetweenGroups, // After the Below blocks (the upper line's), before the Above blocks.
    AfterBlocks,   // After every block at the boundary.
};

// A block at a boundary, relative to some first row, for building trees.
struct Placed {
    std::uint32_t at = 0;
    BlockKind kind = BlockKind::Below;
    std::uint64_t id = 0;
    std::uint32_t rows = 0;

    friend constexpr bool operator<(const Placed& a, const Placed& b) noexcept {
        if (a.at != b.at) {
            return a.at < b.at;
        }
        if (a.kind != b.kind) {
            return a.kind < b.kind;
        }
        return a.id < b.id;
    }
};

[[nodiscard]] BlockItem runOf(std::uint32_t rows) noexcept { return BlockItem{rows, 0, 0, BlockKind::Run}; }

[[nodiscard]] BlockTree single(const BlockItem& item) noexcept { return BlockTree::fromItems(std::span(&item, 1)); }

// a followed by b, merging the two runs at the seam.
[[nodiscard]] BlockTree concatRuns(const BlockTree& a, const BlockTree& b) noexcept {
    if (a.empty() || b.empty()) {
        return a.empty() ? b : a;
    }
    const BlockItem last = *a.back();
    const BlockItem first = *b.front();
    if (last.kind != BlockKind::Run || first.kind != BlockKind::Run) {
        return BlockTree::concat(a, b);
    }
    return BlockTree::concat(BlockTree::concat(a.withoutBack(), single(runOf(last.inputRows + first.inputRows))),
                             b.withoutFront());
}

// `tree` (absolute prefix sums) split at input-row boundary `row`, the blocks there divided by
// `cut`; a run spanning the boundary is cut in two. O(B log n).
[[nodiscard]] std::pair<BlockTree, BlockTree> splitAt(const BlockTree& tree, std::uint32_t row, Cut cut) noexcept {
    // Monotone because the blocks at a boundary are ordered Below, then Above.
    auto [left, right] = tree.split([row, cut](const BlockSummary& s) {
        if (s.inputRows != row) {
            return s.inputRows > row;
        }
        switch (cut) {
        case Cut::BeforeBlocks: return s.lastKind != BlockKind::Run;
        case Cut::BetweenGroups: return s.lastKind == BlockKind::Above;
        case Cut::AfterBlocks: return false;
        }
        return false;
    });
    const std::uint32_t have = left.summary().inputRows;
    if (have < row) {
        FROST_ASSERT(!right.empty());
        const BlockItem spanning = *right.front();
        FROST_ASSERT(spanning.kind == BlockKind::Run && have + spanning.inputRows > row);
        const std::uint32_t k = row - have;
        return {BlockTree::concat(left, single(runOf(k))),
                BlockTree::concat(single(runOf(spanning.inputRows - k)), right.withoutFront())};
    }
    return {std::move(left), std::move(right)};
}

// Runs and blocks over `rows` input rows with `placed` (sorted, at <= rows) at their boundaries.
[[nodiscard]] BlockTree buildTree(std::span<const Placed> placed, std::uint32_t rows) noexcept {
    std::vector<BlockItem> items;
    items.reserve(2 * placed.size() + 1);
    std::uint32_t cursor = 0;
    for (const Placed& block : placed) {
        FROST_ASSERT(block.at <= rows);
        if (block.at > cursor) {
            items.push_back(runOf(block.at - cursor));
            cursor = block.at;
        }
        items.push_back(BlockItem{0, block.rows, block.id, block.kind});
    }
    if (rows > cursor) {
        items.push_back(runOf(rows - cursor));
    }
    return BlockTree::fromItems(items);
}

[[nodiscard]] BlockKind kindOf(BlockPlacement placement) noexcept {
    return placement == BlockPlacement::Above ? BlockKind::Above : BlockKind::Below;
}

[[nodiscard]] Bias biasOf(BlockPlacement placement) noexcept {
    return placement == BlockPlacement::Above ? Bias::Right : Bias::Left;
}

} // namespace

BlockLayer::BlockLayer(const Snapshot& text, std::uint32_t inputRows) noexcept
    : m_tree(single(runOf(inputRows))), m_anchors(text.version(), text.size()) {}

std::uint32_t BlockLayer::toOutputRow(std::uint32_t row) const noexcept {
    const BlockSummary& total = m_tree.summary();
    if (row >= total.inputRows) {
        return total.outputRows;
    }
    const BlockTree::Cursor cursor = m_tree.seek([row](const BlockSummary& s) { return s.inputRows > row; });
    FROST_ASSERT(cursor.item() != nullptr);
    return cursor.prefix().outputRows + (row - cursor.prefix().inputRows);
}

BlockRowRef BlockLayer::fromOutputRow(std::uint32_t row) const noexcept {
    FROST_PRE(row < rowCount());
    const BlockTree::Cursor cursor = m_tree.seek([row](const BlockSummary& s) { return s.outputRows > row; });
    FROST_ASSERT(cursor.item() != nullptr);
    const BlockItem& item = *cursor.item();
    const BlockSummary& prefix = cursor.prefix();
    if (item.kind == BlockKind::Run) {
        return BlockRowRef{false, prefix.inputRows + (row - prefix.outputRows), 0, 0};
    }
    return BlockRowRef{true, prefix.inputRows, item.id, row - prefix.outputRows};
}

const BlockDecoration& BlockLayer::decoration(std::uint64_t id) const noexcept {
    const auto it = m_blocks.find(id);
    FROST_ASSERT(it != m_blocks.end());
    return it->second;
}

ByteOffset BlockLayer::offsetOf(std::uint64_t id) const noexcept {
    const std::optional<ByteOffset> offset = m_anchors.find(AnchorId{id});
    FROST_ASSERT(offset.has_value());
    return *offset;
}

std::uint32_t BlockLayer::lineOf(std::uint64_t id, const FoldLayer& fold) const noexcept {
    return fold.lineAt(fold.toFold(offsetOf(id), biasOf(decoration(id).placement)));
}

std::pair<std::uint32_t, BlockKind> BlockLayer::placementOf(std::uint64_t id, const FoldLayer& fold,
                                                            const WrapLayer& wrap) const noexcept {
    const BlockKind kind = kindOf(decoration(id).placement);
    const std::uint32_t line = lineOf(id, fold);
    // Inlay rows are wrap rows, so the wrap layer's line starts are the block layer's boundaries.
    return {kind == BlockKind::Above ? wrap.firstRowOfLine(line) : wrap.firstRowOfLine(line + 1), kind};
}

Result<BlockId> BlockLayer::add(const BlockDecoration& block, const FoldLayer& fold, const WrapLayer& wrap) noexcept {
    const Snapshot& text = fold.buffer();
    if (block.rows == 0) {
        return fail(ErrorCode::InvalidArgument, "a block decoration needs at least one row");
    }
    if (block.at > text.size()) {
        return fail(ErrorCode::OutOfRange,
                    std::format("block offset {} is past the end of the text ({} bytes)", block.at, text.size()));
    }
    if (!text.isCharBoundary(block.at)) {
        return fail(ErrorCode::InvalidArgument, std::format("byte {} is inside a UTF-8 sequence", block.at));
    }
    if (m_anchors.size() >= AnchorSet::kMaxAnchors ||
        std::uint64_t{rowCount()} + block.rows > std::numeric_limits<std::uint32_t>::max()) {
        return fail(ErrorCode::OutOfRange, "too many block decorations or display rows");
    }
    auto [anchors, id] = m_anchors.withAnchor(block.at, biasOf(block.placement));
    m_anchors = std::move(anchors);
    m_blocks.emplace(id.value, block);
    const auto [row, kind] = placementOf(id.value, fold, wrap);

    // The new id is the largest, so the block goes after every block of its kind at the boundary.
    auto [before, rest] = splitAt(m_tree, row, Cut::BeforeBlocks);
    auto [atRow, after] = rest.split([](const BlockSummary& s) { return s.inputRows > 0; });
    std::vector<BlockItem> blocks;
    for (BlockTree::Cursor cursor = atRow.first(); cursor.item() != nullptr; cursor.next()) {
        blocks.push_back(*cursor.item());
    }
    const auto position =
        std::find_if(blocks.begin(), blocks.end(), [kind](const BlockItem& item) { return item.kind > kind; });
    blocks.insert(position, BlockItem{0, block.rows, id.value, kind});
    m_tree = concatRuns(BlockTree::concat(before, BlockTree::fromItems(blocks)), after);
    return BlockId{id.value};
}

Result<void> BlockLayer::remove(BlockId id, const FoldLayer& fold, const WrapLayer& wrap) noexcept {
    const auto it = m_blocks.find(id.value);
    if (it == m_blocks.end()) {
        return fail(ErrorCode::NotFound, std::format("no block decoration {}", id.value));
    }
    const std::uint32_t row = placementOf(id.value, fold, wrap).first;
    auto [before, rest] = splitAt(m_tree, row, Cut::BeforeBlocks);
    auto [atRow, after] = rest.split([](const BlockSummary& s) { return s.inputRows > 0; });
    std::vector<BlockItem> blocks;
    for (BlockTree::Cursor cursor = atRow.first(); cursor.item() != nullptr; cursor.next()) {
        if (cursor.item()->id != id.value) {
            blocks.push_back(*cursor.item());
        }
    }
    FROST_ASSERT(blocks.size() + 1 == atRow.summary().blocks);
    m_tree = concatRuns(concatRuns(before, BlockTree::fromItems(blocks)), after);
    m_anchors = *m_anchors.without(AnchorId{id.value});
    m_blocks.erase(it);
    return {};
}

Result<BlockInfo> BlockLayer::info(BlockId id, const FoldLayer& fold, const WrapLayer& wrap) const noexcept {
    const auto it = m_blocks.find(id.value);
    if (it == m_blocks.end()) {
        return fail(ErrorCode::NotFound, std::format("no block decoration {}", id.value));
    }
    const std::uint32_t row = placementOf(id.value, fold, wrap).first;
    const auto [before, rest] = splitAt(m_tree, row, Cut::BeforeBlocks);
    std::uint32_t output = before.summary().outputRows;
    for (BlockTree::Cursor cursor = rest.first(); cursor.item() != nullptr && cursor.item()->id != id.value;
         cursor.next()) {
        FROST_ASSERT(cursor.item()->kind != BlockKind::Run);
        output += cursor.item()->blockRows;
    }
    BlockDecoration decoration = it->second;
    decoration.at = offsetOf(id.value);
    return BlockInfo{id, decoration, fold.bufferLine(lineOf(id.value, fold)), DisplayRow{output}};
}

void BlockLayer::apply(std::span<const RawEdit> edits, std::uint64_t newVersion, std::span<const RegionEdit> regions,
                       const FoldLayer& fold, const WrapLayer& wrap, std::vector<RowEdit>& patch) noexcept {
    m_anchors = m_anchors.transformed(edits, newVersion);
    /*
     * A region's old rows hold exactly the blocks attached to its old lines: the Above blocks at its
     * first boundary, everything inside, and the Below blocks at its last boundary, which is what
     * BetweenGroups cuts at both ends select. Their anchors now lie on the region's new lines (the
     * D8 changes contain every raw edit), so the new middle places the blocks found there.
     */
    BlockTree result;
    std::uint32_t kept = 0; // Old input rows before `kept` are in `result` or replaced.
    for (const RegionEdit& region : regions) {
        const BlockTree before = splitAt(m_tree, region.rows.oldFirst, Cut::BetweenGroups).first;
        const BlockTree through = splitAt(m_tree, region.rows.oldEnd, Cut::BetweenGroups).first;
        result = concatRuns(result, splitAt(before, kept, Cut::BetweenGroups).second);
        const std::uint32_t newFirst = result.summary().outputRows;

        std::vector<Placed> placed;
        if (!m_blocks.empty()) {
            const std::uint64_t begin = fold.toBuffer(fold.line(region.lines.newFirst).start, Bias::Left).value();
            const std::uint64_t last = region.lines.newEnd < fold.lineCount()
                                           ? fold.toBuffer(fold.line(region.lines.newEnd).start, Bias::Left).value() - 1
                                           : fold.buffer().size().value();
            for (const ResolvedAnchor& anchor :
                 m_anchors.resolveRange(ByteRange{ByteOffset{begin}, ByteOffset{last}})) {
                const auto [row, kind] = placementOf(anchor.id.value, fold, wrap);
                FROST_ASSERT(row >= region.rows.newFirst);
                placed.push_back(
                    Placed{row - region.rows.newFirst, kind, anchor.id.value, decoration(anchor.id.value).rows});
            }
            std::sort(placed.begin(), placed.end());
        }
        result = concatRuns(result, buildTree(placed, region.rows.newEnd - region.rows.newFirst));
        patch.push_back(RowEdit{DisplayRow{before.summary().outputRows}, DisplayRow{through.summary().outputRows},
                                DisplayRow{newFirst}, DisplayRow{result.summary().outputRows}});
        kept = region.rows.oldEnd;
    }
    m_tree = concatRuns(result, splitAt(m_tree, kept, Cut::BetweenGroups).second);
}

void BlockLayer::rebuild(const FoldLayer& fold, const WrapLayer& wrap) noexcept {
    std::vector<Placed> placed;
    placed.reserve(m_blocks.size());
    for (const auto& [id, block] : m_blocks) {
        const auto [row, kind] = placementOf(id, fold, wrap);
        placed.push_back(Placed{row, kind, id, block.rows});
    }
    std::sort(placed.begin(), placed.end());
    m_tree = buildTree(placed, wrap.rowCount());
}

Result<void> BlockLayer::checkInvariants(const FoldLayer& fold, const WrapLayer& wrap) const noexcept {
    if (Result<void> tree = m_tree.checkInvariants(); !tree) {
        return tree;
    }
    const BlockSummary& total = m_tree.summary();
    if (total.inputRows != wrap.rowCount()) {
        return fail(ErrorCode::Internal, std::format("D3: the block layer has {} input rows, the wrap layer {} rows",
                                                     total.inputRows, wrap.rowCount()));
    }
    if (total.blocks != m_blocks.size() || m_anchors.size() != m_blocks.size()) {
        return fail(ErrorCode::Internal, std::format("D3: {} blocks in the tree, {} anchors, {} registered",
                                                     total.blocks, m_anchors.size(), m_blocks.size()));
    }
    if (m_anchors.version() != fold.buffer().version() || m_anchors.textSize() != fold.buffer().size()) {
        return fail(ErrorCode::Internal, "D3: the block anchors do not match the text's version and size");
    }
    std::uint32_t input = 0;
    std::optional<Placed> previous;
    for (BlockTree::Cursor cursor = m_tree.first(); cursor.item() != nullptr; cursor.next()) {
        const BlockItem& item = *cursor.item();
        if (item.kind == BlockKind::Run) {
            if (item.inputRows == 0 || item.blockRows != 0) {
                return fail(ErrorCode::Internal, "D3: a run without input rows");
            }
            input += item.inputRows;
            continue;
        }
        const auto it = m_blocks.find(item.id);
        if (it == m_blocks.end() || item.inputRows != 0 || item.blockRows != it->second.rows ||
            item.kind != kindOf(it->second.placement)) {
            return fail(ErrorCode::Internal, std::format("D3: block item {} does not match its registration", item.id));
        }
        const auto [row, kind] = placementOf(item.id, fold, wrap);
        if (row != input) {
            return fail(ErrorCode::Internal,
                        std::format("D3: block {} sits at input row {}, its anchor says {}", item.id, input, row));
        }
        const Placed here{input, kind, item.id, item.blockRows};
        if (previous && !(*previous < here)) {
            return fail(ErrorCode::Internal, std::format("D3: block {} is out of order at its boundary", item.id));
        }
        previous = here;
    }
    return {};
}

} // namespace frost::text::detail
