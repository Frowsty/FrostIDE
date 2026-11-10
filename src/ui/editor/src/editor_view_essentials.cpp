// SPDX-License-Identifier: Apache-2.0
//
// EditorView (frost/ui/editor/editor_view.hpp), part 5: the UX-3 editor essentials. Viewport
// edge jumps (Page Top/Bottom), Split Line, Start New Line Before, Overwrite mode, Paste without
// Formatting, Paste from History, Find Word at Caret, Jump to Matching Brace, Code Block
// Start/End, Move Statement Up/Down, and Auto-Indent Lines.

#include "frost/text/editor/essentials.hpp"
#include "frost/ui/editor/editor_key_routing.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/editor/find_bar.hpp"

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QMenu>
#include <QMimeData>
#include <QScrollBar>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::ui {

namespace {

namespace commands = text::commands;

class TreeSyntaxRanges final : public text::ISyntaxRanges {
public:
    explicit TreeSyntaxRanges(syntax::SyntaxTree tree) noexcept : m_tree(std::move(tree)) {}

    [[nodiscard]] std::vector<text::ByteRange> enclosing(text::ByteRange range) const override {
        return m_tree.enclosingRanges(range);
    }

private:
    syntax::SyntaxTree m_tree;
};

[[nodiscard]] std::string normaliseLineBreaks(std::string_view text, std::string_view lineBreak) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            out += lineBreak;
        } else if (text[i] == '\n') {
            out += lineBreak;
        } else {
            out += text[i];
        }
    }
    return out;
}

[[nodiscard]] QString menuLabelForClip(std::size_t index, std::string_view clip) {
    std::string collapsed;
    collapsed.reserve(std::min<std::size_t>(clip.size(), 64));
    bool prevSpace = true;
    for (const char ch : clip) {
        const bool isSpace = ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
        if (isSpace) {
            if (!prevSpace) {
                collapsed.push_back(' ');
                prevSpace = true;
            }
        } else {
            collapsed.push_back(ch);
            prevSpace = false;
        }
        if (collapsed.size() >= 60) {
            collapsed += "...";
            break;
        }
    }
    if (collapsed.empty()) {
        collapsed = "(whitespace)";
    }
    return QStringLiteral("%1  %2").arg(index + 1).arg(
        QString::fromUtf8(collapsed.data(), static_cast<qsizetype>(collapsed.size())));
}

} // namespace

void EditorView::setOverwriteMode(bool enabled) {
    if (m_overwrite == enabled) {
        return;
    }
    m_overwrite = enabled;
    viewport()->update();
    emitStatus();
}

ClipboardHistory& EditorView::clipboardHistory() noexcept {
    if (m_services.keys != nullptr) {
        return m_services.keys->host().clipboardHistory();
    }
    if (m_services.actionHost != nullptr) {
        return m_services.actionHost->clipboardHistory();
    }
    return m_ownClipboard;
}

bool EditorView::pasteText(std::string_view utf8) {
    if (isReadOnly()) {
        return false;
    }
    const std::string text = normaliseLineBreaks(utf8, lineBreakText());
    return apply(commands::paste(commandInput(), text));
}

bool EditorView::pasteWithoutFormatting() {
    if (isReadOnly()) {
        return false;
    }
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (mime == nullptr || !mime->hasText()) {
        return false;
    }
    return pasteText(mime->text().toStdString());
}

bool EditorView::showPasteHistory() {
    if (isReadOnly()) {
        return false;
    }
    ClipboardHistory& history = clipboardHistory();
    if (history.entries().empty()) {
        if (const QMimeData* mime = QGuiApplication::clipboard()->mimeData(); mime != nullptr && mime->hasText()) {
            history.record(mime->text().toStdString());
        }
    }
    if (history.entries().empty()) {
        return false;
    }
    delete m_pasteMenu;
    m_pasteMenu = new QMenu(this);
    m_pasteMenu->setObjectName(QStringLiteral("pasteHistoryMenu"));
    const auto& entries = history.entries();
    QAction* firstAction = nullptr;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string entry = entries[i];
        QAction* act = m_pasteMenu->addAction(menuLabelForClip(i, entry));
        act->setData(QString::fromStdString(entry));
        connect(act, &QAction::triggered, this, [this, entry] {
            clipboardHistory().record(entry);
            (void)pasteText(entry);
        });
        if (firstAction == nullptr) {
            firstAction = act;
        }
    }
    if (firstAction != nullptr) {
        m_pasteMenu->setActiveAction(firstAction);
    }
    m_pasteMenu->popup(globalCaretRect(m_editor->state().primary().head).bottomLeft());
    return true;
}

bool EditorView::findWordAtCaret() {
    const text::Snapshot& snapshot = m_editor->text();
    const text::Selection primary = m_editor->state().primary();
    text::ByteRange targetRange = primary.range();
    if (targetRange.empty()) {
        targetRange = wordAt(primary.head);
    }
    if (targetRange.empty() || targetRange.size() > 1000) {
        return false;
    }
    const std::string word = snapshot.text(targetRange);
    if (word.empty() || word.find_first_of("\r\n") != std::string::npos) {
        return false;
    }
    text::SearchOptions opts = m_findBar->options();
    opts.regex = false;
    m_findBar->setOptions(opts);
    m_findBar->setFindText(QString::fromStdString(word));
    m_findOrigin = targetRange.begin;
    if (!findBarVisible()) {
        m_findBar->setReplaceVisible(false);
        m_findBar->show();
        updateOverlayGeometry();
    }
    if (primary.empty()) {
        select(text::Selection{targetRange.begin, targetRange.end}, text::Bias::Right);
    }
    m_matchesVersion = ~std::uint64_t{0};
    const bool found = findNext();
    viewport()->update();
    return found;
}

