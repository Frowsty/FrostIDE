// SPDX-License-Identifier: Apache-2.0
//
// The editor action table and its registration in frost::input's ActionRegistry
// (frost/ui/editor/editor_actions.hpp). The ids are the IntelliJ keymap's (keymap_intellij.cpp).

#include "frost/ui/editor/editor_actions.hpp"

#include "frost/ui/editor/editor_view.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace frost::ui {

namespace {

using A = EditorAction;

// clang-format off
constexpr std::array kActions{
    EditorActionInfo{"editor.moveLeft", "Move Caret Left", A::MoveLeft, false},
    EditorActionInfo{"editor.moveRight", "Move Caret Right", A::MoveRight, false},
    EditorActionInfo{"editor.moveUp", "Move Caret Up", A::MoveUp, false},
    EditorActionInfo{"editor.moveDown", "Move Caret Down", A::MoveDown, false},
    EditorActionInfo{"editor.moveLeftWithSelection", "Move Caret Left with Selection", A::MoveLeftWithSelection, false},
    EditorActionInfo{"editor.moveRightWithSelection", "Move Caret Right with Selection", A::MoveRightWithSelection, false},
    EditorActionInfo{"editor.moveUpWithSelection", "Move Caret Up with Selection", A::MoveUpWithSelection, false},
    EditorActionInfo{"editor.moveDownWithSelection", "Move Caret Down with Selection", A::MoveDownWithSelection, false},
    EditorActionInfo{"editor.wordLeft", "Move Caret to Previous Word", A::WordLeft, false},
    EditorActionInfo{"editor.wordRight", "Move Caret to Next Word", A::WordRight, false},
    EditorActionInfo{"editor.wordLeftWithSelection", "Move Caret to Previous Word with Selection", A::WordLeftWithSelection, false},
    EditorActionInfo{"editor.wordRightWithSelection", "Move Caret to Next Word with Selection", A::WordRightWithSelection, false},
    EditorActionInfo{"editor.lineStart", "Move Caret to Line Start", A::LineStart, false},
    EditorActionInfo{"editor.lineEnd", "Move Caret to Line End", A::LineEnd, false},
    EditorActionInfo{"editor.lineStartWithSelection", "Move Caret to Line Start with Selection", A::LineStartWithSelection, false},
    EditorActionInfo{"editor.lineEndWithSelection", "Move Caret to Line End with Selection", A::LineEndWithSelection, false},
    EditorActionInfo{"editor.textStart", "Move Caret to Text Start", A::TextStart, false},
    EditorActionInfo{"editor.textEnd", "Move Caret to Text End", A::TextEnd, false},
    EditorActionInfo{"editor.textStartWithSelection", "Move Caret to Text Start with Selection", A::TextStartWithSelection, false},
    EditorActionInfo{"editor.textEndWithSelection", "Move Caret to Text End with Selection", A::TextEndWithSelection, false},
    EditorActionInfo{"editor.pageUp", "Page Up", A::PageUp, false},
    EditorActionInfo{"editor.pageDown", "Page Down", A::PageDown, false},
    EditorActionInfo{"editor.pageUpWithSelection", "Page Up with Selection", A::PageUpWithSelection, false},
    EditorActionInfo{"editor.pageDownWithSelection", "Page Down with Selection", A::PageDownWithSelection, false},
    EditorActionInfo{"editor.scrollUp", "Scroll Up", A::ScrollUp, false},
    EditorActionInfo{"editor.scrollDown", "Scroll Down", A::ScrollDown, false},
    EditorActionInfo{"editor.backspace", "Backspace", A::Backspace, true},
    EditorActionInfo{"editor.delete", "Delete", A::Delete, true},
    EditorActionInfo{"editor.deleteToWordStart", "Delete to Word Start", A::DeleteToWordStart, true},
    EditorActionInfo{"editor.deleteToWordEnd", "Delete to Word End", A::DeleteToWordEnd, true},
    EditorActionInfo{"editor.enter", "Enter", A::Enter, true},
    EditorActionInfo{"editor.startNewLine", "Start New Line", A::StartNewLine, true},
    EditorActionInfo{"editor.tab", "Tab", A::Tab, true},
    EditorActionInfo{"editor.unindent", "Unindent Line or Selection", A::Unindent, true},
    EditorActionInfo{"editor.escape", "Escape", A::Escape, false},
    EditorActionInfo{"editor.selectAll", "Select All", A::SelectAll, false},
    EditorActionInfo{"editor.copy", "Copy", A::Copy, false},
    EditorActionInfo{"editor.cut", "Cut", A::Cut, true},
    EditorActionInfo{"editor.paste", "Paste", A::Paste, true},
    EditorActionInfo{"editor.undo", "Undo", A::Undo, true},
    EditorActionInfo{"editor.redo", "Redo", A::Redo, true},
    EditorActionInfo{"editor.duplicate", "Duplicate Line or Selection", A::Duplicate, true},
    EditorActionInfo{"editor.deleteLine", "Delete Line", A::DeleteLine, true},
    EditorActionInfo{"editor.joinLines", "Join Lines", A::JoinLines, true},
    EditorActionInfo{"editor.moveLineUp", "Move Line Up", A::MoveLineUp, true},
    EditorActionInfo{"editor.moveLineDown", "Move Line Down", A::MoveLineDown, true},
    EditorActionInfo{"editor.commentLine", "Comment with Line Comment", A::CommentLine, true},
    EditorActionInfo{"editor.commentBlock", "Comment with Block Comment", A::CommentBlock, true},
    EditorActionInfo{"editor.find", "Find", A::Find, false},
    EditorActionInfo{"editor.replace", "Replace", A::Replace, true},
    EditorActionInfo{"editor.findNext", "Find Next Occurrence", A::FindNext, false},
    EditorActionInfo{"editor.findPrevious", "Find Previous Occurrence", A::FindPrevious, false},
    EditorActionInfo{"editor.gotoLine", "Go to Line:Column", A::GoToLine, false},
    EditorActionInfo{"navigate.nextMember", "Next Member", A::NextMember, false},
    EditorActionInfo{"navigate.previousMember", "Previous Member", A::PreviousMember, false},
    EditorActionInfo{"navigate.nextError", "Next Highlighted Error", A::NextError, false},
    EditorActionInfo{"navigate.previousError", "Previous Highlighted Error", A::PreviousError, false},
    EditorActionInfo{"editor.foldCollapse", "Fold", A::Fold, false},
    EditorActionInfo{"editor.foldExpand", "Unfold", A::Unfold, false},
    EditorActionInfo{"editor.foldCollapseAll", "Fold All", A::FoldAll, false},
    EditorActionInfo{"editor.foldExpandAll", "Unfold All", A::UnfoldAll, false},
    EditorActionInfo{"editor.foldSelection", "Fold Selection", A::FoldSelection, false},
};
// clang-format on

consteval bool tableInOrder() {
    for (std::size_t i = 0; i < kActions.size(); ++i) {
        if (static_cast<std::size_t>(kActions[i].action) != i) {
            return false;
        }
    }
    return true;
}
static_assert(tableInOrder(), "kActions is indexed by EditorAction");

} // namespace

