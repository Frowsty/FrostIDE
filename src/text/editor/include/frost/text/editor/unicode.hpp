// SPDX-License-Identifier: Apache-2.0
//
// Character navigation for the editor model (ADR 0025): extended grapheme cluster boundaries
// (UAX #29 through utf8proc), the character classes behind word movement, and code point lookups
// around an offset, all over a frost::text Snapshot.
/*
 * Grapheme clusters. Caret movement and deletion step over whole extended grapheme clusters: an
 * emoji ZWJ sequence, a flag (a pair of regional indicators), a base letter with its combining
 * marks, and CR LF are each one step. Boundaries come from utf8proc_grapheme_break_stateful,
 * scanned forwards from a known boundary: the offset itself for the next boundary, and the start of
 * the offset's line for the previous one (a line break always ends a cluster). Lines longer than
 * kGraphemeBackScan bytes are scanned from that many bytes before the offset instead, so a single
 * cluster longer than that (only in adversarial text) may be split. TODO(M1): the QTextBoundaryFinder
 * cross-check and Unicode's GraphemeBreakTest.txt run (hardening).
 *
 * Words. A cluster's class is the class of its first code point: Word (letters, marks, numbers and
 * connector punctuation such as '_'), Space (Zs, tab and the other non-break controls), LineBreak
 * (LF and CR) and Punctuation (everything else, symbols and emoji included). Word movement skips
 * spaces, then one run of clusters of the same class, and never crosses a line break except to step
 * over it when it starts at one.
 *
 * Every function is pure and safe on any thread for a Snapshot that is not assigned concurrently.
 * Offsets are clipped to the text (Snapshot::clip). Costs are per call: n = pieces, B = 8; a
 * previous-boundary query also scans the line before the offset (at most kGraphemeBackScan bytes).
 */
#pragma once

#include "frost/core/strong_type.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_shaper.hpp"

#include <cstdint>
#include <optional>

namespace frost::text {

// How far back a previous-boundary query scans on a long line (see the header comment).
inline constexpr std::uint64_t kGraphemeBackScan = 4096;

// The class of a code point for word movement (header comment).
enum class CharClass : std::uint8_t { Space, LineBreak, Word, Punctuation };

// The class of `cp` (utf8proc's general category). Pure; O(1).
[[nodiscard]] CharClass charClass(char32_t cp) noexcept;

// The code point that starts at `at`, or nullopt at the end of the text. Precondition: `at` is a
// char boundary <= size (FROST_PRE). O(B log n).
[[nodiscard]] std::optional<DecodedChar> charAt(const Snapshot& text, ByteOffset at) noexcept;
// The code point that ends at `at`, or nullopt at offset 0. Precondition: as charAt. O(B log n).
[[nodiscard]] std::optional<DecodedChar> charBefore(const Snapshot& text, ByteOffset at) noexcept;

// The end of the grapheme cluster that starts at or contains `at`: the smallest boundary > at, or
// size() at the end. O(B log n + cluster bytes); where the characters around `at` do not break by
// the pair rules alone (inside a cluster, between flags), plus previousGraphemeBoundary's scan.
[[nodiscard]] ByteOffset nextGraphemeBoundary(const Snapshot& text, ByteOffset at) noexcept;
// The largest grapheme boundary < at, or 0 at the start. O(B log n + line bytes before `at`).
[[nodiscard]] ByteOffset previousGraphemeBoundary(const Snapshot& text, ByteOffset at) noexcept;
// True when `at` is a grapheme cluster boundary (0 and size() always are).
[[nodiscard]] bool isGraphemeBoundary(const Snapshot& text, ByteOffset at) noexcept;

// Ctrl+Right: from a line end, the start of the next line; otherwise past the spaces after `at` and
// then the run of Word or Punctuation clusters that follows, stopping at the line end.
[[nodiscard]] ByteOffset nextWordBoundary(const Snapshot& text, ByteOffset at) noexcept;
// Ctrl+Left: from a line start, the end of the previous line; otherwise back over the spaces
// before `at` and then the run of Word or Punctuation clusters before them, stopping at the line
// start.
[[nodiscard]] ByteOffset previousWordBoundary(const Snapshot& text, ByteOffset at) noexcept;

} // namespace frost::text
