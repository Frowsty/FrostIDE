// SPDX-License-Identifier: Apache-2.0
//
// The editor model's command layer (ADR 0025): editor commands as pure functions from a snapshot,
// the editor state and the settings to one batch of edits plus the state after it. Typing and smart
// keys, deletion, caret movement and selection, Go to Line, next/previous member and the line
// operations live here; find/replace is in search.hpp. frost::text::Editor (editor.hpp) applies the
// results to a Document, so each command becomes one undo step.
/*
 * The command model. A command reads a CommandInput (the text, the selections with their per-caret
 * state, the settings, the language and optionally the display map) and returns a CommandResult:
 *   - edits: one batch for Document::edit, sorted and non-overlapping in the input text's
 *     coordinates (TextBuffer::applyEdits' rules); empty for a pure caret or selection change;
 *   - state: the selections and caret states after the edits, in the new text's coordinates;
 *   - kind: the EditKind that drives the undo tree's coalescing (Typing, DeleteBackward and
 *     DeleteForward coalesce into runs; everything else is its own step);
 *   - breakCoalescing: seal the open undo step first, so that a new word, a line break or a
 *     replaced selection starts a new step (typing "hello world" is two steps, "hello " and
 *     "world").
 * Commands never fail: a command that does not apply returns the input state and no edits.
 *
 * Multi-caret. Every command acts on every selection of the set (a line command on the union of
 * the lines they touch); the commands that create carets (add caret, column mode, select
 * occurrences) are M15 work and do not exist yet. The primary selection survives merges.
 *
 * Caret state. A caret's goal x (the x it keeps while moving vertically) and its bias (which row a
 * caret at a soft wrap belongs to) travel beside its selection in EditorState::carets. Vertical
 * moves keep and return the goal x; every other command clears it.
 *
 * Thread safety: the functions are pure; a CommandInput may be read concurrently, but its
 * DisplayMap is not thread-safe, so commands that read it run on the map's owner thread.
 * Allocation failure terminates (noexcept), as in all of frost::text.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/strong_type.hpp"
#include "frost/text/anchor.hpp"
#include "frost/text/display_map.hpp"
#include "frost/text/document.hpp"
#include "frost/text/editor/language.hpp"
#include "frost/text/editor/outline.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/selection.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_shaper.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::text {

// A caret's state beyond its selection (header comment). A plain value.
struct CaretState {
    // At a soft wrap: Left is the end of the upper row, Right the start of the lower one.
    Bias bias = Bias::Right;
    // The x vertical moves aim for; none until the first vertical move.
    std::optional<TextWidth> goalX;

    friend constexpr bool operator==(const CaretState&, const CaretState&) noexcept = default;
};

// The selections of an editor and the state of each caret. `carets` is parallel to
// selections.all(); missing entries mean the default CaretState. O(1) to copy the selections,
// O(carets) for the vector.
struct EditorState {
    SelectionSet selections;
    std::vector<CaretState> carets;

    // The state of selection `index` (the default when `carets` has no entry for it).
    [[nodiscard]] CaretState caret(std::size_t index) const noexcept {
        return index < carets.size() ? carets[index] : CaretState{};
    }
    // The primary selection, or a caret at 0 when there are no selections.
    [[nodiscard]] Selection primary() const noexcept;
    // One caret at `at`.
    [[nodiscard]] static EditorState caretAt(ByteOffset at) noexcept;
    // `selections` with default caret states.
    [[nodiscard]] static EditorState from(SelectionSet selections) noexcept;

    friend bool operator==(const EditorState& a, const EditorState& b) noexcept;
};

// The editing settings the commands follow (EditorConfig-style indentation, smart keys).
struct EditorSettings {
    // Columns per tab stop, >= 1 (values below 1 count as 1).
    std::uint32_t tabSize = 4;
    // Columns per indent level, >= 1.
    std::uint32_t indentSize = 4;
    // Indent with spaces (true) or tabs.
    bool insertSpaces = true;
    // Auto-close brackets and quotes, overtype their closers and delete empty pairs.
    bool autoClosePairs = true;
    // Home toggles between the first non-space character and column 0.
    bool smartHome = true;
    // The line break Enter inserts in a text that has none yet.
    LineBreak defaultLineBreak = LineBreak::Lf;

    friend constexpr bool operator==(const EditorSettings&, const EditorSettings&) noexcept = default;
};

// Optional parsed lexical context for indentation. GUI thread; borrowed for one command.
class IIndentContext {
public:
    virtual ~IIndentContext() = default;
    // Comment or string containing byte `at`, or nullopt for code. O(tree depth).
    [[nodiscard]] virtual std::optional<ByteRange> opaqueAt(ByteOffset at) const noexcept = 0;
    // Earliest byte needed to recover lexical state around an incomplete construct.
    [[nodiscard]] virtual ByteOffset scanStart(ByteOffset at) const noexcept = 0;
    // Matched block opener for an end/until token, when the parse can identify it.
    [[nodiscard]] virtual std::optional<ByteOffset> closerOpener(ByteOffset at) const noexcept = 0;
};

// Everything a command reads. `language` nullptr means plain text; `display`, when given, must
// reflect `text` (display->version() == text.version()) and is used for vertical movement, which
// otherwise moves by buffer lines. `pageRows` is the row count of PageUp and PageDown (>= 1).
struct CommandInput {
    Snapshot text;
    EditorState state;
    EditorSettings settings;
    const LanguageInfo* language = nullptr;
    const DisplayMap* display = nullptr;
    std::uint32_t pageRows = 20;
    const IIndentContext* indentContext = nullptr;
};

// One replacement of a batch, owning its text.
struct TextEdit {
    ByteRange range;
    std::string text;

    friend bool operator==(const TextEdit&, const TextEdit&) noexcept = default;
};

// What a command produces (header comment).
struct CommandResult {
    std::vector<TextEdit> edits;
    EditorState state;
    EditKind kind = EditKind::Other;
    bool breakCoalescing = false;

    // True when the result edits the text.
    [[nodiscard]] bool changesText() const noexcept { return !edits.empty(); }
};

// Caret movements; each command can move the caret alone or extend the selection.
enum class Movement : std::uint8_t {
    CharLeft,  // One grapheme cluster back (a non-empty selection collapses to its start instead).
    CharRight, // One grapheme cluster forward (a non-empty selection collapses to its end).
    WordLeft,  // previousWordBoundary (unicode.hpp).
    WordRight, // nextWordBoundary.
    LineStart, // Home: smart home (EditorSettings::smartHome) or column 0.
    LineEnd,   // End: the end of the line's content.
    LineUp,    // One display row up (DisplayMap::moveVertically), keeping the goal x.
    LineDown,
    PageUp, // pageRows display rows.
    PageDown,
    TextStart,
    TextEnd,
};

/*
 * Parses Go to Line input: "line" or "line:column" (also "line,column"), both 1-based, surrounding
 * spaces allowed; returns the 0-based point (the column in bytes). Errors: InvalidArgument for
 * anything else, a zero line or column, or numbers beyond 32 bits.
 */
