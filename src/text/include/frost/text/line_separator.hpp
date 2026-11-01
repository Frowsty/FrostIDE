// SPDX-License-Identifier: Apache-2.0
//
// Line separators for frost::text's load and save pipeline (ADR 0018, D9): the separator kinds,
// their bytes and persistence names, line-break statistics in the LSP model (ADR 0013, D2), the
// mixed-EOL flag and the dominant separator that new lines use.
/*
 * Existing line breaks are never rewritten on save: a file keeps its CRLF, LF and lone-CR breaks
 * byte for byte. TextFileFormat::lineSeparator (text_file.hpp) is only what Enter inserts; the
 * loader sets it to the dominant kind of the file, or to a default for files without breaks.
 */
#pragma once

#include "frost/text/positions.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace frost::text {

class Snapshot;

// A line separator kind. The enumerator values are stable and append-only (persisted by name).
enum class LineSeparator : std::uint8_t { Lf, CrLf, Cr };

// "\n", "\r\n", "\r": what Enter inserts (m1/editor-model reads TextFileFormat::lineSeparator).
// O(1). Budget: B5.
[[nodiscard]] constexpr std::string_view separatorBytes(LineSeparator s) noexcept {
    switch (s) {
    case LineSeparator::CrLf: return "\r\n";
    case LineSeparator::Cr: return "\r";
    case LineSeparator::Lf: break;
    }
    return "\n";
}

// "LF", "CRLF", "CR": the status-bar tokens and persistence names. Not translated: technical
// identifiers, like encoding names. O(1).
[[nodiscard]] constexpr std::string_view separatorName(LineSeparator s) noexcept {
    switch (s) {
    case LineSeparator::CrLf: return "CRLF";
    case LineSeparator::Cr: return "CR";
    case LineSeparator::Lf: break;
    }
    return "LF";
}

// The inverse of separatorName, ASCII case-insensitive ("lf", "crlf", "cr", which are also
// EditorConfig end_of_line's values). nullopt otherwise. O(1).
[[nodiscard]] std::optional<LineSeparator> findSeparator(std::string_view name) noexcept;

// The LineBreak (positions.hpp) of a separator: the break an existing line ends with. O(1).
[[nodiscard]] constexpr LineBreak lineBreakOf(LineSeparator s) noexcept {
    switch (s) {
    case LineSeparator::CrLf: return LineBreak::CrLf;
    case LineSeparator::Cr: return LineBreak::Cr;
    case LineSeparator::Lf: break;
    }
    return LineBreak::Lf;
}

// The separator of a line break; LineBreak::None (the last line) has none. O(1).
[[nodiscard]] constexpr std::optional<LineSeparator> separatorOf(LineBreak b) noexcept {
    switch (b) {
    case LineBreak::Lf: return LineSeparator::Lf;
    case LineBreak::CrLf: return LineSeparator::CrLf;
    case LineBreak::Cr: return LineSeparator::Cr;
    case LineBreak::None: break;
    }
    return std::nullopt;
}

// CrLf on Windows, Lf elsewhere: the separator for files without line breaks until m1/settings
// provides a setting. Fixed at compile time. O(1).
[[nodiscard]] LineSeparator platformDefaultSeparator() noexcept;

// Line-break counts in the LSP model (ADR 0013, D2): a CRLF counts once, as crlf; a CR not
// followed by LF is cr; an LF not preceded by CR is lf. A plain value.
struct EolStats {
    std::uint32_t lf = 0;
    std::uint32_t crlf = 0;
    std::uint32_t cr = 0;

    // All line breaks. O(1).
    [[nodiscard]] constexpr std::uint32_t total() const noexcept { return lf + crlf + cr; }
    // At least two of the three counts are non-zero. O(1).
    [[nodiscard]] constexpr bool mixed() const noexcept {
        return (lf != 0 ? 1 : 0) + (crlf != 0 ? 1 : 0) + (cr != 0 ? 1 : 0) >= 2;
    }

    friend constexpr bool operator==(const EolStats&, const EolStats&) noexcept = default;
};

// The line breaks of `text`. Precondition (FROST_PRE): text.size() <= 0xFFFF'FFFE (the buffer
// limit, so counts fit in 32 bits). O(n): vectorisable passes that count LF, CR and CR-LF pairs
// (the pairs only if both occur). Pure and thread-safe. Budget: B15.
[[nodiscard]] EolStats eolStats(std::string_view text) noexcept;
/*
 * The same for a snapshot, in one pass that counts CR bytes over chunks():
 *   cr = summary.lineBreaks - summary.lineFeeds; crlf = crBytes - cr; lf = summary.lineFeeds - crlf.
 * TextSummary already handles CRLFs that straddle pieces. O(n); safe on any thread. Never call it
 * per keystroke, or on the UI thread for large documents: m1/shell-frame refreshes it on a
 * background lane when idle. Callers that only need "mixed" can answer true in O(1) when
 * lineBreaks > lineFeeds > 0 (lone CRs and LFs both present).
 */
[[nodiscard]] EolStats eolStats(const Snapshot& snapshot) noexcept;
// The kind with the largest count. Ties: `fallback` if it is among the largest, else the order Lf,
// CrLf, Cr. No line breaks gives `fallback`. O(1).
[[nodiscard]] LineSeparator dominantSeparator(const EolStats& stats, LineSeparator fallback) noexcept;

} // namespace frost::text
