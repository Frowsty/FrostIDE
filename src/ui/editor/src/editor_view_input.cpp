// SPDX-License-Identifier: Apache-2.0
//
// EditorView (frost/ui/editor/editor_view.hpp), part 3: input events. Keys go through
// EditorKeyRouting, mouse presses and drags select by character, word or line, and
// QInputMethodEvent drives the Document's IME composition.

#include "frost/text/editor/unicode.hpp"
#include "frost/ui/editor/editor_key_routing.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/editor/find_bar.hpp"
#include "frost/ui/editor/go_to_line.hpp"
#include "frost/ui/editor/rhi_renderer.hpp"

#include <QFocusEvent>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QStringList>
#include <QStyleHints>
#include <QToolTip>

#include <algorithm>

namespace frost::ui {

namespace {

namespace commands = text::commands;

// How far a double-click word selection scans each way (a word on a huge line stays cheap).
constexpr int kWordScanLimit = 4096;

[[nodiscard]] std::uint32_t breakBytes(text::LineBreak lineBreak) noexcept {
    switch (lineBreak) {
    case text::LineBreak::None: return 0;
    case text::LineBreak::Lf:
    case text::LineBreak::Cr: return 1;
    case text::LineBreak::CrLf: return 2;
    }
    return 0;
}

} // namespace

// --- events ---

bool EditorView::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::ShortcutOverride: {
        // A window shortcut must not steal a key the editor binds or types.
        auto* key = static_cast<QKeyEvent*>(event);
        if (m_services.keys != nullptr && m_services.keys->wantsKey(*this, *key)) {
            event->accept();
            return true;
        }
        break;
    }
    case QEvent::KeyPress: {
        // Tab and Shift+Tab indent and unindent instead of moving the focus.
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            keyPressEvent(key);
            return true;
        }
        break;
    }
    case QEvent::DevicePixelRatioChange:
        // A DPI change or a move to another monitor: the raster backing store and the QRhi colour
        // buffer follow the new ratio on the next paint, and the QRhi atlas is rebuilt for it.
        viewport()->update();
        break;
    default: break;
    }
    return QAbstractScrollArea::event(event);
}

bool EditorView::viewportEvent(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        // Gutter markers, stripe marks, the inspection widget and squiggles show their messages.
        const auto* help = static_cast<QHelpEvent*>(event);
        const QString tip = toolTipAt(help->pos());
        if (tip.isEmpty()) {
            QToolTip::hideText();
            event->ignore();
        } else {
            QToolTip::showText(help->globalPos(), tip, viewport());
        }
        return true;
    }
    const bool handled = QAbstractScrollArea::viewportEvent(event);
    if (m_rhiViewport && (event->type() == QEvent::Paint || event->type() == QEvent::Resize)) {
        // The view built the frame (Paint) or followed the size (Resize); the QRhiWidget must see
        // the event too, to render that frame and to resize its colour buffer.
        return false;
    }
    return handled;
}

QString EditorView::lineMarkerToolTip(QPointF at) const {
    if (at.x() >= gutterWidth() || m_markers.empty()) {
        return {};
    }
    const LineIndex line = m_editor->text().lineAt(hitAt(at).offset);
    QStringList messages;
    for (auto it = std::ranges::lower_bound(m_markers, line, {}, &LineMarker::line);
         it != m_markers.end() && it->line == line; ++it) {
        messages.push_back(QString::fromStdString(it->message));
    }
    return messages.join(QLatin1Char('\n'));
}

bool EditorView::focusNextPrevChild(bool next) {
    // Inside the find bar or Go to Line, Tab moves between their fields.
    QWidget* focus = focusWidget();
    if (focus != nullptr && focus != this &&
        ((m_findBar != nullptr && m_findBar->isAncestorOf(focus)) ||
         (m_goToLine != nullptr && m_goToLine->isAncestorOf(focus)))) {
        return QAbstractScrollArea::focusNextPrevChild(next);
    }
    return false;
}

void EditorView::keyPressEvent(QKeyEvent* event) {
    beginInput();
    if (m_services.keys != nullptr) {
        if (m_services.keys->handleKey(*this, *event)) {
            event->accept();
        } else {
            event->ignore();
        }
        m_inputStart.reset();
        return;
    }
    // Without key routing the view only types text (tools and tests that drive commands directly).
    const QString text = event->text();
    const bool command = (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) != 0;
    if (!command && !text.isEmpty() && text.at(0).isPrint() && typeText(text.toStdString())) {
        event->accept();
    } else {
        QAbstractScrollArea::keyPressEvent(event);
    }
    m_inputStart.reset();
}

