// SPDX-License-Identifier: Apache-2.0
//
// frost::text::DisplayMap: the layered display map between a document's text and the rows an
// editor shows (PLAN.md section 2, "Editor rendering"; ADR 0026, ADR 0029 decision 9). It maps
// buffer offsets and points to display rows and columns and back in O(log n), updates
// incrementally from Document ChangeSets, and serves row iteration, hit testing and caret movement
// by display row to m1/editor-model and m1/editor-view.
/*
 * Layers, in their final order: folds -> soft wraps -> inlays -> block decorations. Each layer has
 * its own sum tree (or none) and maps its input coordinates to its output coordinates in
 * O(log n); an edit travels up the stack as a patch of replaced line or row ranges.
 *   - Fold layer: buffer text -> folded text. Collapsed ranges are replaced by placeholders;
 *     nested collapsed ranges remain registered while their ancestor is collapsed.
 *   - Soft-wrap layer: folded lines -> wrap rows. Greedy wrapping at a width (by column or in
 *     shaper units), measured with an ITextShaper (text_shaper.hpp).
 *   - Inlay layer: provider text anchored at buffer offsets inside wrap rows; it shifts display
 *     columns without adding rows or changing buffer offsets.
 *   - Block-decoration layer: whole rows inserted above or below buffer lines (the compare and
 *     merge views' alignment fillers), anchored so that they follow edits.
 *
 * Coordinates. A DisplayRow counts every display row, block rows included. A DisplayColumn is a
 * byte offset into the row's display text: the concatenation of its segments (RowSegment), including
 * inline inlay text. x positions (TextWidth) come from the shaper; every row lays out from x = 0.
 *
 * Soft-wrap boundaries. The end of a soft-wrapped row and the start of the row after it are the
 * same buffer offset. Bias picks the row: Bias::Left is the end of the upper row, Bias::Right the
 * start of the lower one (the same rule as anchors: Left sticks to the character before).
 *
 * Wrap rules (ADR 0026): a row breaks before the first character that does not fit, at the last
 * break opportunity in the row (after a run of spaces or tabs, or before or after a Wide
 * character), else right before that character. Spaces and tabs may hang past the width, a
 * Zero-width character and the character after a zero-width joiner never start a row, and every
 * row holds at least one character.
 *
 * Thread safety: not thread-safe; one owner thread (the editor's UI thread). The Snapshot it holds
 * may be shared with any thread. Allocation failure terminates (noexcept), as in all of frost::text.
 * Complexity: n = items in the layers' trees (one per soft wrap plus one, at least one; one per
 * block plus runs), B = 8.
 */
#pragma once
#include "frost/core/error.hpp"
#include "frost/core/strong_type.hpp"
#include "frost/text/anchor.hpp"
#include "frost/text/document.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_shaper.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace frost::text {

namespace detail {
struct DisplayLayers; // src/text/src/display_layers.hpp: the four layers.
} // namespace detail

// Zero-based display row, block rows included.
using DisplayRow = StrongIndex<struct DisplayRowTag, std::uint32_t>;
// Zero-based byte offset into a display row's display text.
using DisplayColumn = StrongIndex<struct DisplayColumnTag, std::uint32_t>;

// A position on screen: a display row and a column in its display text. Ordered by row, then
// column.
struct DisplayPoint {
    DisplayRow row;
    DisplayColumn column;

    friend constexpr auto operator<=>(const DisplayPoint&, const DisplayPoint&) noexcept = default;
};

enum class WrapMode : std::uint8_t {
    Off,    // One row per line.
    Column, // Wrap at `columns` cells of the shaper's cellWidth().
    Width,  // Wrap at `width` shaper units (the viewport's text width).
};

// Soft-wrap settings. Values below 1 count as 1 (every row still holds one character).
struct WrapSettings {
    WrapMode mode = WrapMode::Off;
    std::uint32_t columns = 120;
    TextWidth width = 0;

    friend constexpr bool operator==(const WrapSettings&, const WrapSettings&) noexcept = default;
};