[[nodiscard]] Result<Point> parseGoToLine(std::string_view input) noexcept;

namespace commands {

// Typing and smart keys.

/*
 * Types `utf8` at every selection, replacing it. A single character also gets the smart keys
 * (settings.autoClosePairs): an opening bracket or quote is closed when the caret is followed by
 * whitespace, a closer or the line end (quotes also need no word character before the caret and an
 * even count of that quote on the line); with a non-empty selection it surrounds the selection
 * instead; a closer that is already the next character is overtyped (brackets when the line has no
 * more openers than closers of that kind, quotes when it has an even count). Kind Typing; a new
 * undo step starts at a line break or at a non-space typed after a space or a line start.
 */
[[nodiscard]] CommandResult type(const CommandInput& in, std::string_view utf8) noexcept;
// Inserts `utf8` at every selection as one Paste step: no smart keys, never coalesced.
[[nodiscard]] CommandResult paste(const CommandInput& in, std::string_view utf8) noexcept;
/*
 * Pastes whole lines (text copied or cut with no selection, which ends with a line break): `utf8`
 * is inserted at the start of the line of every selection's start, once per line, and the
 * selections keep their text, moving down with it. IntelliJ's paste of a line copy. Kind Paste,
 * never coalesced.
 */
[[nodiscard]] CommandResult pasteLines(const CommandInput& in, std::string_view utf8) noexcept;
/*
 * Enter: a line break (the current line's, else the text's first, else settings.defaultLineBreak)
 * plus the current line's indentation (only up to the caret when it is inside the indentation).
 * After '{' one more indent level; between a bracket pair ("{|}", "(|)", "[|]") the closer moves to
 * its own line at the original indentation. Kind Typing, always a new undo step.
 */
[[nodiscard]] CommandResult newline(const CommandInput& in) noexcept;
// Tab: carets insert one indent level (spaces to the next indent stop, or a tab); a non-empty
// selection indents every line it touches that is not empty. Kind Typing for carets only, else
// Other.
[[nodiscard]] CommandResult tab(const CommandInput& in) noexcept;
// Shift+Tab: removes one indent level (a tab, or up to indentSize spaces) from every line the
// selections touch. Kind Other.
[[nodiscard]] CommandResult outdent(const CommandInput& in) noexcept;
/*
 * Backspace: a non-empty selection is deleted; a caret deletes the grapheme cluster before it, or
 * back to the previous indent stop when only spaces precede it on its line, or an empty bracket or
 * quote pair around it (settings.autoClosePairs). Kind DeleteBackward.
 */
[[nodiscard]] CommandResult backspace(const CommandInput& in) noexcept;
// Delete: a non-empty selection is deleted; a caret deletes the grapheme cluster after it. Kind
// DeleteForward.
[[nodiscard]] CommandResult deleteForward(const CommandInput& in) noexcept;
// Ctrl+Backspace: deletes back to previousWordBoundary (or the selection). Kind DeleteBackward.
[[nodiscard]] CommandResult deleteWordBackward(const CommandInput& in) noexcept;
// Ctrl+Delete: deletes forward to nextWordBoundary (or the selection). Kind DeleteForward.
[[nodiscard]] CommandResult deleteWordForward(const CommandInput& in) noexcept;

// Movement and selection.

// Moves every caret by `movement`; with `extend` the selection's anchor stays and the head moves.
[[nodiscard]] CommandResult move(const CommandInput& in, Movement movement, bool extend) noexcept;
// Selects the whole text (the caret at the end).
[[nodiscard]] CommandResult selectAll(const CommandInput& in) noexcept;
// One caret at `target` (0-based; a line past the end goes to the last line, a column past the line
// end to its end, a column inside a character to its start).
[[nodiscard]] CommandResult goToLine(const CommandInput& in, Point target) noexcept;
// Moves every caret to the begin of the first member that starts after it (no move when none).
[[nodiscard]] CommandResult nextMember(const CommandInput& in, const IOutlineProvider& outline, bool extend) noexcept;
// Moves every caret to the begin of the last member that starts before it (no move when none).
[[nodiscard]] CommandResult previousMember(const CommandInput& in, const IOutlineProvider& outline,
                                           bool extend) noexcept;

// Line operations. Each is one undo step (kind Other).

// Ctrl+D: a non-empty selection is copied after itself and the copy selected; a caret's line is
// copied below it and the caret moves to the copy.
[[nodiscard]] CommandResult duplicate(const CommandInput& in) noexcept;
// Ctrl+Y: deletes every line the selections touch (with its line break); the carets keep their
// column on the line that takes the deleted lines' place, and carets that land on the same line
// merge into one (the primary's column wins, else the first's).
[[nodiscard]] CommandResult deleteLines(const CommandInput& in) noexcept;
// Alt+Shift+Up / Down: swaps the lines the selections touch with the line above (below); the
// selections move with them. Blocks already at the first (last) line stay.
[[nodiscard]] CommandResult moveLinesUp(const CommandInput& in) noexcept;
[[nodiscard]] CommandResult moveLinesDown(const CommandInput& in) noexcept;
// Ctrl+Shift+J: joins the lines a selection touches (a caret's line with the next), replacing each
// line break and the whitespace around it with one space, or nothing next to a bracket or an empty
// side. A caret goes to the first join point.
[[nodiscard]] CommandResult joinLines(const CommandInput& in) noexcept;
/*
 * Ctrl+/: when every non-blank line the selections touch starts with the language's line comment
 * (after its indentation) the comments are removed (with one following space), otherwise
 * "token " is inserted at the smallest indentation of those lines. A caret moves down one line
 * afterwards, as in IntelliJ. Languages without a line comment wrap each line in their block
 * tokens; plain text has no comments.
 */
[[nodiscard]] CommandResult toggleLineComment(const CommandInput& in) noexcept;
/*
 * Ctrl+Shift+/: a non-empty selection is wrapped in the language's block tokens, or unwrapped when
 * it starts and ends with them or is surrounded by them; a caret inside a block comment on its line
 * unwraps it, otherwise an empty comment is inserted with the caret inside. Languages without block
 * tokens toggle line comments instead.
 */
[[nodiscard]] CommandResult toggleBlockComment(const CommandInput& in) noexcept;

} // namespace commands

} // namespace frost::text
