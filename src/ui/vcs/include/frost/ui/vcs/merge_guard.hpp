// SPDX-License-Identifier: Apache-2.0
//
// The 3-way merge dialog's Apply guard (FND-3): "Apply & Mark Resolved" writes the result pane to
// the working tree and stages it with git add, so it must refuse while the merge model still has
// unresolved chunks or the result text still holds a conflict marker line. Header-only and
// Qt-free, so the guard is tested without a dialog (tests/ui/vcs/merge_guard_test.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace frost::ui {

/*
 * Whether `text` has a line that starts with a git conflict marker: seven '<', '|' or '>'
 * followed by a space, a line end or the end of the text (git's "<<<<<<< ours",
 * "||||||| base" and ">>>>>>> theirs" lines). The "=======" separator alone is not counted:
 * Markdown and reST use such lines on their own, and a real conflict always has the other
 * markers too. Both "\n" and "\r\n" line ends work. Pure; O(text).
 */
[[nodiscard]] inline bool hasConflictMarkers(std::string_view text) noexcept {
    constexpr std::size_t kMarkerLength = 7;
    std::size_t lineStart = 0;
    while (lineStart < text.size()) {
        const std::size_t lineEnd = text.find('\n', lineStart);
        const std::string_view line =
            text.substr(lineStart, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - lineStart);
        for (const char marker : {'<', '|', '>'}) {
            if (line.size() < kMarkerLength ||
                line.substr(0, kMarkerLength).find_first_not_of(marker) != std::string_view::npos) {
                continue;
            }
            if (line.size() == kMarkerLength || line[kMarkerLength] == ' ' || line[kMarkerLength] == '\r') {
                return true;
            }
        }
        if (lineEnd == std::string_view::npos) {
            break;
        }
        lineStart = lineEnd + 1;
    }
    return false;
}

/*
 * Why Apply must refuse for a merge with `unresolvedChunks` chunks the model has not resolved and
 * the result pane's `resultText`, or "" when it may write the file. The message is for the
 * dialog's status line and the Apply button's tooltip. Pure; O(resultText).
 */
[[nodiscard]] inline std::string mergeApplyBlocker(std::uint32_t unresolvedChunks, std::string_view resultText) {
    if (unresolvedChunks > 0) {
        return "Resolve the remaining " + std::to_string(unresolvedChunks) +
               (unresolvedChunks == 1 ? " conflict" : " conflicts") + " before applying.";
    }
    if (hasConflictMarkers(resultText)) {
        return "The result still contains conflict markers; remove them before applying.";
    }
    return {};
}

} // namespace frost::ui
