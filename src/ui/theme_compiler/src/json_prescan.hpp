// SPDX-License-Identifier: Apache-2.0
//
// The Qt-free JSON pre-scan of the theme compiler (private): one pass over the text that
// measures nesting depth and finds member names repeated within one object, which
// QJsonDocument would silently collapse to a single member (ADR 0015 decision 2).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace frost::ui::detail {

struct DuplicateMember {
    std::string jsonPath; // The repeated member's path ("ui.Button.arc"), with names unescaped.
};

struct PrescanResult {
    // The nesting depth reached (the top-level value is 1); scanning stops once it exceeds
    // the limit, so this is at most limit + 1.
    int depth = 0;
    bool depthExceeded = false;
    // The text is not well-formed JSON (scanning stopped there; QJsonDocument reports it).
    bool syntaxError = false;
    std::vector<DuplicateMember> duplicates; // In text order.
};

/*
 * Scans `text` (valid UTF-8 without a BOM) as RFC 8259 JSON. String escapes, including
 * \uXXXX surrogate pairs, are decoded before names are compared, so "a" and "a" collide.
 * Paths join member names with '.' and add "[i]" for array elements. Pure, O(n), recursion
 * depth bounded by `maxDepth` + 1.
 */
[[nodiscard]] PrescanResult prescanJson(std::string_view text, int maxDepth);

} // namespace frost::ui::detail