// Where a block decoration goes relative to the buffer line that holds its anchor.
enum class BlockPlacement : std::uint8_t { Above, Below };

// Stable identity of a block decoration within one DisplayMap; never reused by it.
struct BlockId {
    std::uint64_t value = 0;

    friend constexpr auto operator<=>(BlockId, BlockId) noexcept = default;
};

/*
 * A block decoration: `rows` whole display rows above or below the line holding offset `at`. The
 * anchor follows edits like a registry anchor: Above blocks stick to the text after `at`
 * (Bias::Right), Below blocks to the text before it (Bias::Left), so a block stays with its line's
 * content when lines are inserted next to it; when its line is deleted it goes to the line at the
 * deletion point. At one boundary, the blocks below the upper line come first, then the blocks
 * above the lower line, each group in creation order. `tag` is opaque to the map (the view's paint
 * kind or widget id).
 */
struct BlockDecoration {
    ByteOffset at;
    BlockPlacement placement = BlockPlacement::Below;
    std::uint32_t rows = 1;
    std::uint64_t tag = 0;

    friend constexpr bool operator==(const BlockDecoration&, const BlockDecoration&) noexcept = default;
};

// A block's current state: the decoration with its anchor's current offset, the buffer line it is
// attached to and its first display row.
struct BlockInfo {
    BlockId id;
    BlockDecoration decoration;
    LineIndex line;
    DisplayRow row;
};

enum class RowKind : std::uint8_t { Text, Block };
enum class SegmentKind : std::uint8_t {
    Text,            // Buffer bytes, read from the snapshot.
    FoldPlaceholder, // The placeholder of a folded range.
    Inlay,           // Inline provider text that is not in the buffer.
};

// One piece of a text row's display text, in order.
struct RowSegment {
    SegmentKind kind = SegmentKind::Text;
    // Where the segment starts in the row's display text.
    DisplayColumn column;
    // Text: the bytes shown. FoldPlaceholder: the folded range. Inlay: the empty range at its spot.
    ByteRange buffer;
    // FoldPlaceholder and Inlay: their text, valid until the map changes. Text: empty.
    std::string_view text;
};

/*
 * One display row, as row iteration and rowInfo() report it. Text rows fill line, buffer,
 * startsLine, endsLine and segments; block rows fill block, blockRow, placement, tag and line (the
 * line the block is attached to), and leave buffer empty at the boundary.
 */
struct DisplayRowInfo {
    DisplayRow row;
    RowKind kind = RowKind::Text;
    // Text rows: the buffer line of the row's first byte, and the bytes from the row's start to its
    // content end (never the line break).
    LineIndex line;
    ByteRange buffer;
    bool startsLine = true; // False for a soft-wrapped continuation row.
    bool endsLine = true;   // False when the row ends at a soft wrap.
    // Text rows: the display text in order; empty for an empty row.
    std::vector<RowSegment> segments;
    // Block rows.
    BlockId block;
    std::uint32_t blockRow = 0; // Zero-based row within the block.
    BlockPlacement placement = BlockPlacement::Below;
    std::uint64_t tag = 0;
};

// Display rows [oldFirst, oldEnd) before an update became rows [newFirst, newEnd) after it. An
// update returns these sorted and disjoint; rows outside them only moved.
struct RowEdit {
    DisplayRow oldFirst;
    DisplayRow oldEnd;
    DisplayRow newFirst;
    DisplayRow newEnd;

    friend constexpr bool operator==(const RowEdit&, const RowEdit&) noexcept = default;
};

// A hit test's result: the buffer offset nearest to the point, the bias that keeps a caret on the
// hit row, the display point, and the block when the row is a block row.
struct DisplayHit {
    ByteOffset offset;
    Bias bias = Bias::Right;
    DisplayPoint point;
    std::optional<BlockId> block;
};

// A vertical caret move's result: the new offset and bias, the goal x to keep for the next move,
// and the display point reached.
struct VerticalMove {
    ByteOffset offset;
    Bias bias = Bias::Right;
    TextWidth goalX = 0;
    DisplayPoint point;
};