void EditorView::keyReleaseEvent(QKeyEvent* event) {
    // Releases feed the matcher's double-tap detection.
    if (m_services.keys != nullptr && m_services.keys->handleKey(*this, *event)) {
        event->accept();
        return;
    }
    QAbstractScrollArea::keyReleaseEvent(event);
}

void EditorView::focusInEvent(QFocusEvent* event) {
    m_focused = true;
    if (m_services.keys != nullptr) {
        m_services.keys->observe(*event);
        m_services.keys->host().setActive(this);
    } else if (m_services.actionHost != nullptr) {
        m_services.actionHost->setActive(this);
    }
    restartBlink();
    viewport()->update();
    QAbstractScrollArea::focusInEvent(event);
}

void EditorView::focusOutEvent(QFocusEvent* event) {
    m_focused = false;
    if (m_services.keys != nullptr) {
        m_services.keys->observe(*event);
    }
    if (m_composing) {
        // Losing focus ends the composition with what the IME showed.
        if (const std::optional<text::ByteRange> range = m_document->compositionRange()) {
            finishComposition(m_document->text().text(*range));
        }
    }
    m_blink.stop();
    viewport()->update();
    QAbstractScrollArea::focusOutEvent(event);
}

void EditorView::changeEvent(QEvent* event) {
    if (event->type() == QEvent::StyleChange && m_services.appearance == nullptr) {
        followAppearance();
    }
    QAbstractScrollArea::changeEvent(event);
}

// --- IME ---

void EditorView::followDocument(Result<text::ChangeSet> change, std::optional<ByteOffset> caret) {
    if (!change) {
        m_lastError = change.error();
        return;
    }
    Result<text::EditorUpdate> update = m_editor->follow(*change);
    if (!update) {
        m_lastError = update.error();
        return;
    }
    if (caret) {
        m_editor->setState(text::EditorState::caretAt(*caret));
    }
    afterChange(*update);
}

void EditorView::finishComposition(const std::string& commit) {
    if (!m_composing) {
        return;
    }
    m_composing = false;
    const ByteOffset start = m_compositionBase.begin;
    const ByteOffset caret = start + commit.size();
    const text::EditContext context{text::EditKind::Typing, m_compositionSelections, text::SelectionSet::caret(caret)};
    std::optional<text::ByteRange> replace;
    if (!m_compositionBase.empty()) {
        replace = m_compositionBase;
    }
    Result<text::ChangeSet> change = m_document->commitComposition(commit, context, replace);
    if (!change) {
        // The commit was rejected (a TextBuffer limit): drop the preedit rather than stay composing.
        m_lastError = change.error();
        followDocument(m_document->cancelComposition(), start);
        return;
    }
    followDocument(std::move(change), caret);
}

void EditorView::inputMethodEvent(QInputMethodEvent* event) {
    beginInput();
    if (m_services.keys != nullptr) {
        m_services.keys->observe(*event);
    }
    if (isReadOnly()) {
        event->ignore();
        m_inputStart.reset();
        return;
    }
    const std::string commit = event->commitString().toStdString();
    const std::string preedit = event->preeditString().toStdString();
    if (!m_composing && preedit.empty()) {
        // A plain commit (an IME that composes in its own window, or a dead-key result).
        if (!commit.empty()) {
            (void)apply(commands::type(commandInput(), commit));
        }
        event->accept();
        m_inputStart.reset();
        return;
    }
    if (m_composing && (!commit.empty() || preedit.empty())) {
        finishComposition(commit);
    }
    if (!preedit.empty()) {
        if (!m_composing) {
            // A composition replaces the primary selection on commit and shows at its start.
            const text::Selection primary = m_editor->state().primary();
            m_compositionBase = primary.range();
            m_compositionSelections = m_editor->state().selections;
            if (Result<void> begun = m_document->beginComposition(m_compositionBase.begin); !begun) {
                m_lastError = begun.error();
                event->ignore();
                m_inputStart.reset();
                return;
            }
            m_composing = true;
        }
        Result<text::ChangeSet> change = m_document->updateComposition(preedit);
        const std::optional<text::ByteRange> range = m_document->compositionRange();
        followDocument(std::move(change), range ? std::optional<ByteOffset>(range->end) : std::nullopt);
    }
    event->accept();
    m_inputStart.reset();
}

