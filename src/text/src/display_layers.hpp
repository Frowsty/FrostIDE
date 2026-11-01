// SPDX-License-Identifier: Apache-2.0
//
// The four layers behind DisplayMap (private header; frost/text/display_map.hpp, ADR 0026): the fold
// layer (an identity until M15), the soft-wrap layer, the inlay layer (an identity until M15) and the
// block-decoration layer, with the line and row patches that carry an edit up the stack.
/*
 * Each layer maps its input to its output coordinates in O(B log n) and turns a patch of its input
 * into a patch of its output:
 *   buffer lines --FoldLayer--> fold lines --WrapLayer--> wrap rows --InlayLayer--> inlay rows
 *   (= wrap rows) --BlockLayer--> display rows.
 * Fold coordinates are byte offsets into the folded text and fold line numbers; with the identity
 * fold layer they equal buffer offsets and lines, and the conversions are spelled out anyway so
 * that M15 only fills the fold layer. Design from PLAN.md section 2 ("Editor rendering"); no GPL or
 * proprietary editor source was consulted (CLAUDE.md, clean room).
 */
#pragma once

#include "frost/core/assert.hpp"
#include "frost/core/error.hpp"
#include "frost/text/anchor.hpp"
#include "frost/text/anchor_set.hpp"
#include "frost/text/display_map.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/sum_tree.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/text/text_shaper.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::text::detail {

// Lines or rows [oldFirst, oldEnd) before an update became [newFirst, newEnd) after it. A patch is
// a vector of these, sorted and disjoint on both sides; neither side is ever empty.
struct SpanEdit {
    std::uint32_t oldFirst = 0;
    std::uint32_t oldEnd = 0;
    std::uint32_t newFirst = 0;
    std::uint32_t newEnd = 0;

    friend constexpr bool operator==(const SpanEdit&, const SpanEdit&) noexcept = default;
};

/*
 * The buffer-line patch of a ChangeSet's TextChanges (application order, each in its own step's
 * coordinates; D8 normalised, so the line structure outside each change is unchanged). Change i
 * replaces lines [start.line, oldEnd.line] by [start.line, newEnd.line] of its step; the steps are
 * composed into one patch from the text before the set to the text after it, merging overlaps.
 * O(m^2) for m changes (m is the caret count).
 */
[[nodiscard]] std::vector<SpanEdit> composeLinePatch(std::span<const TextChange> changes) noexcept;

// --- Fold layer ---------------------------------------------------------------------------------

// A fold line: its first byte, its content end and the end of its line break (fold offsets).
struct FoldLine {
    std::uint64_t start = 0;
    std::uint64_t contentEnd = 0;
    std::uint64_t end = 0;
};

/*
 * The folded text over a buffer snapshot. An identity layer until M15: fold lines are buffer lines
 * and fold offsets are buffer offsets. M15 adds the folds (anchored ranges replaced by a
 * placeholder) behind the same queries; the wrap layer reads only these. An immutable value.
 */
class FoldLayer {
public:
    FoldLayer() noexcept = default;
    explicit FoldLayer(Snapshot text) noexcept : m_text(std::move(text)) {}

    [[nodiscard]] const Snapshot& buffer() const noexcept { return m_text; }
    // The folded text's size in bytes and its line count (at least 1). O(1).
    [[nodiscard]] std::uint64_t size() const noexcept { return m_text.size().value(); }
    [[nodiscard]] std::uint32_t lineCount() const noexcept { return m_text.lineCount(); }
    // Fold line `line`. Precondition: line < lineCount(). O(B log n + P).
    [[nodiscard]] FoldLine line(std::uint32_t line) const noexcept;
    // The fold line holding fold offset `offset` (<= size()); CRLF-interior reports the CR's line.
    [[nodiscard]] std::uint32_t lineAt(std::uint64_t offset) const noexcept;
    // `offset` (<= size()) floored to a character boundary.
    [[nodiscard]] std::uint64_t clip(std::uint64_t offset) const noexcept;
    // Buffer -> fold and back. `bias` picks a side of a fold (M15); identities until then.
    [[nodiscard]] std::uint64_t toFold(ByteOffset offset, Bias bias) const noexcept;
    [[nodiscard]] ByteOffset toBuffer(std::uint64_t offset, Bias bias) const noexcept;
    // The buffer line where fold line `line` starts.
    [[nodiscard]] LineIndex bufferLine(std::uint32_t line) const noexcept { return LineIndex{line}; }