// Structural counters for the most recent folded update; used by deterministic tests.
struct FoldUpdateStats {
    std::uint64_t replacedRuns = 0;
    std::uint64_t visitedFolds = 0;
    std::uint64_t materializedFoldLists = 0;
    std::uint64_t eventIntervalsRewritten = 0;
    std::uint64_t eventItemsScanned = 0;
};

// Stable identity while a collapsed range remains registered in one display map.
struct FoldId {
    std::uint64_t value = 0;
    friend constexpr auto operator<=>(FoldId, FoldId) noexcept = default;
};
struct FoldInfo {
    FoldId id;
    ByteRange range;
};

// An inline inlay (e.g. a type or parameter hint) anchored at buffer `offset` with `bias`.
struct DisplayInlay {
    ByteOffset offset;
    std::string text;
    Bias bias = Bias::Left;

    friend bool operator==(const DisplayInlay&, const DisplayInlay&) = default;
};

using Inlay = DisplayInlay;

class DisplayMap;

// A forward cursor over display rows (DisplayMap::rows). It borrows the map: valid until the map
// changes or is destroyed. Each step is one O(B log n) lookup. Not thread-safe.
class DisplayRowCursor {
public:
    // Stores the next row in `row` (reusing its segment capacity) and returns true, or returns
    // false after the last row.
    bool next(DisplayRowInfo& row) noexcept;

private:
    friend class DisplayMap;

    DisplayRowCursor(const DisplayMap* map, std::uint32_t next) noexcept : m_map(map), m_next(next) {}

    const DisplayMap* m_map = nullptr;
    std::uint32_t m_next = 0;
};

/*
 * The display map of one document view. It holds the Snapshot it reflects and borrows the shaper.
 * Feed it every ChangeSet of its Document, in order, with the snapshot after it (apply); reset()
 * starts over on unrelated text.
 *
 * Queries clamp external display positions (rows past the end clamp to the last row, columns to the
 * row's end) and take buffer offsets of the held snapshot (FROST_PRE: offset <= size). Every
 * mapping query is O(B log n) plus, for the geometry queries, the length of the row they shape.
 */
class DisplayMap {
public:
    using Inlay = DisplayInlay;

    // The map of `text`, wrapped by `wrap` and measured by `shaper`, with no blocks. O(text) when
    // wrapping, O(1) otherwise. Precondition: `shaper` outlives the map (or is replaced first).
    DisplayMap(Snapshot text, const ITextShaper& shaper, WrapSettings wrap = {}) noexcept;

    // Movable; a moved-from map may only be destroyed or assigned to.
    DisplayMap(DisplayMap&& other) noexcept;
    DisplayMap& operator=(DisplayMap&& other) noexcept;
    ~DisplayMap();
    DisplayMap(const DisplayMap&) = delete ("a display map owns its layers; build another over the same snapshot");
    DisplayMap& operator=(const DisplayMap&) = delete ("a display map owns its layers");

    // The snapshot the map reflects, borrowed until the next update. O(1).
    [[nodiscard]] const Snapshot& text() const noexcept;
    // text().version(). O(1).
    [[nodiscard]] std::uint64_t version() const noexcept;
    [[nodiscard]] const WrapSettings& wrap() const noexcept;
    // The wrap width in shaper units, 0 when wrapping is off. O(1).
    [[nodiscard]] TextWidth wrapWidth() const noexcept;
    // The number of display rows, block rows included; at least 1. O(1).
    [[nodiscard]] std::uint32_t rowCount() const noexcept;

    // Updates.

