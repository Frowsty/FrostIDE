// SPDX-License-Identifier: Apache-2.0
//
// The editor essentials of UX-3 (PLAN.md Appendix C) as pure editor-model commands and queries:
// Split Line, Start New Line Before, overwrite-mode typing, Move Statement Up/Down, Auto-Indent
// Lines, Jump to Matching Brace and Code Block Start/End. They follow commands.hpp's model (a
// CommandInput in, one CommandResult out, each result one undo step) and add ISyntaxRanges, the
// seam through which the UI hands in the syntax tree's node ranges (frost::syntax stays out of the
// text layer).
#pragma once

#include "frost/core/strong_type.hpp"
#include "frost/text/editor/commands.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/snapshot.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace frost::text {

/*
 * The syntax tree's view the structural commands need: the ranges of the named nodes that contain
 * `range`, smallest first and strictly growing, the last one the root's (frost::syntax::SyntaxTree::
 * enclosingRanges has this contract). Implementations answer for the CommandInput's text only and
 * are borrowed for one call; GUI thread. An empty answer means "no syntax here".
 */
class ISyntaxRanges {
public:
    virtual ~ISyntaxRanges() = default;
    [[nodiscard]] virtual std::vector<ByteRange> enclosing(ByteRange range) const = 0;
};

// An inclusive run of buffer lines (0-based).
struct LineBlock {
    std::uint32_t first = 0;
    std::uint32_t last = 0;

    friend constexpr bool operator==(const LineBlock&, const LineBlock&) noexcept = default;
};

// A bracket pair: the offsets of the opening and the closing bracket characters (open < close).
struct BracketPair {
    ByteOffset open;
    ByteOffset close;

    friend constexpr bool operator==(const BracketPair&, const BracketPair&) noexcept = default;
};

// The most bytes the plain bracket scans (no syntax tree, or no node that decides) read in either
// direction before giving up; a pair further apart is reported as not found.
inline constexpr std::uint64_t kBracketScanLimit = 1u << 20;

/*
 * The bracket pair ((), [] or {}) of the bracket right after `caret`, else of the one right before
 * it. With `syntax`, the innermost node that starts with that bracket and ends with its partner
 * (or the reverse) decides, so brackets in strings and comments do not count; without one, or when
 * no node decides, a plain nesting count of that bracket kind over at most kBracketScanLimit bytes.
 * Nullopt when neither neighbour is a bracket or it has no partner. O(depth) or O(scan).
 */
[[nodiscard]] std::optional<BracketPair> matchingBracket(const Snapshot& text, ByteOffset caret,
                                                         const ISyntaxRanges* syntax) noexcept;

/*
 * The innermost {} pair around `caret`: open < caret <= close (a caret right before '{' or right
 * after '}' is outside that block, so repeating Code Block Start/End walks outwards). Nodes of
 * `syntax` that start with '{' and end with '}' are preferred; otherwise a plain nesting count over
 * at most kBracketScanLimit bytes each way. Nullopt at top level. O(depth) or O(scan).
 */
[[nodiscard]] std::optional<BracketPair> enclosingBraceBlock(const Snapshot& text, ByteOffset caret,
                                                             const ISyntaxRanges* syntax) noexcept;

/*
 * The Move Statement plan for `state` (the lines of every selection, as one block): the block
 * grown to the whole statements that start on its first and last lines (a caret on a statement's
 * continuation or closing line takes the statement that contains it), and the neighbour it swaps
 * with, the whole statement above (below) it or one blank line. A statement is the largest node
 * that starts at its line's first non-blank character, grown to an enclosing node ending at the
 * same offset (an Allman brace block joins its `if`). Nullopt when the block is the first (last)
 * statement of its block, the neighbour line holds no statement start (a closing brace), or the
 * text has no syntax (callers then move lines). Lines are searched at most 2048 away. O(lines x
 * depth).
 */
struct StatementMove {
    LineBlock block;
    LineBlock neighbour;
};
[[nodiscard]] std::optional<StatementMove> planStatementMove(const Snapshot& text, const EditorState& state, bool up,
                                                             const ISyntaxRanges& syntax) noexcept;

namespace commands {

/*
 * Ctrl+Enter (IntelliJ Split Line): the line break and indentation of Enter (newline()) at every
 * caret, but the carets stay before the break. Kind Typing, a new undo step.
 */
[[nodiscard]] CommandResult splitLine(const CommandInput& in) noexcept;

/*
 * Ctrl+Alt+Enter (Start New Line Before): an empty line above the line of every selection's head,
 * indented like that line (one more level when the line starts with a closer: '}', ')', ']', or
 * Lua's end, until, else, elseif); the caret goes to the new line's end. Kind Typing, a new step.
 */
[[nodiscard]] CommandResult startNewLineBefore(const CommandInput& in) noexcept;

/*
 * Overwrite-mode typing (Insert toggles it): every caret first takes the grapheme cluster after it
 * unless it is at its line end, then `utf8` replaces the selections as type() does, without the
 * smart keys. Non-empty selections are replaced as in type(). Kind Typing, coalescing like type().
 */
[[nodiscard]] CommandResult overtype(const CommandInput& in, std::string_view utf8) noexcept;

/*
 * Swaps the whole lines of `block` with `neighbour`, which must be directly above or below it
 * (FROST_PRE: adjacent, disjoint, inside the text). The line breaks stay in place (a missing break
 * at the text's end stays at the end); selections on either part move with their lines. Kind
 * Other.
 */
[[nodiscard]] CommandResult swapLineBlocks(const CommandInput& in, LineBlock block, LineBlock neighbour) noexcept;

/*
 * Ctrl+Shift+Up / Down (Move Statement): planStatementMove's swap. Without `syntax` (plain text, no
 * parse yet) it is moveLinesUp / moveLinesDown, as IntelliJ does for unstructured text; with one
 * and no plan, nothing changes. Kind Other.
 */
[[nodiscard]] CommandResult moveStatement(const CommandInput& in, bool up, const ISyntaxRanges* syntax) noexcept;

/*
 * Ctrl+Alt+I (Auto-Indent Lines): re-indents every non-blank line the selections touch, top-down,
 * from the line above it with the editor's bracket rules (the ones Enter uses): one level more
 * after a line that leaves an opener unclosed ('{', '(', '[', and Lua's function, then, do, repeat,
 * else), the opener line's indentation after a line that closes one opened above it, one level
 * less on a line that starts with a closer. Brackets in string literals and after the line comment
 * token are ignored. Only C-family languages (C, C++, C#, HLSL, GLSL, JavaScript, TypeScript, JSON)
 * and Lua are re-indented; other languages (Python and YAML, where indentation is syntax, CMake,
 * plain text) return no edits. Kind Other.
 */
[[nodiscard]] CommandResult autoIndentLines(const CommandInput& in) noexcept;
// Whether autoIndentLines re-indents `language` (nullptr: plain text, no). O(1).
[[nodiscard]] bool autoIndentSupports(const LanguageInfo* language) noexcept;

} // namespace commands

} // namespace frost::text
