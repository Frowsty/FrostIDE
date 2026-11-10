// SPDX-License-Identifier: Apache-2.0
//
// EditorView (frost/ui/editor/editor_view.hpp): command execution, folding, selection/movement,
// clipboard, find/replace, and Go to Line.

#include "frost/syntax/syntax_tree.hpp"
#include "frost/text/editor/essentials.hpp"
#include "frost/text/editor/unicode.hpp"
#include "frost/ui/editor/find_bar.hpp"
#include "frost/ui/editor/go_to_line.hpp"

#include "editor_view_internal.hpp"
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QScrollBar>

#include <algorithm>
#include <utility>

namespace frost::ui {

namespace {

namespace commands = text::commands;
using text::Movement;

} // namespace

// --- model plumbing ---

text::CommandInput EditorView::commandInput() const { return m_editor->input(); }

bool EditorView::applyUpdate(Result<text::EditorUpdate> update) {
    if (!update) {
        m_lastError = update.error();
        return false;
    }
    afterChange(*update);
    return true;
}

Result<void> EditorView::followExternalChange(const text::ChangeSet& change, bool notifyTextChange) {
    auto update = m_editor->follow(change);
    if (!update)
        return std::unexpected(std::move(update.error()));
    afterChange(*update, notifyTextChange);
    return {};
}

bool EditorView::apply(const text::CommandResult& result) {
    if (result.changesText() && isReadOnly()) {
        return false;
    }
    if (result.changesText() && std::ranges::any_of(result.edits, [&](const auto& edit) {
            return m_editor->display().intersectsFold(edit.range);
        })) {
        std::vector<text::FoldInfo> folds;
        for (const auto& edit : result.edits) {
            auto affected = m_editor->display().foldInfosIntersecting(edit.range);
            folds.insert(folds.end(), affected.begin(), affected.end());
        }
        std::ranges::sort(folds, [](const auto& a, const auto& b) {
            return a.range.begin > b.range.begin || (a.range.begin == b.range.begin && a.range.end < b.range.end);
        });
        folds.erase(std::unique(folds.begin(), folds.end(), [](const auto& a, const auto& b) { return a.id == b.id; }),
                    folds.end());
        for (const auto& fold : folds)
            (void)removeCollapsedFold(fold.id);
    }
    return applyUpdate(m_editor->apply(result));
}

void EditorView::setState(text::EditorState state) {
    m_editor->setState(std::move(state));
    afterChange(text::EditorUpdate{});
}

bool EditorView::setCollapsedFolds(std::vector<text::ByteRange> ranges) {
    const auto& snapshot = m_editor->text();
    for (const auto range : ranges) {
        if (range.begin > range.end || range.end > snapshot.size() || !snapshot.isCharBoundary(range.begin) ||
            !snapshot.isCharBoundary(range.end) || !text::isGraphemeBoundary(snapshot, range.begin) ||
            !text::isGraphemeBoundary(snapshot, range.end)) {
            m_lastError = Error(ErrorCode::InvalidArgument, "fold boundaries must be grapheme boundaries");
            return false;
        }
    }
    auto changed = m_editor->setFolds(std::move(ranges));
    if (!changed) {
        m_lastError = changed.error();
        return false;
    }
    const auto& state = m_editor->state();
    std::vector<text::Selection> selections(state.selections.all().begin(), state.selections.all().end());
    bool moved = false;
    for (auto& selection : selections) {
        for (const auto& fold : collapsedFolds()) {
            if (fold.begin < selection.anchor && selection.anchor < fold.end) {
                selection.anchor = fold.begin;
                moved = true;
            }
            if (fold.begin < selection.head && selection.head < fold.end) {
                selection.head = fold.begin;
                moved = true;
            }
        }
    }
    if (moved) {
        m_editor->setState(
            text::EditorState::from(text::SelectionSet::fromSelections(selections, state.selections.primaryIndex())));
    }
    updateScrollBars();
    ensureCaretVisible();
    viewport()->update();
    emitStatus();
    return true;
}

void EditorView::revealFoldAt(ByteOffset target) {
    const auto folds = m_editor->display().foldInfosContaining(target);
    for (const auto& fold : folds)
        (void)removeCollapsedFold(fold.id);
}

bool EditorView::addCollapsedFold(text::ByteRange range) {
    const auto& snapshot = m_editor->text();
    if (range.begin >= range.end || range.end > snapshot.size() || !snapshot.isCharBoundary(range.begin) ||
        !snapshot.isCharBoundary(range.end) || !text::isGraphemeBoundary(snapshot, range.begin) ||
        !text::isGraphemeBoundary(snapshot, range.end))
        return false;
    auto added = m_editor->addFold(range);
    if (!added) {
        m_lastError = added.error();
        return false;
    }
    const auto& state = m_editor->state();
    std::vector<text::Selection> selections(state.selections.all().begin(), state.selections.all().end());
    bool moved = false;
    for (auto& selection : selections) {
        if (range.begin < selection.anchor && selection.anchor < range.end) {
            selection.anchor = range.begin;
            moved = true;
        }
        if (range.begin < selection.head && selection.head < range.end) {
            selection.head = range.begin;
            moved = true;
        }
    }
    if (moved)
        m_editor->setState(
            text::EditorState::from(text::SelectionSet::fromSelections(selections, state.selections.primaryIndex())));
    updateScrollBars();
    ensureCaretVisible();
    viewport()->update();
    emitStatus();
    return true;
}

bool EditorView::removeCollapsedFold(text::FoldId id) {
    const auto removed = m_editor->removeFold(id);
    if (!removed) {
        m_lastError = removed.error();
        return false;
    }
    updateScrollBars();
    viewport()->update();
    return true;
}

bool EditorView::changeFold(bool collapse, bool all) {
    if (!finishCompositionBeforeFold())
        return false;
    if (all && !collapse)
        return setCollapsedFolds({});
    const auto selection = m_editor->state().primary().range();
    if (collapse && !all && !selection.empty()) {
        if (!addCollapsedFold(selection))
            return false;
        setState(text::EditorState::caretAt(selection.begin));
        return true;
    }
    if (collapse && all) {
        return setCollapsedFolds(m_foldCandidates);
    }
    const auto at = m_editor->state().primary().head;
    if (!collapse) {
        const auto containing = m_editor->display().foldInfosContaining(at);
        if (!containing.empty())
            return removeCollapsedFold(containing.front().id);
        auto found = m_editor->display().foldAtBoundary(at, true);
        if (!found)
            found = m_editor->display().foldAtBoundary(at, false);
        if (!found) {
            const auto line = m_editor->text().lineInfo(m_editor->text().lineAt(at));
            if (line)
                found = m_editor->display().firstFoldStartingIn(line->content);
        }
        return found && removeCollapsedFold(found->id);
    }
    std::optional<text::ByteRange> candidate;
    const auto line = m_editor->text().lineAt(at);
    const auto info = m_editor->text().lineInfo(line);
    if (info) {
        const auto first = std::ranges::lower_bound(m_foldCandidates, info->content.begin, {}, &text::ByteRange::begin);
        if (first != m_foldCandidates.end() && first->begin <= info->content.end)
            candidate = *first;
    }
    if (!candidate)
        candidate = foldCandidateAt(at);
    return candidate && addCollapsedFold(*candidate);
}

std::optional<text::ByteRange> EditorView::foldCandidateAt(ByteOffset offset) const noexcept {
    if (m_foldCandidates.empty())
        return std::nullopt;
    const auto limit = static_cast<std::size_t>(
        std::ranges::upper_bound(m_foldCandidates, offset, {}, &text::ByteRange::begin) - m_foldCandidates.begin());
    const auto found = findCandidate(m_foldCandidateMaxEnd, 1, 0, m_foldCandidates.size(), limit, offset.value());
    return found ? std::optional(m_foldCandidates[*found]) : std::nullopt;
}

bool EditorView::toggleFoldOnLine(LineIndex line) {
    const auto& snapshot = m_editor->text();
    const auto info = snapshot.lineInfo(line);
    if (!info)
        return false;
    if (const auto found = m_editor->display().firstFoldStartingIn(info->content))
        return removeCollapsedFold(found->id);
    const auto candidate = std::ranges::lower_bound(m_foldCandidates, info->content.begin, {}, &text::ByteRange::begin);
    if (candidate == m_foldCandidates.end() || candidate->begin > info->content.end)
        return false;
    return addCollapsedFold(*candidate);
}

bool EditorView::typeText(std::string_view text) {
    if (text.empty() || isReadOnly() || m_composing) {
        return false;
    }
    if (m_overwrite) {
        return apply(commands::overtype(commandInput(), text));
    }
    return apply(commands::type(commandInput(), text));
}

void EditorView::goToLine(int line, int column) {
    const text::Point target{LineIndex{static_cast<std::uint32_t>(std::max(line, 1) - 1)},
                             ByteColumn{static_cast<std::uint32_t>(std::max(column, 1) - 1)}};
    (void)apply(commands::goToLine(commandInput(), target));
}

void EditorView::goToStatusPosition(int line, int column) {
    const auto& snapshot = m_editor->text();
    const auto index =
        std::min<std::uint32_t>(static_cast<std::uint32_t>(std::max(line, 1) - 1), snapshot.lineCount() - 1);
    const auto info = snapshot.lineInfo(LineIndex{index});
    FROST_ASSERT(info);
    const int wanted = std::max(column, 1);
    ByteOffset at = info->content.begin;
    for (int current = 1; current < wanted && at < info->content.end; ++current)
        at = text::nextGraphemeBoundary(snapshot, at);
    // status() switches to Unicode scalar counts on lines past its 4096-byte work limit.
    if (at.value() - info->content.begin.value() > 4096) {
        at = info->content.begin;
        for (int current = 1; current < wanted && at < info->content.end; ++current) {
            auto next = at + 1;
            while (next < info->content.end && snapshot.clip(next) == at)
                next = next + 1;
            at = next;
        }
    }
    setState(text::EditorState::caretAt(at));
}

bool EditorView::move(Movement movement, bool extend) {
    return apply(commands::move(commandInput(), movement, extend));
}

bool EditorView::extendSelection() {
    const text::Snapshot& snapshot = m_editor->text();
    const text::SelectionSet before = m_editor->state().selections;
    if (m_selectionHistoryDocVersion != snapshot.version() ||
        (!m_selectionHistory.empty() && !(before == m_lastExtendedSelection))) {
        m_selectionHistory.clear();
    }
    const syntax::SyntaxTree tree = m_syntax ? m_syntax->tree() : syntax::SyntaxTree{};
    const bool validTree = !tree.empty() && tree.version() == snapshot.version();
    const std::span<const text::Selection> current = before.all();
    std::vector<text::Selection> after;
    after.reserve(current.size());
    bool changed = false;
    for (const text::Selection& sel : current) {
        const text::ByteRange cur = sel.range();
        const std::vector<text::ByteRange> astRanges =
            validTree ? tree.enclosingRanges(cur) : std::vector<text::ByteRange>{};
        const text::ByteRange extended = commands::extendSelectionRange(snapshot, cur, astRanges);
        if (extended != cur) {
            changed = true;
        }
        after.push_back(text::Selection{extended.begin, extended.end});
    }
    if (!changed) {
        return false;
    }
    m_selectionHistory.push_back(before);
    m_selectionHistoryDocVersion = snapshot.version();
    text::EditorState nextState =
        text::EditorState::from(text::SelectionSet::fromSelections(after, before.primaryIndex()));
    m_lastExtendedSelection = nextState.selections;
    m_applyingExtendShrink = true;
    setState(std::move(nextState));
    m_applyingExtendShrink = false;
    return true;
}

bool EditorView::shrinkSelection() {
    const text::Snapshot& snapshot = m_editor->text();
    if (m_selectionHistory.empty() || m_selectionHistoryDocVersion != snapshot.version() ||
        !(m_editor->state().selections == m_lastExtendedSelection)) {
        m_selectionHistory.clear();
        return false;
    }
    text::SelectionSet previous = std::move(m_selectionHistory.back());
    m_selectionHistory.pop_back();
    m_lastExtendedSelection = previous;
    m_applyingExtendShrink = true;
    setState(text::EditorState::from(std::move(previous)));
    m_applyingExtendShrink = false;
    return true;
}

void EditorView::scrollRows(int rows) { verticalScrollBar()->setValue(verticalScrollBar()->value() + rows); }

bool EditorView::perform(EditorAction action) {
    const EditorActionInfo& info = editorActionInfo(action);
    if (info.edits && isReadOnly()) {
        return false;
    }
    if ((action == EditorAction::Backspace || action == EditorAction::Delete) && m_editor->state().primary().empty()) {
        const auto caret = m_editor->state().primary().head;
        if (m_editor->display().hasFoldBoundary(caret, action == EditorAction::Delete)) {
            while (const auto fold = m_editor->display().foldAtBoundary(caret, action == EditorAction::Delete)) {
                if (!removeCollapsedFold(fold->id))
                    return false;
            }
            return true;
        }
    }
    const int page = static_cast<int>(m_editor->config().pageRows);
    class IndentContext final : public text::IIndentContext {
    public:
        explicit IndentContext(syntax::SyntaxTree tree) : m_tree(std::move(tree)) {}
        std::optional<text::ByteRange> opaqueAt(ByteOffset at) const noexcept override {
            return m_tree.luaOpaqueAt(at);
        }
        ByteOffset scanStart(ByteOffset at) const noexcept override { return m_tree.luaErrorStartAt(at); }
        std::optional<ByteOffset> closerOpener(ByteOffset at) const noexcept override {
            return m_tree.luaCloserOpener(at);
        }

    private:
        syntax::SyntaxTree m_tree;
    };
    const auto newline = [&] {
        const syntax::SyntaxTree indentTree = m_syntax ? m_syntax->tree() : syntax::SyntaxTree{};
        const IndentContext indentContext(indentTree);
        text::CommandInput input = commandInput();
        if (!indentTree.empty() && !indentTree.stale() && indentTree.language() == syntax::LanguageId::Lua &&
            indentTree.version() == input.text.version()) {
            input.indentContext = &indentContext;
        }
        return apply(commands::newline(input));
    };
    using A = EditorAction;
    switch (action) {
    case A::MoveLeft: return move(Movement::CharLeft, false);
    case A::MoveRight: return move(Movement::CharRight, false);
    case A::MoveUp: return move(Movement::LineUp, false);
    case A::MoveDown: return move(Movement::LineDown, false);
    case A::MoveLeftWithSelection: return move(Movement::CharLeft, true);
    case A::MoveRightWithSelection: return move(Movement::CharRight, true);
    case A::MoveUpWithSelection:
        return m_columnMode ? apply(commands::addCaretAbove(commandInput())) : move(Movement::LineUp, true);
    case A::MoveDownWithSelection:
        return m_columnMode ? apply(commands::addCaretBelow(commandInput())) : move(Movement::LineDown, true);
    case A::WordLeft: return move(Movement::WordLeft, false);
    case A::WordRight: return move(Movement::WordRight, false);
    case A::WordLeftWithSelection: return move(Movement::WordLeft, true);
    case A::WordRightWithSelection: return move(Movement::WordRight, true);
    case A::LineStart: return move(Movement::LineStart, false);
    case A::LineEnd: return move(Movement::LineEnd, false);
    case A::LineStartWithSelection: return move(Movement::LineStart, true);
    case A::LineEndWithSelection: return move(Movement::LineEnd, true);
    case A::TextStart: return move(Movement::TextStart, false);
    case A::TextEnd: return move(Movement::TextEnd, false);
    case A::TextStartWithSelection: return move(Movement::TextStart, true);
    case A::TextEndWithSelection: return move(Movement::TextEnd, true);
    case A::PageUp: scrollRows(-page); return move(Movement::PageUp, false);
    case A::PageDown: scrollRows(page); return move(Movement::PageDown, false);
    case A::PageUpWithSelection: scrollRows(-page); return move(Movement::PageUp, true);
    case A::PageDownWithSelection: scrollRows(page); return move(Movement::PageDown, true);
    case A::ScrollUp: scrollRows(-1); return true;
    case A::ScrollDown: scrollRows(1); return true;
    case A::Backspace: return apply(commands::backspace(commandInput()));
    case A::Delete: return apply(commands::deleteForward(commandInput()));
    case A::DeleteToWordStart: return apply(commands::deleteWordBackward(commandInput()));
    case A::DeleteToWordEnd: return apply(commands::deleteWordForward(commandInput()));
    case A::Enter: return newline();
    case A::StartNewLine: (void)move(Movement::LineEnd, false); return newline();
    case A::Tab: return apply(commands::tab(commandInput()));
    case A::Unindent: return apply(commands::outdent(commandInput()));
    case A::Escape: escape(); return true;
    case A::SelectAll: return apply(commands::selectAll(commandInput()));
    case A::Copy: copySelection(); return true;
    case A::Cut: cutSelection(); return true;
    case A::Paste: pasteClipboard(); return true;
    case A::Undo: return applyUpdate(m_editor->undo());
    case A::Redo: return applyUpdate(m_editor->redo());
    case A::Duplicate: return apply(commands::duplicate(commandInput()));
    case A::DeleteLine: return apply(commands::deleteLines(commandInput()));
    case A::JoinLines: return apply(commands::joinLines(commandInput()));
    case A::MoveLineUp: return apply(commands::moveLinesUp(commandInput()));
    case A::MoveLineDown: return apply(commands::moveLinesDown(commandInput()));
    case A::CommentLine: return apply(commands::toggleLineComment(commandInput()));
    case A::CommentBlock: return apply(commands::toggleBlockComment(commandInput()));
    case A::Find: openFind(false); return true;
    case A::Replace: openFind(true); return true;
    case A::FindNext: return findNext();
    case A::FindPrevious: return findPrevious();
    case A::GoToLine: openGoToLine(); return true;
    case A::NextMember: return m_outline != nullptr && apply(commands::nextMember(commandInput(), *m_outline, false));
    case A::PreviousMember:
        return m_outline != nullptr && apply(commands::previousMember(commandInput(), *m_outline, false));
    case A::NextError: return gotoNextProblem(true);
    case A::PreviousError: return gotoNextProblem(false);
    case A::Fold: return changeFold(true, false);
    case A::Unfold: return changeFold(false, false);
    case A::FoldAll: return changeFold(true, true);
    case A::UnfoldAll: return changeFold(false, true);
    case A::FoldSelection: return changeFold(true, false);
    case A::ExtendSelection: return extendSelection();
    case A::ShrinkSelection: return shrinkSelection();
    case A::SelectNextOccurrence: return apply(commands::selectNextOccurrence(commandInput()));
    case A::UnselectOccurrence: return apply(commands::unselectOccurrence(commandInput()));
    case A::SelectAllOccurrences: return apply(commands::selectAllOccurrences(commandInput()));
    case A::ToggleCase: return apply(commands::toggleCase(commandInput()));
    case A::ToggleColumnMode: setColumnMode(!m_columnMode); return true;
    case A::PageTop:
    case A::PageBottom:
    case A::PageTopWithSelection:
    case A::PageBottomWithSelection:
    case A::SplitLine:
    case A::StartNewLineBefore:
    case A::ToggleOverwrite:
    case A::PasteWithoutFormatting:
    case A::PasteFromHistory:
    case A::FindWordAtCaret:
    case A::MatchingBrace:
    case A::CodeBlockStart:
    case A::CodeBlockEnd:
    case A::CodeBlockStartWithSelection:
    case A::CodeBlockEndWithSelection:
    case A::MoveStatementUp:
    case A::MoveStatementDown:
    case A::AutoIndentLines: return performEssential(action);
    }
    return false;
}

void EditorView::escape() {
    if (findBarVisible()) {
        closeFind();
        return;
    }
    if (m_goToLine->isVisible()) {
        m_goToLine->hide();
        return;
    }
    if (m_balloon) {
        m_balloon.reset();
        viewport()->update();
        return;
    }
    // IntelliJ's Escape: drop the secondary carets and the selection, keep the primary caret.
    setState(text::EditorState::caretAt(m_editor->state().primary().head));
}

// --- clipboard ---

std::string EditorView::lineBreakText() const {
    const text::Snapshot& snapshot = m_editor->text();
    if (snapshot.lineCount() > 1) {
        if (const Result<text::LineInfo> first = snapshot.lineInfo(LineIndex{0})) {
            return std::string(breakText(first->lineBreak));
        }
    }
    return std::string(breakText(lineBreakFor(m_settings.lineSeparator)));
}

void EditorView::copySelection() {
    const text::Snapshot& snapshot = m_editor->text();
    const std::span<const text::Selection> selections = m_editor->state().selections.all();
    const bool lineCopy =
        std::ranges::all_of(selections, [](const text::Selection& selection) { return selection.empty(); });
    std::string out;
    if (lineCopy) {
        // IntelliJ copies the caret's whole line, break included, when nothing is selected.
        std::vector<std::uint32_t> lines;
        for (const text::Selection& selection : selections) {
            lines.push_back(snapshot.lineAt(selection.head).value());
        }
        if (lines.empty()) {
            lines.push_back(0);
        }
        std::ranges::sort(lines);
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
        for (const std::uint32_t line : lines) {
            const Result<text::LineInfo> info = snapshot.lineInfo(LineIndex{line});
            if (!info) {
                continue;
            }
            out += snapshot.text(info->content);
            out += info->lineBreak == text::LineBreak::None ? lineBreakText() : std::string(breakText(info->lineBreak));
        }
    } else {
        const std::string separator = lineBreakText();
        bool firstPart = true;
        for (const text::Selection& selection : selections) {
            if (selection.empty()) {
                continue;
            }
            if (!firstPart) {
                out += separator;
            }
            out += snapshot.text(selection.range());
            firstPart = false;
        }
    }
    clipboardHistory().record(out);
    auto* mime = new QMimeData();
    mime->setText(QString::fromStdString(out));
    if (lineCopy) {
        mime->setData(QString::fromLatin1(kLineCopyMime), QByteArray("1"));
    }
    QGuiApplication::clipboard()->setMimeData(mime);
}

void EditorView::cutSelection() {
    const bool lineCut = !hasSelection();
    copySelection();
    if (lineCut) {
        (void)apply(commands::deleteLines(commandInput()));
        return;
    }
    // Deleting the selection, as its own undo step (a cut never coalesces with typing).
    text::CommandResult result = commands::backspace(commandInput());
    result.kind = text::EditKind::Other;
    result.breakCoalescing = true;
    (void)apply(result);
}

void EditorView::pasteClipboard() {
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (mime == nullptr || !mime->hasText()) {
        return;
    }
    const std::string text = normaliseBreaks(utf8(mime->text()), lineBreakText());
    if (mime->hasFormat(QString::fromLatin1(kLineCopyMime)) && !hasSelection()) {
        (void)apply(commands::pasteLines(commandInput(), text));
    } else {
        (void)apply(commands::paste(commandInput(), text));
    }
}

// --- find / replace ---

bool EditorView::findBarVisible() const noexcept { return m_findBar != nullptr && !m_findBar->isHidden(); }

void EditorView::openFind(bool replace) {
    const text::Selection primary = m_editor->state().primary();
    if (!findBarVisible()) {
        m_findOrigin = primary.range().begin;
    }
    if (!primary.empty() && primary.range().size() <= 1000) {
        const std::string selected = m_editor->text().text(primary.range());
        if (selected.find_first_of("\r\n") == std::string::npos) {
            m_findBar->setFindText(QString::fromStdString(selected));
            m_findOrigin = primary.range().begin;
        }
    }
    m_findBar->setReplaceVisible(replace);
    m_findBar->show();
    updateOverlayGeometry();
    m_findBar->focusFind();
    m_matchesVersion = ~std::uint64_t{0};
    refreshMatchLabel();
    viewport()->update();
}

void EditorView::closeFind() {
    m_findBar->hide();
    updateOverlayGeometry();
    m_matches.clear();
    m_matchesVersion = ~std::uint64_t{0};
    setFocus(Qt::OtherFocusReason);
    viewport()->update();
}

std::optional<text::SearchPattern> EditorView::currentPattern() const {
    const QString query = m_findBar->findText();
    if (query.isEmpty()) {
        return std::nullopt;
    }
    Result<text::SearchPattern> pattern = text::SearchPattern::compile(utf8(query), m_findBar->options());
    if (!pattern) {
        return std::nullopt;
    }
    return std::move(*pattern);
}

void EditorView::onQueryChanged() {
    m_matchesVersion = ~std::uint64_t{0};
    if (const std::optional<text::SearchPattern> pattern = currentPattern()) {
        // Incremental search: the first match at or after where the search started.
        text::CommandInput input = commandInput();
        input.state = text::EditorState::caretAt(m_findOrigin);
        const text::FindResult result = commands::findNext(input, *pattern);
        if (result.found) {
            (void)apply(result.command);
        }
    }
    refreshMatchLabel();
    viewport()->update();
}

const std::vector<text::ByteRange>& EditorView::searchMatches() {
    if (!findBarVisible()) {
        m_matches.clear();
        return m_matches;
    }
    const std::uint64_t version = m_editor->text().version();
    const QString query = m_findBar->findText();
    const text::SearchOptions options = m_findBar->options();
    if (version == m_matchesVersion && query == m_matchesQuery && options == m_matchesOptions) {
        return m_matches;
    }
    m_matches.clear();
    if (const std::optional<text::SearchPattern> pattern = currentPattern()) {
        for (const text::ByteRange& match : pattern->findAll(m_editor->text())) {
            if (!match.empty()) {
                m_matches.push_back(match);
            }
        }
    }
    m_matchesVersion = version;
    m_matchesQuery = query;
    m_matchesOptions = options;
    return m_matches;
}

void EditorView::refreshMatchLabel() {
    if (!findBarVisible()) {
        return;
    }
    const QString query = m_findBar->findText();
    if (!query.isEmpty() && !text::SearchPattern::compile(utf8(query), m_findBar->options())) {
        m_findBar->showError(QStringLiteral("Bad pattern"));
        return;
    }
    const std::vector<text::ByteRange>& matches = searchMatches();
    const text::ByteRange selected = m_editor->state().primary().range();
    const auto it = std::ranges::lower_bound(matches, selected.begin, {}, &text::ByteRange::begin);
    const std::size_t current =
        (it != matches.end() && *it == selected) ? static_cast<std::size_t>(it - matches.begin()) + 1 : 0;
    m_findBar->showMatches(current, matches.size());
}

bool EditorView::findNext() {
    const std::optional<text::SearchPattern> pattern = currentPattern();
    if (!pattern) {
        return false;
    }
    const text::FindResult result = commands::findNext(commandInput(), *pattern);
    if (result.found) {
        (void)apply(result.command);
    }
    refreshMatchLabel();
    return result.found;
}

bool EditorView::findPrevious() {
    const std::optional<text::SearchPattern> pattern = currentPattern();
    if (!pattern) {
        return false;
    }
    const text::FindResult result = commands::findPrevious(commandInput(), *pattern);
    if (result.found) {
        (void)apply(result.command);
    }
    refreshMatchLabel();
    return result.found;
}

bool EditorView::replaceNext() {
    const std::optional<text::SearchPattern> pattern = currentPattern();
    if (!pattern || isReadOnly()) {
        return false;
    }
    const text::FindResult result = commands::replaceNext(commandInput(), *pattern, utf8(m_findBar->replaceText()));
    const bool applied = apply(result.command);
    refreshMatchLabel();
    return applied && (result.found || result.replaced > 0);
}

std::size_t EditorView::replaceAll() {
    const std::optional<text::SearchPattern> pattern = currentPattern();
    if (!pattern || isReadOnly()) {
        return 0;
    }
    const text::FindResult result = commands::replaceAll(commandInput(), *pattern, utf8(m_findBar->replaceText()));
    if (!apply(result.command)) {
        return 0;
    }
    refreshMatchLabel();
    return result.replaced;
}

// --- Go to Line ---

void EditorView::openGoToLine() {
    const EditorViewStatus now = status();
    m_goToLine->open(QStringLiteral("%1:%2").arg(now.line).arg(now.column));
    updateOverlayGeometry();
}

void EditorView::updateOverlayGeometry() {
    if (findBarVisible()) {
        const int height = m_findBar->sizeHint().height();
        setViewportMargins(0, height, 0, 0);
        m_findBar->setGeometry(0, 0, width(), height);
    } else {
        setViewportMargins(0, 0, 0, 0);
    }
    if (m_goToLine != nullptr && !m_goToLine->isHidden()) {
        const QSize size = m_goToLine->sizeHint();
        const int x = std::max(0, (width() - size.width()) / 2);
        m_goToLine->setGeometry(x, viewport()->y() + 8, size.width(), size.height());
        m_goToLine->raise();
    }
}

} // namespace frost::ui
