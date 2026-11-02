// SPDX-License-Identifier: Apache-2.0
//
// EditorChangeMarkers (frost/ui/vcs/change_markers.hpp): the base, the debounced diff, the markers,
// the change popup and Rollback Lines.

#include "frost/ui/vcs/change_markers.hpp"

#include "frost/text/editor/commands.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/shell/popup.hpp"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace frost::ui {
namespace {

// The change popup: a Popup that opens at a point.
class ChangePopup final : public Popup {
public:
    explicit ChangePopup(QWidget* owner) : Popup(owner) {}
    void showAt(QPoint globalPos) { openAt(globalPos); }
};

ChangeStripeKind stripeKind(vcs::LineChangeKind kind) noexcept {
    switch (kind) {
    case vcs::LineChangeKind::Added: return ChangeStripeKind::Added;
    case vcs::LineChangeKind::Deleted: return ChangeStripeKind::Deleted;
    case vcs::LineChangeKind::Modified: return ChangeStripeKind::Modified;
    }
    return ChangeStripeKind::Modified;
}

} // namespace

EditorChangeMarkers::EditorChangeMarkers(EditorView& view, BaseProvider provider)
    : QObject(&view), m_view(&view), m_provider(std::move(provider)) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(300);
    connect(&m_debounce, &QTimer::timeout, this, &EditorChangeMarkers::recompute);
    connect(&view, &EditorView::textChanged, this, [this] { m_debounce.start(); });
    connect(&view, &EditorView::lineChangeClicked, this,
            [this](int index, QPoint globalPos) { showChangePopup(static_cast<std::size_t>(index), globalPos); });
    refreshBase();
}

EditorChangeMarkers::~EditorChangeMarkers() {
    if (m_popup) {
        m_popup->deleteLater();
    }
}

void EditorChangeMarkers::setDebounce(std::chrono::milliseconds debounce) {
    m_debounce.setInterval(static_cast<int>(debounce.count()));
}

void EditorChangeMarkers::refreshBase() {
    if (!m_view || !m_provider || m_view->path().empty()) {
        return;
    }
    const std::uint64_t request = ++m_request;
    m_basePending = true;
    const QPointer<EditorChangeMarkers> self(this);
    m_provider(m_view->path(), [self, request](Result<std::optional<std::string>> base) {
        if (self) {
            self->onBase(std::move(base), request);
        }
    });
}

void EditorChangeMarkers::onBase(Result<std::optional<std::string>> base, std::uint64_t request) {
    if (request != m_request) {
        return; // A newer read is on its way.
    }
    m_basePending = false;
    m_base.reset();
    if (base && base->has_value()) {
        if (auto sequence = diff::LineSequence::fromBytes(std::move(**base), diff::LineSplit::Lsp)) {
            m_base = std::move(*sequence);
        }
    }
    recompute();
}

void EditorChangeMarkers::recompute() {
    ++m_diffGeneration;
    m_debounce.stop();
    m_changes.clear();
    if (m_view && m_base) {
        if (auto current = diff::LineSequence::fromSnapshot(m_view->editor().text(), diff::LineSplit::Lsp)) {
            m_current = std::move(*current);
            if (auto changes = vcs::computeLineChanges(*m_base, m_current)) {
                m_changes = std::move(*changes);
            }
        }
    }
    if (m_view) {
        std::vector<LineChangeMarker> markers;
        markers.reserve(m_changes.size());
        for (const vcs::LineChange& change : m_changes) {
            markers.push_back(LineChangeMarker{change.current.begin, change.current.size(), stripeKind(change.kind)});
        }
        m_view->setLineChanges(std::move(markers));
    }
    emit changesUpdated();
}

std::string EditorChangeMarkers::baseText(std::size_t index) const {
    if (!m_base || index >= m_changes.size()) {
        return {};
    }
    return std::string(vcs::baseText(*m_base, m_changes[index]));
}

bool EditorChangeMarkers::rollback(std::size_t index) {
    if (!m_view || !m_base || m_view->isReadOnly()) {
        return false;
    }
    if (m_basePending || m_debounce.isActive()) {
        return false; // A stale index may name a different change after recomputing.
    }
    if (index >= m_changes.size()) {
        return false;
    }
    const vcs::LineRollback edit = vcs::rollbackEdit(*m_base, m_current, m_changes[index]);
    text::CommandResult result;
    result.edits.push_back(text::TextEdit{edit.range, edit.text});
    result.state = text::EditorState::caretAt(edit.range.begin);
    result.breakCoalescing = true;
    if (!m_view->apply(result)) {
        return false;
    }
    recompute();
    return true;
}

void EditorChangeMarkers::showChangePopup(std::size_t index, QPoint globalPos) {
    if (!m_view || index >= m_changes.size()) {
        return;
    }
    if (m_popup) {
        m_popup->closePopup(Popup::CloseReason::Dismissed);
    }
    const vcs::LineChange& change = m_changes[index];
    auto* popup = new ChangePopup(m_view);
    auto* layout = new QVBoxLayout(popup);
    QString title;
    switch (change.kind) {
    case vcs::LineChangeKind::Added:
        title = tr("%n line(s) added", nullptr, static_cast<int>(change.current.size()));
        break;
    case vcs::LineChangeKind::Deleted:
        title = tr("%n line(s) deleted", nullptr, static_cast<int>(change.base.size()));
        break;
    case vcs::LineChangeKind::Modified:
        title = tr("%n line(s) changed", nullptr, static_cast<int>(change.current.size()));
        break;
    }
    layout->addWidget(new QLabel(title, popup));
    const std::string old = baseText(index);
    if (!old.empty()) {
        auto* text = new QPlainTextEdit(popup);
        text->setObjectName(QStringLiteral("changePopupBaseText"));
        text->setReadOnly(true);
        text->setLineWrapMode(QPlainTextEdit::NoWrap);
        text->setFont(m_view->editorFont());
        QString shown = QString::fromStdString(old);
        if (shown.endsWith(QLatin1Char('\n'))) {
            shown.chop(1);
        }
        text->setPlainText(shown);
        const int lines = std::min(12, static_cast<int>(change.base.size()));
        text->setFixedHeight(text->fontMetrics().lineSpacing() * (lines + 1) + 8);
        layout->addWidget(text);
    }
    auto* row = new QHBoxLayout;
    auto* rollbackButton = new QPushButton(tr("Rollback Lines"), popup);
    rollbackButton->setEnabled(!m_view->isReadOnly() && !m_basePending && !m_debounce.isActive());
    row->addStretch(1);
    row->addWidget(rollbackButton);
    layout->addLayout(row);
    const QPointer<EditorChangeMarkers> self(this);
    const auto version = m_view->document().version();
    const auto request = m_request;
    const auto generation = m_diffGeneration;
    connect(rollbackButton, &QPushButton::clicked, popup, [self, popup, index, version, request, generation] {
        if (self && self->m_view && self->m_view->document().version() == version && self->m_request == request &&
            self->m_diffGeneration == generation) {
            self->rollback(index);
        }
        popup->closePopup(Popup::CloseReason::Accepted);
    });
    m_popup = popup;
    m_popupRollback = rollbackButton;
    popup->showAt(globalPos);
}

} // namespace frost::ui
