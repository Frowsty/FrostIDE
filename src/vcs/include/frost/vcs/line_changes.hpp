// SPDX-License-Identifier: Apache-2.0
//
// Editor change markers (M5, ADR 0048): the line-level changes of an open buffer against its HEAD
// version (frost::diff's histogram diff), what the gutter draws as added, modified and deleted
// marks, and the edit that rolls one change back ("Rollback Lines").
/*
 * Both texts are LineSequences with LineSplit::Lsp, so line i is the editor's line i
 * (frost::text::Snapshot's model): build `base` from the HEAD bytes (GitRepository::headContent,
 * filtered as a checkout writes them) and `current` from the document's snapshot. Each hunk of the
 * diff becomes one LineChange: lines only in the buffer are Added, lines only in HEAD are Deleted
 * (marked at the buffer line they were removed before), and replaced lines are Modified.
 * Everything here is pure and thread-safe; the UI runs it debounced as the user types.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/diff/line_diff.hpp"
#include "frost/diff/line_sequence.hpp"
#include "frost/text/positions.hpp"

#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::vcs {

enum class LineChangeKind : std::uint8_t { Added, Modified, Deleted };

// "added", "modified" or "deleted".
[[nodiscard]] std::string_view toString(LineChangeKind kind) noexcept;

// One change of the buffer against its base.
struct LineChange {
    LineChangeKind kind = LineChangeKind::Modified;
    diff::LineRange current; // The buffer's lines; empty for Deleted (begin: the line they were removed before).
    diff::LineRange base;    // The base's lines; empty for Added.

    friend bool operator==(const LineChange&, const LineChange&) = default;
};

/*
 * The changes of `current` against `base`, in line order. Errors: those of diff::diffLines
 * (OutOfRange for huge texts, Cancelled). A diff that hit the work cap still gives valid, coarser
 * changes. O(diff).
 */
[[nodiscard]] Result<std::vector<LineChange>>
computeLineChanges(const diff::LineSequence& base, const diff::LineSequence& current, std::stop_token stop = {});

// The base's text of `change` (its old lines, breaks included); "" for Added. O(1).
[[nodiscard]] std::string_view baseText(const diff::LineSequence& base, const LineChange& change) noexcept;

// What rolling `change` back does to the buffer: replace `range` (bytes of `current`) with `text`.
struct LineRollback {
    text::ByteRange range;
    std::string text;

    friend bool operator==(const LineRollback&, const LineRollback&) = default;
};

// The edit that restores `change`'s base lines in the buffer. Precondition: `change` came from
// computeLineChanges(base, current). O(change).
[[nodiscard]] LineRollback rollbackEdit(const diff::LineSequence& base, const diff::LineSequence& current,
                                        const LineChange& change);

// The change that covers buffer line `line` (for Deleted: line == current.begin), or nullptr. O(log n).
[[nodiscard]] const LineChange* changeAtLine(std::span<const LineChange> changes, LineIndex line) noexcept;

} // namespace frost::vcs
