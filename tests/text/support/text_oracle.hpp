// SPDX-License-Identifier: Apache-2.0
//
// TextOracle: a brute-force model of frost::text over std::string, the independent oracle of the
// piece-tree differential, stress and fuzz tests (ADR 0012, section 9.1). No gtest dependency.
/*
 * Independence. The oracle shares no code with frost::text or frost::io beyond the plain value
 * types (Point, TextSummary, TextChange, ErrorCode):
 *   - its UTF-8 validator decodes by bit pattern (0xxxxxxx, 110xxxxx, 1110xxxx, 11110xxx, then
 *     10xxxxxx continuations), assembles the scalar value and rejects truncation, stray
 *     continuations, non-shortest forms (the re-encoded length differs), D800..DFFF and values
 *     above 10FFFF; io uses table 3-7's byte ranges instead;
 *   - characters and UTF-16 units are counted by decoding, not by text's per-byte rule;
 *   - lines are split by LF, CR and CRLF directly into tables (line starts, content ends,
 *     breaks, per-position UTF-16 counts), and its TextSummary is derived from those tables;
 *   - every conversion, the CRLF-interior rule, the round-trip rules, the edit check order and
 *     the TextChange normalisation (D8) are reimplemented from their definitions.
 * Comparing a buffer's summary with text::summarize(full text) is a consistency check, labelled so.
 *
 * Cost: edits are std::string operations; the tables are rebuilt only by rebuild() (full checks).
 * Not thread-safe.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/text/text_summary.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::text::test {

// The oracle's UTF-8 decoder: the length (1-4) of the well-formed sequence starting at `i`, or 0.
// Stores the scalar value in `scalar` on success.
[[nodiscard]] std::size_t oracleDecode(std::string_view text, std::size_t i, char32_t& scalar) noexcept;
// The offset of the first byte of the first ill-formed sequence, or text.size() when well formed.
[[nodiscard]] std::size_t oracleValidPrefix(std::string_view text) noexcept;
// True when `text` is well formed by the oracle's validator.
[[nodiscard]] inline bool oracleValid(std::string_view text) noexcept { return oracleValidPrefix(text) == text.size(); }
// The summary of `text` (well formed): one decoding pass that splits lines by LF, CR and CRLF
// itself, without tables (TextOracle::summary() derives the same from its tables). O(n).
[[nodiscard]] TextSummary oracleSummary(std::string_view text);
// The point of offset `x` (<= text.size()) by a linear scan, with the CRLF-interior rule: lines
// end at LF, CR and CRLF; an offset between CR and LF reports the CR's point. O(x).
[[nodiscard]] Point linearPointAt(std::string_view text, std::uint64_t x);
// The offset of an LSP position by LSP's rules (line start plus the column clamped to the line's
// content, byte columns) by a linear scan. Precondition: point.line < the text's line count. O(n).
[[nodiscard]] std::uint64_t linearLspOffset(std::string_view text, Point point);

// A normalised change in bytes (D8), as the oracle computes it.
struct ByteChange {
    std::uint64_t start = 0;
    std::uint64_t oldEnd = 0;
    std::uint64_t newEnd = 0;

    friend bool operator==(const ByteChange&, const ByteChange&) = default;
};
// The D8-normalised change for replacing [a, b) of `before` by `inserted`.
[[nodiscard]] ByteChange expectedChange(std::string_view before, std::uint64_t a, std::uint64_t b,
                                        std::string_view inserted);
// True when 0 < x < text.size(), text[x - 1] == CR and text[x] == LF.
[[nodiscard]] bool crlfInterior(std::string_view text, std::uint64_t x) noexcept;

class TextOracle {
public:
    explicit TextOracle(std::string text = {});

    [[nodiscard]] const std::string& text() const noexcept { return m_text; }
    [[nodiscard]] std::uint64_t size() const noexcept { return m_text.size(); }
    // Replaces the text; the tables become stale until rebuild().
    void setText(std::string text);
    // Replaces [a, b) with `inserted` (no validation); the tables become stale.
    void replace(std::uint64_t a, std::uint64_t b, std::string_view inserted);

    // True when `x` (<= size) is not inside a multi-byte sequence, by decoding up to 3 bytes back.
    // O(1); needs no tables. False beyond size, true at size.
    [[nodiscard]] bool isCharBoundary(std::uint64_t x) const noexcept;
    // `x` floored to a char boundary (x <= size). O(1).
    [[nodiscard]] std::uint64_t floorChar(std::uint64_t x) const noexcept;

    // The shared edit check order (text_buffer.hpp), checks 1-4: the expected error, if any.
    [[nodiscard]] std::optional<ErrorCode> checkEdit(std::uint64_t a, std::uint64_t b,
                                                     std::string_view inserted) const noexcept;

    // Rebuilds the line and UTF-16 tables from the current text. O(size).
    void rebuild();

    // Queries below need fresh tables (rebuild() after the last change).
    [[nodiscard]] std::uint32_t lineCount() const noexcept { return static_cast<std::uint32_t>(m_lineStarts.size()); }
    [[nodiscard]] TextSummary summary() const;
    [[nodiscard]] std::uint64_t utf16Size() const noexcept { return m_utf16Before.back(); }
    [[nodiscard]] Point pointAt(std::uint64_t x) const;
    [[nodiscard]] Utf16Point utf16PointAt(std::uint64_t x) const;
    [[nodiscard]] std::uint64_t utf16At(std::uint64_t x) const;
    [[nodiscard]] std::uint64_t clip(std::uint64_t x) const;
    // nullopt where the buffer must return OutOfRange.
    [[nodiscard]] std::optional<std::uint64_t> offsetAt(Point point) const;
    [[nodiscard]] std::optional<std::uint64_t> offsetAt(Utf16Point point) const;
    [[nodiscard]] std::optional<std::uint64_t> offsetAtUtf16(std::uint64_t units) const;
    [[nodiscard]] std::optional<LineInfo> lineInfo(std::uint32_t line) const;
    // The offset of an LSP position by LSP's rules: line start plus the column clamped to the line's
    // content (byte columns). Precondition: point.line < lineCount().
    [[nodiscard]] std::uint64_t lspOffset(Point point) const;
    // crlfFloor(y): y - 1 when y is CRLF-interior, else y.
    [[nodiscard]] std::uint64_t crlfFloor(std::uint64_t y) const noexcept;

private:
    [[nodiscard]] std::uint32_t lineOf(std::uint64_t y) const;

    std::string m_text;
    // Tables (rebuild()).
    std::vector<std::uint64_t> m_lineStarts;
    std::vector<std::uint64_t> m_contentEnds;
    std::vector<LineBreak> m_breaks;
    std::vector<std::uint64_t> m_utf16Before; // Size + 1 entries; inside a char, the char start's.
    std::vector<std::uint64_t> m_charsBefore; // Likewise for scalar values.
};

} // namespace frost::text::test