QVariant EditorView::inputMethodQuery(Qt::InputMethodQuery query) const {
    const text::EditorState& state = m_editor->state();
    const text::Selection primary = state.primary();
    switch (query) {
    case Qt::ImEnabled: return !isReadOnly();
    case Qt::ImCursorRectangle: {
        const QRectF caret = caretRect(primary.head, state.caret(state.selections.primaryIndex()).bias);
        return caret.translated(viewport()->pos()).toAlignedRect();
    }
    case Qt::ImFont: return editorFont();
    case Qt::ImHints: return static_cast<int>(Qt::ImhMultiLine);
    case Qt::ImSurroundingText:
    case Qt::ImCursorPosition:
    case Qt::ImAnchorPosition: {
        // The caret's line up to 1000 bytes each side; positions in UTF-16 units of that text.
        const text::Snapshot& snapshot = m_editor->text();
        const Result<text::LineInfo> line = snapshot.lineInfo(snapshot.lineAt(primary.head));
        if (!line) {
            return QVariant();
        }
        const ByteOffset begin = snapshot.clip(
            std::max(line->content.begin, primary.head - std::min<std::uint64_t>(primary.head.value(), 1000)));
        const ByteOffset end = snapshot.clip(std::min(line->content.end, primary.head + 1000));
        const QString before =
            QString::fromStdString(snapshot.text(text::ByteRange{begin, std::max(begin, primary.head)}));
        if (query == Qt::ImSurroundingText) {
            return QString::fromStdString(snapshot.text(text::ByteRange{begin, end}));
        }
        if (query == Qt::ImCursorPosition) {
            return static_cast<int>(before.size());
        }
        const ByteOffset anchor = std::clamp(primary.anchor, begin, end);
        return static_cast<int>(QString::fromStdString(snapshot.text(text::ByteRange{begin, anchor})).size());
    }
    case Qt::ImCurrentSelection: {
        const text::ByteRange range = primary.range();
        if (range.size() > 1000) {
            return QString();
        }
        return QString::fromStdString(m_editor->text().text(range));
    }
    default: return QAbstractScrollArea::inputMethodQuery(query);
    }
}

// --- mouse ---

text::ByteRange EditorView::wordAt(ByteOffset offset) const {
    const text::Snapshot& snapshot = m_editor->text();
    const std::optional<text::DecodedChar> after = text::charAt(snapshot, offset);
    const std::optional<text::DecodedChar> before = text::charBefore(snapshot, offset);
    // The class under the click: the character after it, or the one before at a word's end.
    std::optional<text::CharClass> cls;
    if (after && text::charClass(after->cp) != text::CharClass::LineBreak) {
        cls = text::charClass(after->cp);
        if (*cls != text::CharClass::Word && before && text::charClass(before->cp) == text::CharClass::Word) {
            cls = text::CharClass::Word;
        }
    } else if (before && text::charClass(before->cp) != text::CharClass::LineBreak) {
        cls = text::charClass(before->cp);
    }
    if (!cls) {
        return text::ByteRange{offset, offset};
    }
    ByteOffset begin = offset;
    for (int i = 0; i < kWordScanLimit; ++i) {
        const std::optional<text::DecodedChar> ch = text::charBefore(snapshot, begin);
        if (!ch || text::charClass(ch->cp) != *cls) {
            break;
        }
        begin = begin - ch->bytes;
    }
    ByteOffset end = offset;
    for (int i = 0; i < kWordScanLimit; ++i) {
        const std::optional<text::DecodedChar> ch = text::charAt(snapshot, end);
        if (!ch || text::charClass(ch->cp) != *cls) {
            break;
        }
        end = end + ch->bytes;
    }
    return text::ByteRange{begin, end};
}

text::ByteRange EditorView::lineAt(ByteOffset offset) const {
    const text::Snapshot& snapshot = m_editor->text();
    const Result<text::LineInfo> line = snapshot.lineInfo(snapshot.lineAt(offset));
    if (!line) {
        return text::ByteRange{offset, offset};
    }
    return text::ByteRange{line->content.begin, line->content.end + breakBytes(line->lineBreak)};
}

void EditorView::select(text::Selection selection, text::Bias bias) {
    text::EditorState state;
    state.selections = text::SelectionSet::fromSelections(std::span<const text::Selection>(&selection, 1), 0);
    state.carets.push_back(text::CaretState{bias, std::nullopt});
    setState(std::move(state));
}