    // Calls fn(std::string_view chunk, SegmentKind kind) over the folded text [begin, end) (char
    // boundaries) in order: Text chunks, and FoldPlaceholder chunks from M15 on.
    template<class F>
    void forEachChunk(std::uint64_t begin, std::uint64_t end, F&& fn) const noexcept {
        for (const std::string_view chunk : m_text.chunks(ByteRange{ByteOffset{begin}, ByteOffset{end}})) {
            fn(chunk, SegmentKind::Text);
        }
    }

    // Calls fn(std::uint32_t line, const FoldLine& info) for the lines [first, end) in order.
    // Precondition: first < end <= lineCount(). O(B log n + bytes of the lines).
    template<class F>
    void forEachLine(std::uint32_t first, std::uint32_t end, F&& fn) const noexcept {
        Result<LineCursor> cursor = m_text.linesFrom(LineIndex{first});
        FROST_ASSERT(cursor.has_value());
        LineInfo info;
        for (std::uint32_t line = first; line < end && cursor->next(info); ++line) {
            fn(line, toFoldLine(info));
        }
    }

    // The fold layer over `newText` (the result of `changes`), with the fold-line patch appended
    // to `patch`. Identity: the buffer-line patch.
    [[nodiscard]] FoldLayer updated(std::span<const TextChange> changes, Snapshot newText,
                                    std::vector<SpanEdit>& patch) const noexcept;

private:
    [[nodiscard]] static FoldLine toFoldLine(const LineInfo& info) noexcept;

    Snapshot m_text;
};

// --- Soft-wrap layer ----------------------------------------------------------------------------

/*
 * A run of wrap rows: row 0 starts at the item's first byte (possibly inside a line, after a soft
 * wrap); rows 1.. are whole lines from their line starts; rows 0..rows-2 end with a line break, and
 * the last row does too exactly when lineBreaks == rows (otherwise it ends at a soft wrap or at the
 * end of the text). So lineBreaks is rows or rows - 1, and the items of a tree end exactly at the
 * soft wraps: unwrapped text costs one item however long it is.
 */
struct WrapItem {
    std::uint64_t bytes = 0; // Fold bytes, line breaks included.
    std::uint32_t rows = 0;
    std::uint32_t lineBreaks = 0;
};

struct WrapSummary {
    std::uint64_t bytes = 0;
    std::uint32_t rows = 0;
    std::uint32_t lineBreaks = 0;

    friend constexpr bool operator==(const WrapSummary&, const WrapSummary&) noexcept = default;
};

struct WrapTraits {
    using Item = WrapItem;
    using Summary = WrapSummary;
    [[nodiscard]] static constexpr Summary summary(const Item& item) noexcept {
        return {item.bytes, item.rows, item.lineBreaks};
    }
    [[nodiscard]] static constexpr Summary combine(const Summary& a, const Summary& b) noexcept {
        return {a.bytes + b.bytes, a.rows + b.rows, a.lineBreaks + b.lineBreaks};
    }
};

using WrapTree = SumTree<WrapTraits, 8>;

// One wrap row in fold coordinates: its fold line, first byte and content end (no line break).
struct WrapRow {
    std::uint32_t line = 0;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    bool startsLine = true;
    bool endsLine = true;
};

/*
 * Wrap rows over the fold layer. An immutable value; the fold layer it was built over is passed to
 * the queries that need line positions (it is not stored, so updates can read old and new folds).
 */
class WrapLayer {
public:
    WrapLayer() noexcept = default;
    // Wraps every line of `fold` at `width` shaper units (0: off). O(text) when wrapping, else O(1).
    WrapLayer(const FoldLayer& fold, const ITextShaper& shaper, TextWidth width) noexcept;

    [[nodiscard]] TextWidth width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t rowCount() const noexcept { return m_tree.summary().rows; }
    [[nodiscard]] const WrapTree& tree() const noexcept { return m_tree; }

    // Row `row` (< rowCount()). O(B log n + P).
    [[nodiscard]] WrapRow row(std::uint32_t row, const FoldLayer& fold) const noexcept;
    // The row holding fold offset `offset` (<= size); at a soft wrap Left picks the upper row.
    [[nodiscard]] std::uint32_t rowAt(std::uint64_t offset, Bias bias, const FoldLayer& fold) const noexcept;
    // The first row of fold line `line`; rowCount() for line == lineCount(). O(B log n).
    [[nodiscard]] std::uint32_t firstRowOfLine(std::uint32_t line) const noexcept;