std::span<const EditorActionInfo> editorActions() noexcept { return kActions; }

const EditorActionInfo& editorActionInfo(EditorAction action) noexcept {
    return kActions[static_cast<std::size_t>(action)];
}

EditorView* EditorActionHost::active() const noexcept { return m_active.data(); }

void EditorActionHost::setActive(EditorView* view) noexcept { m_active = view; }

Result<void> registerEditorActions(input::ActionRegistry& registry, EditorActionHost& host) {
    return registerEditorActions(registry, host, {});
}

Result<void> registerEditorActions(input::ActionRegistry& registry, EditorActionHost& host,
                                   std::span<const std::string_view> skip) {
    for (const EditorActionInfo& row : kActions) {
        if (std::ranges::find(skip, row.id) != skip.end()) {
            continue;
        }
        input::ActionSpec spec;
        spec.id = std::string(row.id);
        spec.name = std::string(row.name);
        spec.group = "Editor";
        spec.context.all = input::ContextKeys{input::ContextKey::EditorFocus};
        if (row.edits) {
            spec.context.none = input::ContextKeys{input::ContextKey::EditorReadOnly};
        }
        const EditorAction action = row.action;
        EditorActionHost* target = &host;
        spec.handler = [target, action](const input::ActionContext&, const input::InvokeInfo&) {
            if (EditorView* view = target->active()) {
                (void)view->perform(action);
            }
        };
        if (Result<input::ActionId> added = registry.add(std::move(spec)); !added) {
            return std::unexpected(added.error());
        }
    }
    return {};
}

} // namespace frost::ui
