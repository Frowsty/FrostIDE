// SPDX-License-Identifier: Apache-2.0
//
// frost::syntax::SyntaxTree (ADR 0023, D5 and D7): an immutable, shareable tree-sitter tree in the
// coordinates of one text snapshot, with the queries the editor needs: highlight spans for a
// range, the outline, and the enclosing ranges for extend selection.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/syntax/highlight.hpp"
#include "frost/syntax/language.hpp"
#include "frost/syntax/outline.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/snapshot.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace frost::syntax {

namespace detail {
struct TreeState;
} // namespace detail

// Bounds for one query call (ADR 0023 D6 d). A result that hits any bound is returned with
// complete = false, never as an error.
struct QueryLimits {
    // The clock for `budget`; null means systemClock(). Must outlive the call.
    const IClock* clock = nullptr;
    // Wall time allowed for the query; zero means unlimited. Checked every ~1000 query steps.
    IClock::Duration budget = std::chrono::milliseconds(1);
    // tree-sitter's in-progress match limit (ts_query_cursor_set_match_limit).
    std::uint32_t matchLimit = 1024;
    // The most captures (highlights) or items (outline) collected.
    std::uint32_t maxResults = 100'000;
    // The deepest outline nesting kept (depth 0 is top level); deeper items are dropped.
    std::uint32_t maxDepth = 64;
    // Optional cooperative cancellation; a stopped query is incomplete.
    std::stop_token stop{};
};

// Fold ranges may nest; ordered by start, outer before inner.
struct FoldResult {
    std::vector<text::ByteRange> ranges;
    bool complete = true;
};

/*
 * A parsed tree plus the snapshot whose coordinates it is in. While a reparse is pending the
 * session publishes the last parsed tree edited with every change since (stale() is true): its
 * node ranges are shifted to the current text, so highlights stay attached to their text until the
 * reparse lands (PLAN.md 3.4 step 2: "the old highlights stay").
 *
 * Value semantics: default-constructed it is empty; copying is O(1) (one atomic increment).
 * Thread safety: every member is const and safe concurrently from any number of threads; tree-
 * sitter trees and queries are safe to read concurrently (the Send and Sync impls of tree-sitter's
 * own Rust binding, lib/binding_rust/lib.rs, MIT). Each query uses its own cursor.
 * Lifetime: the LanguageRegistry it came from must outlive it.
 * Positions are byte offsets of text(); ranges passed in are clamped to text().size().
 */
class SyntaxTree {
public:
    SyntaxTree() noexcept = default;
    // Wraps a tree state (frost::syntax internal).
    explicit SyntaxTree(std::shared_ptr<const detail::TreeState> state) noexcept : m_state(std::move(state)) {}

    // True when there is no tree (syntax off, failed, or before the first parse).
    [[nodiscard]] bool empty() const noexcept { return m_state == nullptr; }
    // The tree's language; None when empty.
    [[nodiscard]] LanguageId language() const noexcept;
    // The snapshot whose coordinates the tree is in (an empty snapshot when empty). O(1).
    [[nodiscard]] text::Snapshot text() const noexcept;
    // text().version().
    [[nodiscard]] std::uint64_t version() const noexcept;
    // The version actually parsed; smaller than version() while a reparse is pending.
    [[nodiscard]] std::uint64_t parsedVersion() const noexcept;
    // version() != parsedVersion().
    [[nodiscard]] bool stale() const noexcept { return version() != parsedVersion(); }
    // True when the tree contains ERROR or MISSING nodes.
    [[nodiscard]] bool hasError() const noexcept;

    /*
     * The highlight spans intersecting `range` (HighlightResult's invariants). Precedence: the
     * innermost capture wins; for captures of one range, the first pattern in highlights.scm wins.
     * Captures named "_..." only feed predicates. Empty with complete = false when empty() or the
     * language's queries are not prepared yet. O(B log n + c log c) for the c captures in range,
     * bounded by `limits`. Precondition: range.begin <= range.end. Budget: B5.
     */
    [[nodiscard]] HighlightResult highlights(text::ByteRange range, const QueryLimits& limits = {}) const noexcept;

    // The outline of the whole document (Outline's invariants); empty with complete = false when
    // empty() or the queries are not prepared. O(c log c). Call from a UserVisible task for big files.
    [[nodiscard]] Outline outline(const QueryLimits& limits = {}) const noexcept;

    // Multiline foldable ranges. Lua, C++, C# and CMake supply queries; other languages return no ranges.
    // Incomplete means no prepared tree or a query limit was reached. O(c log c).
    [[nodiscard]] FoldResult foldRanges(const QueryLimits& limits = {}) const noexcept;

    // Lua comment/string containing `at`, or nullopt. Call only on a current Lua tree;
    // O(tree depth). The returned byte range belongs to text().
    [[nodiscard]] std::optional<text::ByteRange> luaOpaqueAt(ByteOffset at) const noexcept;
    // Earliest enclosing error's start, or `at` when outside an incomplete construct. O(depth).
    [[nodiscard]] ByteOffset luaErrorStartAt(ByteOffset at) const noexcept;
    // The matching block opener for a Lua end/until token, or nullopt. O(tree depth).
    [[nodiscard]] std::optional<ByteOffset> luaCloserOpener(ByteOffset at) const noexcept;

    /*
     * The ranges of the named nodes that contain `selection`, smallest first, strictly growing and
     * ending with the root's range; for extend selection (m1/editor-model). Descends from the root
     * with ts_tree_cursor_goto_first_child_for_byte rather than walking parents. Empty when empty().
     * Precondition: selection.begin <= selection.end. O(depth x fan-out). Budget: B5.
     */
    [[nodiscard]] std::vector<text::ByteRange> enclosingRanges(text::ByteRange selection) const noexcept;

    // An iterative dump of every node (named and anonymous), one per line, indented by depth:
    // `type [start-end] (row,col)-(row,col)` with ranges, `type` without; MISSING and ERROR nodes
    // are marked. For tests. O(nodes).
    [[nodiscard]] std::string dumpForTesting(bool withRanges) const;

    // The shared state (frost::syntax internal).
    [[nodiscard]] const std::shared_ptr<const detail::TreeState>& state() const noexcept { return m_state; }

private:
    std::shared_ptr<const detail::TreeState> m_state;
};

} // namespace frost::syntax