    /*
     * Applies `changes` (a Document ChangeSet) whose result is `text`. Errors, in order:
     * InvalidArgument if changes.baseVersion != version(), if text.version() != changes.version,
     * or if a version-changing set has no raw edits. On error nothing changes. Returns the display rows that changed.
     * Lines outside the changed lines keep their rows (they are not shaped again); blocks follow their anchors. O(m B
     * log n + the changed lines' bytes + k) for m changes and k blocks on the changed lines. Budget: B5.
     */
    [[nodiscard]] Result<std::vector<RowEdit>> apply(const ChangeSet& changes, const Snapshot& text) noexcept;
    // Rebuilds the map over unrelated `text` and drops every block. Returns one edit over all rows.
    std::vector<RowEdit> reset(Snapshot text) noexcept;
    // Wraps again with `wrap`, keeping the blocks. Returns one edit over all rows. O(text) when
    // wrapping, O(blocks B log n) otherwise.
    std::vector<RowEdit> setWrap(WrapSettings wrap) noexcept;
    // Measures with `shaper` from now on and wraps again (a font or zoom change). Same cost.
    std::vector<RowEdit> setShaper(const ITextShaper& shaper) noexcept;

    // Replaces collapsed ranges, retaining nested ranges even while their parent is collapsed.
    // Ranges must be nonempty, character-aligned, nested or disjoint, and must not split CRLF.
    // The placeholder is "...". Errors leave the map unchanged. On success invalidates all FoldIds.
    // Rebuilds wrapping and blocks;
    // O(f log f + f line lookups + blocks), plus the text shaped when wrapping, for f ranges.
    [[nodiscard]] Result<std::vector<RowEdit>> setFolds(std::vector<ByteRange> ranges) noexcept;
    [[nodiscard]] const std::vector<ByteRange>& folds() const noexcept;
    // True when the range intersects visible hidden text, or an empty range lies strictly inside it.
    // Precondition: range.begin <= range.end <= text().size(). O(log f).
    [[nodiscard]] bool intersectsFold(ByteRange range) const noexcept;
    // Collapsed ranges strictly containing offset, innermost first. O(log f + affected paths).
    [[nodiscard]] std::vector<ByteRange> foldsContaining(ByteOffset offset) const noexcept;
    [[nodiscard]] std::vector<FoldInfo> foldInfosContaining(ByteOffset offset) const noexcept;
    [[nodiscard]] std::vector<FoldInfo> foldInfosIntersecting(ByteRange range) const noexcept;
    // One entry per fold endpoint in the inclusive byte range; a fold may appear twice.
    [[nodiscard]] std::vector<FoldInfo> foldInfosWithBoundaryIn(ByteRange range) const noexcept;
    // Whether a collapsed range starts in the closed buffer range; O(log f + endpoints in range).
    [[nodiscard]] bool foldStartsIn(ByteRange range) const noexcept;
    // Whether a collapsed range starts or ends at offset; O(log f + endpoints at offset).
    [[nodiscard]] bool hasFoldBoundary(ByteOffset offset, bool start) const noexcept;
    [[nodiscard]] FoldUpdateStats foldUpdateStatsForTesting() const noexcept;
    // Incremental registration/removal. Invalid, crossing, duplicate or non-boundary ranges fail.
    [[nodiscard]] Result<FoldId> addFold(ByteRange range) noexcept;
    [[nodiscard]] Result<void> removeFold(FoldId id) noexcept;
    // The first registered fold starting in a closed range, or none.
    [[nodiscard]] std::optional<FoldInfo> firstFoldStartingIn(ByteRange range) const noexcept;
    [[nodiscard]] std::optional<FoldInfo> foldAtBoundary(ByteOffset offset, bool start) const noexcept;

    // Inline inlays (UX-4).

    // Replaces all inline inlays and projects them into their wrap rows.
    void setInlays(std::vector<Inlay> inlays) noexcept;
    // Currently held inline inlays, sorted by (offset, bias) preserving insertion order for ties.
    [[nodiscard]] const std::vector<Inlay>& inlays() const noexcept;

    // Block decorations.

    // Adds a block. Errors, in order: InvalidArgument (rows == 0), OutOfRange (at > size),
    // InvalidArgument (at inside a UTF-8 sequence), OutOfRange (too many blocks or display rows).
    // O(B log n + k) for the k blocks at its boundary.
    [[nodiscard]] Result<BlockId> addBlock(const BlockDecoration& block) noexcept;
    // Removes a block. Errors: NotFound. O(B log n + k).
    [[nodiscard]] Result<void> removeBlock(BlockId id) noexcept;
    // The block's current state. Errors: NotFound. O(B log n + k).
    [[nodiscard]] Result<BlockInfo> blockInfo(BlockId id) const noexcept;
    [[nodiscard]] std::size_t blockCount() const noexcept;