    // The layer after the fold-line patch `lines` (old lines index `oldFold`, new lines `newFold`),
    // with the row patch appended to `rows` (one entry per line edit, in order).
    [[nodiscard]] WrapLayer updated(const FoldLayer& oldFold, const FoldLayer& newFold, std::span<const SpanEdit> lines,
                                    const ITextShaper& shaper, std::vector<SpanEdit>& rows) const noexcept;

    // D1 and D2 of DisplayMap::checkInvariants.
    [[nodiscard]] Result<void> checkInvariants(const FoldLayer& fold) const noexcept;

private:
    WrapLayer(WrapTree tree, TextWidth width) noexcept : m_tree(std::move(tree)), m_width(width) {}

    WrapTree m_tree;
    TextWidth m_width = 0;
};

// The fold offsets where the continuation rows of fold line `line` start (empty when it fits),
// appended to `breaks`. The rules are display_map.hpp's. O(line bytes). Exposed for tests.
void wrapLine(const FoldLayer& fold, const FoldLine& line, const ITextShaper& shaper, TextWidth width,
              std::vector<std::uint64_t>& breaks) noexcept;

// --- Inlay layer --------------------------------------------------------------------------------

/*
 * Inline text inside wrap rows. An identity layer until M15: inlay rows are wrap rows (inlays never
 * add rows) and inlay columns are wrap columns. M15 adds the inlays (anchored strings) behind the
 * same queries; the block layer and DisplayMap read only these. An immutable value.
 */
class InlayLayer {
public:
    // The inlay column of wrap column `column` in wrap row `row`; at an inlay, Left is before it.
    [[nodiscard]] std::uint32_t toInlayColumn(std::uint32_t /*row*/, std::uint32_t column,
                                              Bias /*bias*/) const noexcept {
        return column;
    }
    // The wrap column of inlay column `column` in row `row`; a column inside an inlay snaps to its
    // side by `bias`.
    [[nodiscard]] std::uint32_t toWrapColumn(std::uint32_t /*row*/, std::uint32_t column,
                                             Bias /*bias*/) const noexcept {
        return column;
    }
    // Appends the display segments of wrap row `wrapRow` (`row`, with its buffer range `buffer`):
    // the row's buffer text, split around its inlays (none until M15).
    void appendSegments(std::uint32_t wrapRow, ByteRange buffer, std::vector<RowSegment>& out) const noexcept;
    // The row patch of the layer above: inlays never add rows, so the wrap patch passes through.
    [[nodiscard]] InlayLayer updated(std::span<const SpanEdit> rows, std::vector<SpanEdit>& out) const noexcept;
};

// --- Block-decoration layer ---------------------------------------------------------------------

// Run: input rows passed through. Below and Above: a block's rows (Below sorts before Above).
enum class BlockKind : std::uint8_t { Run, Below, Above };

struct BlockItem {
    std::uint32_t inputRows = 0; // Runs only, >= 1.
    std::uint32_t blockRows = 0; // Blocks only, >= 1.
    std::uint64_t id = 0;        // Blocks only: the BlockId (also its AnchorId).
    BlockKind kind = BlockKind::Run;
};

struct BlockSummary {
    std::uint32_t inputRows = 0;
    std::uint32_t outputRows = 0;
    std::uint32_t blocks = 0;
    BlockKind lastKind = BlockKind::Run; // The kind of the last item; Run for the empty summary.

    friend constexpr bool operator==(const BlockSummary&, const BlockSummary&) noexcept = default;
};

struct BlockTraits {
    using Item = BlockItem;
    using Summary = BlockSummary;
    [[nodiscard]] static constexpr Summary summary(const Item& item) noexcept {
        return {item.inputRows, item.inputRows + item.blockRows, item.kind == BlockKind::Run ? 0u : 1u, item.kind};
    }
    // Rows add; lastKind is the right side's unless it is empty (every item has an output row).
    [[nodiscard]] static constexpr Summary combine(const Summary& a, const Summary& b) noexcept {
        return {a.inputRows + b.inputRows, a.outputRows + b.outputRows, a.blocks + b.blocks,
                b.outputRows > 0 ? b.lastKind : a.lastKind};
    }
};

using BlockTree = SumTree<BlockTraits, 8>;