void EditorView::dragTo(const text::DisplayHit& hit) {
    text::ByteRange unit{hit.offset, hit.offset};
    if (m_granularity == Granularity::Word) {
        unit = wordAt(hit.offset);
    } else if (m_granularity == Granularity::Line) {
        unit = lineAt(hit.offset);
    }
    // The selection always covers the unit the drag started in; its caret end follows the mouse.
    if (unit.begin < m_dragOrigin.begin) {
        select(text::Selection{m_dragOrigin.end, unit.begin}, hit.bias);
    } else {
        select(text::Selection{m_dragOrigin.begin, std::max(unit.end, m_dragOrigin.end)}, hit.bias);
    }
}

void EditorView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    beginInput();
    if (m_composing) {
        if (const std::optional<text::ByteRange> range = m_document->compositionRange()) {
            finishComposition(m_document->text().text(*range));
        }
    }
    setFocus(Qt::MouseFocusReason);
    const QPointF pos = event->position();
    if (clickInsight(pos)) {
        // The error stripe or the inspection widget took the click.
        m_inputStart.reset();
        event->accept();
        return;
    }
    if (const std::optional<std::size_t> change = lineChangeAt(pos)) {
        // A click on a VCS change stripe opens its popup (the VCS layer listens) instead of selecting.
        m_dragging = false;
        m_inputStart.reset();
        emit lineChangeClicked(static_cast<int>(*change), viewport()->mapToGlobal(pos.toPoint()));
        event->accept();
        return;
    }
    const text::DisplayHit hit = hitAt(pos);
    const auto now = std::chrono::steady_clock::now();
    const auto interval = std::chrono::milliseconds(QGuiApplication::styleHints()->mouseDoubleClickInterval());
    const bool triple = now - m_lastDoubleClick < interval && QLineF(pos, m_lastDoubleClickPos).length() < 6.0;
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    m_dragging = true;
    if (triple || pos.x() < gutterWidth()) {
        // A triple click, or a click in the gutter, selects the whole line (break included).
        m_granularity = Granularity::Line;
        m_dragOrigin = lineAt(hit.offset);
        select(text::Selection{m_dragOrigin.begin, m_dragOrigin.end}, text::Bias::Right);
    } else if (modifiers == (Qt::AltModifier | Qt::ShiftModifier)) {
        // Alt+Shift+click adds a caret (IntelliJ).
        m_dragging = false;
        std::vector<text::Selection> all(m_editor->state().selections.all().begin(),
                                         m_editor->state().selections.all().end());
        all.push_back(text::Selection::caret(hit.offset));
        setState(text::EditorState::from(text::SelectionSet::fromSelections(all, all.size() - 1)));
    } else if ((modifiers & Qt::ShiftModifier) != 0) {
        // Shift+click extends the primary selection from its anchor.
        m_granularity = Granularity::Character;
        const ByteOffset anchor = m_editor->state().primary().anchor;
        m_dragOrigin = text::ByteRange{anchor, anchor};
        select(text::Selection{anchor, hit.offset}, hit.bias);
    } else {
        m_granularity = Granularity::Character;
        m_dragOrigin = text::ByteRange{hit.offset, hit.offset};
        select(text::Selection::caret(hit.offset), hit.bias);
        if (modifiers == Qt::ControlModifier) {
            m_dragging = false; // A navigation click, not the start of a selection.
            emit ctrlClicked(hit.offset);
        }
    }
    m_inputStart.reset();
    event->accept();
}

void EditorView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseDoubleClickEvent(event);
        return;
    }
    const QPointF pos = event->position();
    if (pos.x() < gutterWidth() || insightContains(pos)) {
        mousePressEvent(event);
        return;
    }
    beginInput();
    const text::DisplayHit hit = hitAt(pos);
    m_granularity = Granularity::Word;
    m_dragOrigin = wordAt(hit.offset);
    m_dragging = true;
    m_lastDoubleClick = std::chrono::steady_clock::now();
    m_lastDoubleClickPos = pos;
    select(text::Selection{m_dragOrigin.begin, m_dragOrigin.end}, text::Bias::Right);
    m_inputStart.reset();
    event->accept();
}

void EditorView::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging || (event->buttons() & Qt::LeftButton) == 0) {
        QAbstractScrollArea::mouseMoveEvent(event);
        return;
    }
    dragTo(hitAt(event->position()));
    event->accept();
}

void EditorView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

} // namespace frost::ui
