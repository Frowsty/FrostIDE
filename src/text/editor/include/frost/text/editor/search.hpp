// SPDX-License-Identifier: Apache-2.0
//
// Find and replace in one file (ADR 0025): SearchPattern compiles a query (plain text or an RE2
// regular expression, with case and whole-word options) and finds matches in a Snapshot forwards,
// backwards (over reverse chunk iteration) or all at once; the find/replace commands turn matches
// into selections and edits, replace all as one undo step.
/*
 * Matching modes.
 *   - Plain, case-sensitive: a byte search streamed over the snapshot's chunks, forwards with
 *     Snapshot::chunks and backwards with Snapshot::reversedChunks; the needle may span lines.
 *   - Plain case-insensitive, and regex: RE2 (UTF-8, Unicode case folding; RE2 syntax, so no
 *     backreferences or lookaround). A pattern that cannot match a line break is run line by line
 *     on each line's content, so ^ and $ are the line's ends whatever its break (LF, CR or CRLF). A
 *     pattern that mentions a line break (a literal one, or \n or \r) is run over the whole text,
 *     with ^ and $ also matching at LF ((?m)).
 * Whole word: a match must not have a word character (unicode.hpp's Word class) both just before
 * its first character and as that character, nor both as its last character and just after it.
 * Empty matches (possible only with a regex) are skipped by findNext and findPrevious, and kept by
 * findAll and replace all, so "^" inserts at every line start.
 *
 * Replacement. Plain mode inserts the replacement as is. Regex mode expands $0 or $& (the match),
 * $1..$99 (groups; a missing group is empty), $$ (a dollar sign), and \n, \r, \t and \\.
 *
 * Thread safety: a SearchPattern is an immutable value (the compiled program is shared and RE2 is
 * thread-safe), safe on any thread. Costs: n = text bytes; plain case-sensitive search is O(n) with
 * no allocation beyond a needle-sized window; line mode copies one line at a time; whole-text mode
 * copies the text once per call.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/strong_type.hpp"
#include "frost/text/editor/commands.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/snapshot.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::text {

namespace detail {
struct SearchProgram; // search.cpp: the needle or the compiled RE2 program.
} // namespace detail

// The options of a query.
struct SearchOptions {
    bool caseSensitive = false;
    bool wholeWord = false;
    bool regex = false;

    friend constexpr bool operator==(const SearchOptions&, const SearchOptions&) noexcept = default;
};

// A compiled query (header comment). O(1) to copy.
class SearchPattern {
public:
    /*
     * Compiles `pattern` with `options`. Errors: InvalidArgument for an empty pattern, a pattern
     * that is not valid UTF-8, or a regex RE2 rejects (the message carries RE2's reason).
     */
    [[nodiscard]] static Result<SearchPattern> compile(std::string_view pattern, SearchOptions options = {}) noexcept;

    [[nodiscard]] const std::string& pattern() const noexcept;
    [[nodiscard]] SearchOptions options() const noexcept;

    // The first non-empty match that starts at or after `from` (clipped), or nullopt. O(n).
    [[nodiscard]] std::optional<ByteRange> findNext(const Snapshot& text, ByteOffset from) const noexcept;
    // The last non-empty match that ends at or before `before` (clipped), or nullopt. O(n).
    [[nodiscard]] std::optional<ByteRange> findPrevious(const Snapshot& text, ByteOffset before) const noexcept;
    // Every match in order, non-overlapping, empty matches included. O(n + matches).
    [[nodiscard]] std::vector<ByteRange> findAll(const Snapshot& text) const noexcept;
    // True when `range` (char boundaries of `text`) is exactly a match.
    [[nodiscard]] bool matches(const Snapshot& text, ByteRange range) const noexcept;
    // The text that replaces `match` (a match of this pattern in `text`) by `replacement`.
    [[nodiscard]] std::string replacementFor(const Snapshot& text, ByteRange match,
                                             std::string_view replacement) const noexcept;
    // One edit per match of findAll, each replacing the match by `replacement`, in order.
    [[nodiscard]] std::vector<TextEdit> replacements(const Snapshot& text, std::string_view replacement) const noexcept;

private:
    explicit SearchPattern(std::shared_ptr<const detail::SearchProgram> program) noexcept;

    std::shared_ptr<const detail::SearchProgram> m_program;
};

// What a find or replace command did, beside its CommandResult.
struct FindResult {
    CommandResult command;
    bool found = false;       // A match was selected.
    bool wrapped = false;     // The search went past the end (or start) and started over.
    std::size_t replaced = 0; // Matches replaced.
};

namespace commands {

// Selects the first match after the primary selection (its end), wrapping to the start when
// `wrap`; the result has one selection. Not found: the state is unchanged.
[[nodiscard]] FindResult findNext(const CommandInput& in, const SearchPattern& pattern, bool wrap = true) noexcept;
// Selects the last match before the primary selection (its start), wrapping to the end when `wrap`.
[[nodiscard]] FindResult findPrevious(const CommandInput& in, const SearchPattern& pattern, bool wrap = true) noexcept;
// When the primary selection is a match, replaces it and selects the next match after it;
// otherwise selects the next match (as findNext) and replaces nothing. Kind Other.
[[nodiscard]] FindResult replaceNext(const CommandInput& in, const SearchPattern& pattern, std::string_view replacement,
                                     bool wrap = true) noexcept;
// Replaces every match as one batch, so one undo step undoes them all; the selections follow the
// edits. Kind Other.
[[nodiscard]] FindResult replaceAll(const CommandInput& in, const SearchPattern& pattern,
                                    std::string_view replacement) noexcept;

} // namespace commands

} // namespace frost::text
