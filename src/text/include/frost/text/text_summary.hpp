// SPDX-License-Identifier: Apache-2.0
//
// TextSummary: the monoid that frost::text caches in every piece-tree node (ADR 0012, D1 and D2).
// It counts bytes, UTF-16 code units, characters (Unicode scalar values), line breaks and line
// feeds, and keeps the line metrics needed to answer line/column queries and the longest line
// of any concatenation in O(1) from its parts.
/*
 * Provenance (clean room; ADR 0012, Sources): the line metrics serve PLAN.md section 3's "longest
 * line" summary. Combining two texts' longest lines in O(1) needs each side's first and last
 * partial rows (the rows that join at the seam) and its longest row with that row's index; the
 * combine rules below follow from that. Zed's rope crate (GPL), whose TextSummary has similar
 * line fields, was not consulted; ADR 0012 records that resemblance, which a review found.
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace frost::text {

/*
 * A summary of a UTF-8 text. Plain value, 64 bytes, a literal type; value-initialised it is the
 * summary of the empty text, which is combine's identity.
 *
 * Line model (LSP 3.17): LF, CR and CRLF each count as one line break, so a text has lineBreaks
 * + 1 lines (rows 0..lineBreaks). Line lengths exclude the break. "First line" is row 0 and
 * "last line" is row lineBreaks; for a text without breaks both are the whole text.
 *
 * Counting rules, all per byte so that summaries are exact and split-invariant:
 *   - chars counts the bytes that are not continuation bytes (0x80..0xBF);
 *   - utf16 = chars + the count of bytes >= 0xF0 (4-byte sequences need a surrogate pair);
 *   - lineFeeds counts LF bytes; lineBreaks - lineFeeds is the number of lone CRs.
 * For well-formed UTF-8 chars and utf16 are the scalar-value and UTF-16 counts; for other input
 * they are deterministic but meaningless. Bytes, breaks, line feeds and line byte lengths are
 * exact for any input. The 32-bit fields hold every value for texts of at most 0xFFFF'FFFE bytes,
 * the buffer limit (D6); longer inputs are outside the domain.
 *
 * startsWithLf and endsWithCr let combine() recognise a CRLF split across two summaries, which
 * then counts once.
 */
struct TextSummary {
    std::uint64_t bytes = 0;
    std::uint64_t utf16 = 0;
    std::uint64_t chars = 0;
    std::uint32_t lineBreaks = 0;       // LF, CR and CRLF each once; lineCount = lineBreaks + 1.
    std::uint32_t lineFeeds = 0;        // LF bytes; lineBreaks - lineFeeds = lone CRs.
    std::uint32_t firstLineChars = 0;   // Characters of row 0, excluding its break.
    std::uint32_t lastLineChars = 0;    // Characters of row lineBreaks.
    std::uint32_t lastLineBytes = 0;    // Bytes of row lineBreaks.
    std::uint32_t lastLineUtf16 = 0;    // UTF-16 units of row lineBreaks.
    std::uint32_t longestLineChars = 0; // Characters of the longest row.
    std::uint32_t longestLine = 0;      // The first (smallest) row among the longest.
    bool startsWithLf = false;          // The first byte is LF.
    bool endsWithCr = false;            // The last byte is CR.

    friend constexpr bool operator==(const TextSummary&, const TextSummary&) noexcept = default;
};
static_assert(sizeof(TextSummary) == 64, "TextSummary is 64 bytes (ADR 0012: piece and node sizes)");

/*
 * combine(a, b) is the summary of the concatenation of a's text and b's text. It is a monoid
 * with identity TextSummary{}: associative, pure and O(1). Rules:
 *   - If a.bytes == 0 the result is b; if b.bytes == 0 it is a.
 *   - bytes, utf16, chars and lineFeeds add. crlf = a.endsWithCr && b.startsWithLf, and
 *     lineBreaks = a.lineBreaks + b.lineBreaks - crlf. startsWithLf comes from a, endsWithCr
 *     from b.
 *   - If crlf: a's last row and b's first row are both the empty gap between the CR and the LF,
 *     which is not a row of the concatenation. firstLineChars comes from a and the last* fields
 *     from b (a ends with a break, b starts with one). The longest line is the best of a's
 *     (a.longestLineChars, a.longestLine) and, only if b.longestLineChars > 0, b's
 *     (b.longestLineChars, a.lineBreaks + b.longestLine - 1); b's row 0 is the empty gap, so a
 *     positive length implies b.longestLine >= 1.
 *   - Otherwise a's last row and b's first row join into one row, row a.lineBreaks, of
 *     joined = a.lastLineChars + b.firstLineChars characters. firstLineChars is joined if
 *     a.lineBreaks == 0, else a.firstLineChars. The last* fields are a's plus b's totals (chars,
 *     bytes, utf16) if b.lineBreaks == 0, else b's. The longest line is the best of
 *     (a.longestLineChars, a.longestLine), (joined, a.lineBreaks) and
 *     (b.longestLineChars, a.lineBreaks + b.longestLine).
 *   - "Best" means the most characters; ties go to the smallest row.
 * Thread-safe (pure).
 */
[[nodiscard]] TextSummary combine(const TextSummary& a, const TextSummary& b) noexcept;

// The summary of `utf8` (at most 0xFFFF'FFFE bytes). O(n), pure and thread-safe; never allocates.
// Bytes, line breaks, line feeds and line byte lengths are exact for any input; chars and utf16
// are exact for well-formed UTF-8 and deterministic otherwise (see TextSummary). Budget: B15.
[[nodiscard]] TextSummary summarize(std::string_view utf8) noexcept;

} // namespace frost::text
