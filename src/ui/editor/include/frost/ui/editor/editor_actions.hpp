// SPDX-License-Identifier: Apache-2.0
//
// The editor's actions in frost::input's registry (ADR 0016 decision 9; ADR 0037): the action table
// (ids the IntelliJ keymap binds, names, whether they edit), EditorActionHost, which tracks the
// editor the keyboard is in, and registerEditorActions, whose handlers run on that editor.
#pragma once

#include "frost/core/error.hpp"
#include "frost/input/action.hpp"

#include <QPointer>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>

namespace frost::ui {

class EditorView;

// Every editor action EditorView::perform runs.
enum class EditorAction : std::uint8_t {
    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,
    MoveLeftWithSelection,
    MoveRightWithSelection,
    MoveUpWithSelection,
    MoveDownWithSelection,
    WordLeft,
    WordRight,
    WordLeftWithSelection,
    WordRightWithSelection,
    LineStart,
    LineEnd,
    LineStartWithSelection,
    LineEndWithSelection,
    TextStart,
    TextEnd,
    TextStartWithSelection,
    TextEndWithSelection,
    PageUp,
    PageDown,
    PageUpWithSelection,
    PageDownWithSelection,
    ScrollUp,
    ScrollDown,
    Backspace,
    Delete,
    DeleteToWordStart,
    DeleteToWordEnd,
    Enter,
    StartNewLine,
    Tab,
    Unindent,
    Escape,
    SelectAll,
    Copy,
    Cut,
    Paste,
    Undo,
    Redo,
    Duplicate,
    DeleteLine,
    JoinLines,
    MoveLineUp,
    MoveLineDown,
    CommentLine,
    CommentBlock,
    Find,
    Replace,
    FindNext,
    FindPrevious,
    GoToLine,
    NextMember,
    PreviousMember,
    NextError,     // F2 (ADR 0047).
    PreviousError, // Shift+F2.
    Fold,
    Unfold,
    FoldAll,
    UnfoldAll,
    FoldSelection,
    ExtendSelection,
    ShrinkSelection,
    SelectNextOccurrence,
    UnselectOccurrence,
    SelectAllOccurrences,
    ToggleCase,
    ToggleColumnMode,
    // UX-3's editor essentials (frost/text/editor/essentials.hpp).
    PageTop,
    PageBottom,
    PageTopWithSelection,
    PageBottomWithSelection,
    SplitLine,
    StartNewLineBefore,
    ToggleOverwrite,
    PasteWithoutFormatting,
    PasteFromHistory,
    FindWordAtCaret,
    MatchingBrace,
    CodeBlockStart,
    CodeBlockEnd,
    CodeBlockStartWithSelection,
    CodeBlockEndWithSelection,
    MoveStatementUp,
    MoveStatementDown,
    AutoIndentLines,
};

// One row of the action table.
struct EditorActionInfo {
    std::string_view id;   // The frost::input action id the keymap binds ("editor.deleteLine").
    std::string_view name; // Find Action text (English; TODO(M1) wrapped in tr() by the shell).
    EditorAction action;
    bool edits; // Changes the text: disabled in read-only editors (ContextKey::EditorReadOnly).
};

// The table, in EditorAction order. Valid for the program's lifetime. O(1).
[[nodiscard]] std::span<const EditorActionInfo> editorActions() noexcept;
// The row of `action`. O(1).
[[nodiscard]] const EditorActionInfo& editorActionInfo(EditorAction action) noexcept;

/*
 * The clipboard history of Paste from History (Ctrl+Shift+V; feature row 254): the texts copied or
 * cut in the editors, and the system clipboard's text whenever the history is shown, newest first,
 * without duplicates (recording a text again moves it to the front), at most kCapacity entries of
 * at most kMaxBytes each (longer texts are not recorded). In memory only: it does not survive a
 * restart. GUI thread only.
 */
class ClipboardHistory {
public:
    static constexpr std::size_t kCapacity = 32;
    static constexpr std::size_t kMaxBytes = 1u << 20;

    // Records `text` as the newest entry; empty and over-long texts are ignored. O(kCapacity).
    void record(std::string text);
    [[nodiscard]] const std::deque<std::string>& entries() const noexcept { return m_entries; }
    void clear() noexcept { m_entries.clear(); }

private:
    std::deque<std::string> m_entries;
};

/*
 * The editor the editor actions run on: the one the keyboard is in. Views set themselves active on
 * focus-in and on every routed key; a destroyed view is dropped automatically (QPointer). GUI thread
 * only. The shell keeps one host per registry and outlives its views' registrations.
 */
class EditorActionHost {
public:
    [[nodiscard]] EditorView* active() const noexcept;
    void setActive(EditorView* view) noexcept;
    // The clipboard history the host's views share.
    [[nodiscard]] ClipboardHistory& clipboardHistory() noexcept { return m_clipboard; }

private:
    QPointer<EditorView> m_active;
    ClipboardHistory m_clipboard;
};

/*
 * Registers every editorActions() row in `registry` (group "Editor", context EditorFocus, editing
 * actions also need !EditorReadOnly); each handler runs EditorView::perform on host.active(), and
 * does nothing without one. Errors: the registry's add() errors, AlreadyExists when this registry
 * already has an editor action. Precondition: `host` outlives `registry`'s use of the handlers.
 */
[[nodiscard]] Result<void> registerEditorActions(input::ActionRegistry& registry, EditorActionHost& host);
/*
 * As above, but rows whose id is in `skip` are left out: the composition root passes the ids
 * another owner registers with its own handler (the shell's editor.gotoLine, which opens the Go to
 * Line popup). Errors: as above.
 */
[[nodiscard]] Result<void> registerEditorActions(input::ActionRegistry& registry, EditorActionHost& host,
                                                 std::span<const std::string_view> skip);

} // namespace frost::ui
