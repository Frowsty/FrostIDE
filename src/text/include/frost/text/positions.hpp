// SPDX-License-Identifier: Apache-2.0
//
// Position types of frost::text (ADR 0013): byte ranges, line/column points in bytes and in
// UTF-16 code units, line-break kinds and per-line information. Plain values; every type here
// is trivially copyable and safe to share between threads.
/*
 * The line model is LSP 3.17's: LF, CR and CRLF each end a line once, and columns exclude the
 * break. A document of n line breaks has n + 1 lines; the last line has no break (it may be
 * empty). Byte columns count UTF-8 bytes from the line start; UTF-16 columns count UTF-16 code
 * units (LSP's default "utf-16" position encoding).
 */
#pragma once

#include "frost/core/strong_type.hpp"

#include <compare>
#include <cstdint>

namespace frost::text {

// A half-open byte range [begin, end). Invariant (callers' responsibility): begin <= end.
struct ByteRange {
    ByteOffset begin;
    ByteOffset end;

    // The number of bytes in the range. Precondition: begin <= end. O(1).
    [[nodiscard]] constexpr std::uint64_t size() const noexcept { return end - begin; }
    // True when the range holds no bytes. O(1).
    [[nodiscard]] constexpr bool empty() const noexcept { return begin == end; }

    friend constexpr bool operator==(const ByteRange&, const ByteRange&) noexcept = default;
};

// A position as a zero-based line and a byte column within that line (columns exclude the break).
// Ordered by line, then column.
struct Point {
    LineIndex line;
    ByteColumn column;

    friend constexpr auto operator<=>(const Point&, const Point&) noexcept = default;
};

// A position as a zero-based line and a UTF-16 column within that line (an LSP Position).
// Ordered by line, then column.
struct Utf16Point {
    LineIndex line;
    Utf16Column column;

    friend constexpr auto operator<=>(const Utf16Point&, const Utf16Point&) noexcept = default;
};

// The break that ends a line. None for the last line of a document.
enum class LineBreak : std::uint8_t { None, Lf, Cr, CrLf };

// One line: its index, its content (the bytes before the break) and the break that ends it.
// The break's bytes follow content.end: 0 (None), 1 (Lf, Cr) or 2 (CrLf).
struct LineInfo {
    LineIndex line;
    ByteRange content;
    LineBreak lineBreak = LineBreak::None;

    friend constexpr bool operator==(const LineInfo&, const LineInfo&) noexcept = default;
};

} // namespace frost::text
