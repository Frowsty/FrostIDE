// SPDX-License-Identifier: Apache-2.0
//
// Private helpers of the editor commands (ADR 0025): BatchBuilder collects one edit batch and maps
// offsets into the text after it, plus state construction and the line queries the line
// operations share.
#pragma once

#include "frost/core/strong_type.hpp"
#include "frost/text/anchor.hpp"
#include "frost/text/editor/commands.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/selection.hpp"
#include "frost/text/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::text::detail {

/*
 * Collects the replacements of one batch in any order and, after finish(), maps offsets into the
 * text after it. finish() sorts by (begin, end, insertion order), so inserts at one offset keep
 * their order and come before a range that starts there. An edit identical to an earlier one (two
 * carets asking for the same change) is merged into it; any other edit that overlaps an earlier
 * one is dropped, and its post() offsets are those of its range's begin.
 */
class BatchBuilder {
public:
    // Adds a replacement of `range` of the input text by `text`; returns its id.
    std::size_t add(ByteRange range, std::string text);
    // Sorts the batch and computes the offsets after it. Call once, before post() and map().
    void finish() noexcept;

    // The offset after the batch of byte `k` of edit `id`'s new text (k <= its size).
    [[nodiscard]] ByteOffset post(std::size_t id, std::uint64_t k) const noexcept;
    // `offset` of the input text mapped through the batch with `bias` (anchor.hpp's rule).
    [[nodiscard]] ByteOffset map(ByteOffset offset, Bias bias) const noexcept;
    // The batch as raw edits (after finish()).
    [[nodiscard]] std::span<const RawEdit> raw() const noexcept { return m_raw; }
    [[nodiscard]] bool empty() const noexcept { return m_edits.empty(); }
    // The sorted edits, no-ops dropped (after finish()); leaves the builder empty.
    [[nodiscard]] std::vector<TextEdit> take() noexcept;

private:
    static constexpr std::size_t kDropped = static_cast<std::size_t>(-1);

    std::vector<TextEdit> m_edits;     // In insertion order until finish(), then sorted and kept.
    std::vector<ByteRange> m_ranges;   // id -> its range, for dropped edits.
    std::vector<std::size_t> m_slot;   // id -> kept index, or kDropped.
    std::vector<std::int64_t> m_shift; // Sorted index -> the size change of the edits before it.
    std::vector<RawEdit> m_raw;
};

// Normalises `selections` (with their parallel caret states) into a state; `primary` indexes them.
[[nodiscard]] EditorState makeState(std::span<const Selection> selections, std::span<const CaretState> carets,
                                    std::size_t primary) noexcept;
// `state`'s selections mapped through `raw` (SelectionSet::transformed), goal x cleared.
[[nodiscard]] EditorState mapState(const EditorState& state, std::span<const RawEdit> raw) noexcept;
// The selections of `state`, or one caret at 0 when it has none.
[[nodiscard]] std::vector<Selection> selectionsOf(const EditorState& state) noexcept;
// The index of the primary selection among selectionsOf(state).
[[nodiscard]] std::size_t primaryOf(const EditorState& state) noexcept;

// Line queries.

// The line holding `at` (a CRLF-interior offset reports the CR's line).
[[nodiscard]] LineInfo lineOf(const Snapshot& text, ByteOffset at) noexcept;
// Line `line` (FROST_PRE: line < lineCount).
[[nodiscard]] LineInfo lineAt(const Snapshot& text, std::uint32_t line) noexcept;
// The offset after the line's break (the content end for the last line).
[[nodiscard]] ByteOffset lineEndWithBreak(const LineInfo& line) noexcept;
// The bytes of a line break kind.
[[nodiscard]] std::string_view breakText(LineBreak lineBreak) noexcept;
// The line break new lines get: the text's first line break, else `fallback`.
[[nodiscard]] std::string_view documentBreak(const Snapshot& text, LineBreak fallback) noexcept;
// The first offset in the line's content that is not a space or tab (the content end if none).
[[nodiscard]] ByteOffset indentEnd(const Snapshot& text, const LineInfo& line) noexcept;
// The offset after the last character of the line's content that is not a space or tab (the
// content end when the line is blank: then the indentation is the whole line).
[[nodiscard]] ByteOffset trimmedEnd(const Snapshot& text, const LineInfo& line) noexcept;
// The display column of `at` on its line, with tabs to multiples of `tabSize` and one column per
// other code point.
[[nodiscard]] std::uint64_t visualColumn(const Snapshot& text, const LineInfo& line, ByteOffset at,
                                         std::uint32_t tabSize) noexcept;

// An inclusive run of lines.
struct LineSpan {
    std::uint32_t first = 0;
    std::uint32_t last = 0;
};

// The lines a selection touches: a non-empty selection that ends at the start of a later line does
// not touch that line.
[[nodiscard]] LineSpan touchedLines(const Snapshot& text, const Selection& selection) noexcept;
// The spans of `selections` in order, merged when they overlap (and when they are adjacent with
// `mergeAdjacent`).
[[nodiscard]] std::vector<LineSpan> touchedSpans(const Snapshot& text, std::span<const Selection> selections,
                                                 bool mergeAdjacent) noexcept;

} // namespace frost::text::detail