    // Mapping.

    // The display point of buffer `offset` (FROST_PRE: offset <= size); `bias` picks the row at a
    // soft wrap. A CRLF-interior offset maps to its line's end. Budget: B5.
    [[nodiscard]] DisplayPoint toDisplayPoint(ByteOffset offset, Bias bias = Bias::Right) const noexcept;
    // The display point of a buffer point. Errors: Snapshot::offsetAt's (OutOfRange line).
    [[nodiscard]] Result<DisplayPoint> toDisplayPoint(Point point, Bias bias = Bias::Right) const noexcept;
    // The buffer offset of `point`, clamped: a column past the row's end gives its content end, a
    // column inside a character floors to its start. A block row gives the boundary it sits at: the
    // start of the line below for an Above block, the end of the line above for a Below block.
    // Budget: B5.
    [[nodiscard]] ByteOffset toOffset(DisplayPoint point) const noexcept;
    // text().pointAt(toOffset(point)).
    [[nodiscard]] Point toPoint(DisplayPoint point) const noexcept;
    // The display row's description. Precondition: row < rowCount(). Budget: B5.
    [[nodiscard]] DisplayRowInfo rowInfo(DisplayRow row) const noexcept;
    // The same into `out`, reusing its segment capacity.
    void rowInfo(DisplayRow row, DisplayRowInfo& out) const noexcept;
    // A cursor over the rows from `first` (clamped) to the last; for painting. Budget: B5.
    [[nodiscard]] DisplayRowCursor rows(DisplayRow first) const noexcept;

    // Geometry, through the shaper (O(row length) on top of the mapping).

    // The x position of the boundary before `point`'s column in its row (clamped); 0 on a block row.
    [[nodiscard]] TextWidth xAt(DisplayPoint point) const noexcept;
    /*
     * The character boundary nearest to x in `row` (clamped): a character cluster (a character and
     * the Zero-width ones after it) is entered past its middle. x past the row's end gives the
     * row's content end, with Bias::Left at a soft wrap so a caret stays on this row. A block row
     * gives its boundary (toOffset) and the block's id.
     */
    [[nodiscard]] DisplayHit hitTest(DisplayRow row, TextWidth x) const noexcept;
    /*
     * Moves a caret at (`from`, `bias`) by `rows` display rows (negative is up), keeping `goalX`
     * (the caret's own x when not given). Block rows are skipped in the direction of the move;
     * moves past the first or last row stop there. Returns the goal x to pass to the next move.
     */
    [[nodiscard]] VerticalMove moveVertically(ByteOffset from, Bias bias, std::optional<TextWidth> goalX,
                                              std::int32_t rows) const noexcept;

    /*
     * Checks the layers, returning ErrorCode::Internal naming the first violation:
     *   D1 the SumTree invariants S1-S4 of the wrap and block trees;
     *   D2 the wrap rows cover the folded text exactly (bytes, line breaks), each wrap item is
     *      well-formed and every soft wrap lies inside its line;
     *   D3 the block layer's input rows equal the wrap rows, each block sits at the boundary its
     *      anchor gives, in the documented order, and the anchors match the registry and the text.
     * O(n B log n + blocks). For tests.
     */
    [[nodiscard]] Result<void> checkInvariants() const noexcept;

private:
    // Every row edit after a full rebuild: one edit over all rows.
    [[nodiscard]] std::vector<RowEdit> wholeEdit(std::uint32_t oldRows) const noexcept;
    // The nearest text row to `row` in direction `step` (+1 or -1), or the other way when none.
    [[nodiscard]] std::uint32_t nearestTextRow(std::uint32_t row, int step) const noexcept;

    std::unique_ptr<detail::DisplayLayers> m_layers;
};

} // namespace frost::text