// An output row of the block layer: an input row, or row `rowInBlock` of block `id`, which sits at
// input-row boundary `inputRow` (the input rows before it).
struct BlockRowRef {
    bool isBlock = false;
    std::uint32_t inputRow = 0;
    std::uint64_t id = 0;
    std::uint32_t rowInBlock = 0;
};

// One replaced region, in fold lines and in input rows.
struct RegionEdit {
    SpanEdit lines;
    SpanEdit rows;
};

/*
 * Block rows between input rows. The tree is runs of input rows and block items; blocks at one
 * boundary keep the order Below (by id) then Above (by id). Each block has an anchor in a private
 * AnchorSet (ids are BlockIds) transformed with every ChangeSet's raw edits, and a boundary derived
 * from it: an Above block of line L sits before the first row of L, a Below block after its last.
 */
class BlockLayer {
public:
    BlockLayer() noexcept = default;
    // No blocks over `inputRows` rows of `text`.
    BlockLayer(const Snapshot& text, std::uint32_t inputRows) noexcept;

    [[nodiscard]] std::uint32_t rowCount() const noexcept { return m_tree.summary().outputRows; }
    [[nodiscard]] std::uint32_t inputRowCount() const noexcept { return m_tree.summary().inputRows; }
    [[nodiscard]] std::size_t blockCount() const noexcept { return m_blocks.size(); }

    // The output row of input row `row` (<= inputRowCount(); the end maps to rowCount()).
    [[nodiscard]] std::uint32_t toOutputRow(std::uint32_t row) const noexcept;
    // What output row `row` (< rowCount()) shows.
    [[nodiscard]] BlockRowRef fromOutputRow(std::uint32_t row) const noexcept;
    // The registration of block `id` (its `at` is the offset at creation). Precondition: it exists.
    [[nodiscard]] const BlockDecoration& decoration(std::uint64_t id) const noexcept;
    // The current anchor offset of block `id`. Precondition: it exists.
    [[nodiscard]] ByteOffset offsetOf(std::uint64_t id) const noexcept;
    // The fold line block `id` is attached to. Precondition: it exists.
    [[nodiscard]] std::uint32_t lineOf(std::uint64_t id, const FoldLayer& fold) const noexcept;

    // Adds, removes and describes blocks (DisplayMap's errors and costs).
    [[nodiscard]] Result<BlockId> add(const BlockDecoration& block, const FoldLayer& fold,
                                      const WrapLayer& wrap) noexcept;
    [[nodiscard]] Result<void> remove(BlockId id, const FoldLayer& fold, const WrapLayer& wrap) noexcept;
    [[nodiscard]] Result<BlockInfo> info(BlockId id, const FoldLayer& fold, const WrapLayer& wrap) const noexcept;

    /*
     * Maps the anchors through `edits` to `newVersion` and splices the tree for `regions` (the wrap
     * layer's line and row edits, ascending): the blocks attached to a region's lines are placed
     * again on the new lines, all others keep their place. Appends the output-row patch to `patch`.
     */
    void apply(std::span<const RawEdit> edits, std::uint64_t newVersion, std::span<const RegionEdit> regions,
               const FoldLayer& fold, const WrapLayer& wrap, std::vector<RowEdit>& patch) noexcept;
    // Places every block again over a new wrap layer (same text).
    void rebuild(const FoldLayer& fold, const WrapLayer& wrap) noexcept;

    // D3 of DisplayMap::checkInvariants.
    [[nodiscard]] Result<void> checkInvariants(const FoldLayer& fold, const WrapLayer& wrap) const noexcept;

private:
    // The input-row boundary and kind where block `id` belongs.
    [[nodiscard]] std::pair<std::uint32_t, BlockKind> placementOf(std::uint64_t id, const FoldLayer& fold,
                                                                  const WrapLayer& wrap) const noexcept;

    BlockTree m_tree;
    AnchorSet m_anchors;
    std::map<std::uint64_t, BlockDecoration> m_blocks; // By id; `at` is the offset at creation.
};

// --- The stack ----------------------------------------------------------------------------------

// DisplayMap's state: the four layers, the borrowed shaper and the wrap settings.
struct DisplayLayers {
    FoldLayer fold;
    WrapLayer wrap;
    InlayLayer inlay;
    BlockLayer block;
    const ITextShaper* shaper = nullptr;
    WrapSettings settings;
};

} // namespace frost::text::detail