bool EditorView::moveToPageEdge(bool bottom, bool extend) {
    const text::DisplayMap& map = m_editor->display();
    if (map.rowCount() == 0) {
        return false;
    }
    const std::uint32_t topRow = std::min(firstVisibleRow(), map.rowCount() - 1);
    const auto fullRows =
        static_cast<std::uint32_t>(std::max(1, static_cast<int>(std::floor(viewport()->height() / m_lineHeight))));
    const std::uint32_t bottomRow = std::min(topRow + fullRows - 1, map.rowCount() - 1);
    const std::uint32_t targetRow = bottom ? bottomRow : topRow;

    const text::EditorState& state = m_editor->state();
    const std::size_t primaryIdx = state.selections.primaryIndex();
    const text::Selection primary = state.primary();
    const text::CaretState caret = state.caret(primaryIdx);
    const text::DisplayPoint point = map.toDisplayPoint(primary.head, caret.bias);
    const text::TextWidth wantedX = caret.goalX.value_or(map.xAt(point));
    const text::DisplayHit hit = map.hitTest(text::DisplayRow{targetRow}, wantedX);

    const ByteOffset anchor = extend ? primary.anchor : hit.offset;
    const text::Selection nextSelection{anchor, hit.offset};
    text::EditorState nextState;
    nextState.selections = text::SelectionSet::fromSelections(std::span<const text::Selection>(&nextSelection, 1), 0);
    nextState.carets.push_back(text::CaretState{hit.bias, wantedX});
    setState(std::move(nextState));
    return true;
}

bool EditorView::moveToMatchingBrace() {
    const text::Snapshot& snapshot = m_editor->text();
    const syntax::SyntaxTree tree = m_syntax ? m_syntax->tree() : syntax::SyntaxTree{};
    const bool validTree = !tree.empty() && tree.version() == snapshot.version();
    const TreeSyntaxRanges ranges(tree);
    const ByteOffset caret = m_editor->state().primary().head;
    const std::optional<text::BracketPair> pair = text::matchingBracket(snapshot, caret, validTree ? &ranges : nullptr);
    if (!pair) {
        return false;
    }
    const ByteOffset target = (caret == pair->close || caret == pair->close + 1) ? pair->open : pair->close;
    select(text::Selection::caret(target), text::Bias::Right);
    return true;
}

bool EditorView::moveToCodeBlockEdge(bool end, bool extend) {
    const text::Snapshot& snapshot = m_editor->text();
    const syntax::SyntaxTree tree = m_syntax ? m_syntax->tree() : syntax::SyntaxTree{};
    const bool validTree = !tree.empty() && tree.version() == snapshot.version();
    const TreeSyntaxRanges ranges(tree);
    const text::Selection primary = m_editor->state().primary();
    const std::optional<text::BracketPair> pair =
        text::enclosingBraceBlock(snapshot, primary.head, validTree ? &ranges : nullptr);
    if (!pair) {
        return false;
    }
    const ByteOffset target = end ? pair->close + 1 : pair->open;
    const ByteOffset anchor = extend ? primary.anchor : target;
    select(text::Selection{anchor, target}, text::Bias::Right);
    return true;
}

bool EditorView::performEssential(EditorAction action) {
    const auto moveStmt = [&](bool up) {
        const syntax::SyntaxTree tree = m_syntax ? m_syntax->tree() : syntax::SyntaxTree{};
        const bool validTree = !tree.empty() && tree.version() == m_editor->text().version();
        const TreeSyntaxRanges ranges(tree);
        return apply(commands::moveStatement(commandInput(), up, validTree ? &ranges : nullptr));
    };

    using A = EditorAction;
    switch (action) {
    case A::PageTop: return moveToPageEdge(false, false);
    case A::PageBottom: return moveToPageEdge(true, false);
    case A::PageTopWithSelection: return moveToPageEdge(false, true);
    case A::PageBottomWithSelection: return moveToPageEdge(true, true);
    case A::SplitLine: return apply(commands::splitLine(commandInput()));
    case A::StartNewLineBefore: return apply(commands::startNewLineBefore(commandInput()));
    case A::ToggleOverwrite: setOverwriteMode(!m_overwrite); return true;
    case A::PasteWithoutFormatting: return pasteWithoutFormatting();
    case A::PasteFromHistory: return showPasteHistory();
    case A::FindWordAtCaret: return findWordAtCaret();
    case A::MatchingBrace: return moveToMatchingBrace();
    case A::CodeBlockStart: return moveToCodeBlockEdge(false, false);
    case A::CodeBlockEnd: return moveToCodeBlockEdge(true, false);
    case A::CodeBlockStartWithSelection: return moveToCodeBlockEdge(false, true);
    case A::CodeBlockEndWithSelection: return moveToCodeBlockEdge(true, true);
    case A::MoveStatementUp: return moveStmt(true);
    case A::MoveStatementDown: return moveStmt(false);
    case A::AutoIndentLines: return apply(commands::autoIndentLines(commandInput()));
    default: return false;
    }
}

} // namespace frost::ui
