// SPDX-License-Identifier: Apache-2.0
//
// The Qt-free JSON pre-scan of the theme compiler (private): one pass over the text that
// measures nesting depth and finds member names repeated within one object, which
// QJsonDocument would silently collapse to a single member (ADR 0015 decision 2).
#pragma once

#include "frost/ui/theme_compiler/theme_compiler.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace frost::ui::detail {

struct DuplicateMember {
    // The repeated member's path ("ui.Button.arc"), with names unescaped and elided by
    // elideForDiagnostic (at most kMaxDiagnosticPathBytes plus the "..." marker).
    std::string jsonPath;
};

struct PrescanResult {
    // The nesting depth reached (the top-level value is 1); scanning stops once it exceeds
    // the limit, so this is at most limit + 1.
    int depth = 0;
    bool depthExceeded = false;
    // The text is not well-formed JSON (scanning stopped there; QJsonDocument reports it).
    bool syntaxError = false;
    // The first `maxReported` duplicates, in text order, and how many there are in total.
    std::vector<DuplicateMember> duplicates;
    std::size_t duplicateCount = 0;
};

/*
 * Scans `text` (valid UTF-8 without a BOM) as RFC 8259 JSON. String escapes, including
 * \uXXXX surrogate pairs, are decoded before names are compared, so "a" and its escaped
 * spelling (a backslash, then u0061) collide. Paths join member names with '.' and add "[i]"
 * for array elements; they are built only for the first `maxReported` duplicates (the scan
 * keeps a stack of pointers to the names, never a path per member or element), so memory is
 * linear in the input whatever its shape. Pure. O(n log n) time (one name set per object, plus
 * maxReported path joins of at most n bytes each), O(n) memory, recursion depth bounded by
 * `maxDepth` + 1.
 */
[[nodiscard]] PrescanResult prescanJson(std::string_view text, int maxDepth, std::size_t maxReported);

/*
 * `text` when it is at most `maxBytes` long; otherwise its first and last (maxBytes / 2)
 * bytes, each cut back to a UTF-8 character boundary, joined by "...". Pure, O(maxBytes).
 */
[[nodiscard]] std::string elideForDiagnostic(std::string_view text, std::size_t maxBytes = kMaxDiagnosticPathBytes);

} // namespace frost::ui::detail
