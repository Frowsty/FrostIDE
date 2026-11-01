// SPDX-License-Identifier: Apache-2.0
//
// Grapheme cluster boundaries, character classes and word boundaries over a Snapshot
// (frost/text/editor/unicode.hpp, ADR 0025). The cluster rules are utf8proc's
// (utf8proc_grapheme_break_stateful, UAX #29 extended grapheme clusters).

#include "frost/text/editor/unicode.hpp"

#include "frost/core/assert.hpp"

#include "char_cursor.hpp"

#include <optional>
#include <vector>

#include <utf8proc.h>

namespace frost::text {

namespace detail {

bool ForwardClusters::next(Cluster& out) noexcept {
    if (!m_has) {
        return false;
    }
    out.range.begin = m_at;
    out.first = m_char.cp;
    char32_t previous = m_char.cp;
    ByteOffset end = m_at + m_char.bytes;
    // utf8proc's state may be reset to zero after a break, so each cluster starts from zero.
    utf8proc_int32_t state = 0;
    while ((m_has = m_chars.next(m_at, m_char))) {
        if (utf8proc_grapheme_break_stateful(static_cast<utf8proc_int32_t>(previous),
                                             static_cast<utf8proc_int32_t>(m_char.cp), &state)) {
            break;
        }
        previous = m_char.cp;
        end = m_at + m_char.bytes;
    }
    out.range.end = end;
    return true;
}

} // namespace detail

namespace {

// The line holding `at` (a CRLF-interior offset reports the CR's line).
LineInfo lineOf(const Snapshot& text, ByteOffset at) noexcept { return *text.lineInfo(text.lineAt(at)); }

// Where a backward scan on `line` that must reach `at` starts (header comment of unicode.hpp).
ByteOffset scanStart(const Snapshot& text, const LineInfo& line, ByteOffset at) noexcept {
    if (at - line.content.begin <= kGraphemeBackScan) {
        return line.content.begin;
    }
    return text.clip(at - kGraphemeBackScan);
}

CharClass clusterClass(char32_t first) noexcept { return charClass(first); }

} // namespace

CharClass charClass(char32_t cp) noexcept {
    if (cp == U'\n' || cp == U'\r') {
        return CharClass::LineBreak;
    }
    if (cp == U'\t' || cp == U'\v' || cp == U'\f') {
        return CharClass::Space;
    }
    switch (utf8proc_category(static_cast<utf8proc_int32_t>(cp))) {
    case UTF8PROC_CATEGORY_LU:
    case UTF8PROC_CATEGORY_LL:
    case UTF8PROC_CATEGORY_LT:
    case UTF8PROC_CATEGORY_LM:
    case UTF8PROC_CATEGORY_LO:
    case UTF8PROC_CATEGORY_MN:
    case UTF8PROC_CATEGORY_MC:
    case UTF8PROC_CATEGORY_ME:
    case UTF8PROC_CATEGORY_ND:
    case UTF8PROC_CATEGORY_NL:
    case UTF8PROC_CATEGORY_NO:
    case UTF8PROC_CATEGORY_PC: return CharClass::Word;
    case UTF8PROC_CATEGORY_ZS:
    case UTF8PROC_CATEGORY_ZL:
    case UTF8PROC_CATEGORY_ZP: return CharClass::Space;
    default: return CharClass::Punctuation;
    }
}

std::optional<DecodedChar> charAt(const Snapshot& text, ByteOffset at) noexcept {
    FROST_PRE(at <= text.size() && text.isCharBoundary(at));
    if (at == text.size()) {
        return std::nullopt;
    }
    return decodeUtf8(text.chunkAt(at));
}

std::optional<DecodedChar> charBefore(const Snapshot& text, ByteOffset at) noexcept {
    FROST_PRE(at <= text.size() && text.isCharBoundary(at));
    detail::BackwardChars chars(text, at);
    ByteOffset where;
    DecodedChar c;
    if (!chars.next(where, c)) {
        return std::nullopt;
    }
    return c;
}

ByteOffset nextGraphemeBoundary(const Snapshot& text, ByteOffset at) noexcept {
    at = text.clip(at);
    /*
     * When the pair rules alone see no break between the characters around `at` (a combining mark,
     * a ZWJ, a regional indicator after another, CR LF), `at` may be inside a cluster whose context
     * rules need its start: scan from the previous boundary. Otherwise `at` starts a cluster.
     */
    ByteOffset start = at;
    const std::optional<DecodedChar> before = charBefore(text, at);
    const std::optional<DecodedChar> after = charAt(text, at);
    if (before && after &&
        !utf8proc_grapheme_break(static_cast<utf8proc_int32_t>(before->cp), static_cast<utf8proc_int32_t>(after->cp))) {
        start = previousGraphemeBoundary(text, at);
    }
    detail::ForwardClusters clusters(text, start);
    detail::Cluster cluster;
    while (clusters.next(cluster)) {
        if (cluster.range.end > at) {
            return cluster.range.end;
        }
    }
    return at;
}

ByteOffset previousGraphemeBoundary(const Snapshot& text, ByteOffset at) noexcept {
    at = text.clip(at);
    if (at == ByteOffset{0}) {
        return at;
    }
    const LineInfo line = lineOf(text, at);
    if (at <= line.content.begin) {
        // At a line start: the previous line's break is one cluster (CR LF included).
        return text.lineInfo(line.line - 1u)->content.end;
    }
    if (at > line.content.end) {
        return line.content.end; // Between the CR and the LF of a CRLF.
    }
    detail::ForwardClusters clusters(text, scanStart(text, line, at));
    detail::Cluster cluster;
    ByteOffset last = line.content.begin;
    while (clusters.next(cluster) && cluster.range.begin < at) {
        last = cluster.range.begin;
    }
    return last;
}

bool isGraphemeBoundary(const Snapshot& text, ByteOffset at) noexcept {
    at = text.clip(at);
    if (at == ByteOffset{0} || at == text.size()) {
        return true;
    }
    return nextGraphemeBoundary(text, previousGraphemeBoundary(text, at)) == at;
}

ByteOffset nextWordBoundary(const Snapshot& text, ByteOffset at) noexcept {
    at = text.clip(at);
    detail::ForwardClusters clusters(text, at);
    detail::Cluster cluster;
    if (!clusters.next(cluster)) {
        return at;
    }
    if (clusterClass(cluster.first) == CharClass::LineBreak) {
        return cluster.range.end; // From a line end to the next line's start.
    }
    // Skip the spaces; stop at a line end.
    while (clusterClass(cluster.first) == CharClass::Space) {
        if (!clusters.next(cluster)) {
            return text.size();
        }
    }
    const CharClass run = clusterClass(cluster.first);
    if (run == CharClass::LineBreak) {
        return cluster.range.begin;
    }
    ByteOffset end = cluster.range.end;
    while (clusters.next(cluster) && clusterClass(cluster.first) == run) {
        end = cluster.range.end;
    }
    return end;
}

ByteOffset previousWordBoundary(const Snapshot& text, ByteOffset at) noexcept {
    at = text.clip(at);
    if (at == ByteOffset{0}) {
        return at;
    }
    const LineInfo line = lineOf(text, at);
    if (at <= line.content.begin || at > line.content.end) {
        return previousGraphemeBoundary(text, at); // From a line start to the previous line's end.
    }
    // The clusters of the line before `at`, scanned forwards, then walked backwards.
    std::vector<detail::Cluster> before;
    detail::ForwardClusters clusters(text, scanStart(text, line, at));
    detail::Cluster cluster;
    while (clusters.next(cluster) && cluster.range.begin < at) {
        before.push_back(cluster);
    }
    std::size_t i = before.size();
    while (i > 0 && clusterClass(before[i - 1].first) == CharClass::Space) {
        --i;
    }
    if (i == 0) {
        return before.empty() ? at : before.front().range.begin;
    }
    const CharClass run = clusterClass(before[i - 1].first);
    while (i > 0 && clusterClass(before[i - 1].first) == run) {
        --i;
    }
    return before[i].range.begin;
}

} // namespace frost::text
