// SPDX-License-Identifier: Apache-2.0
//
// Popup, ListPopup and InputPopup (frost/ui/shell/popup.hpp).

#include "frost/ui/shell/popup.hpp"

#include "frost/ui/frost_style.hpp"

#include "shell_util.hpp"
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace frost::ui {

Popup::Popup(QWidget* owner) : QFrame(owner, Qt::Popup | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("frostPopup"));
    setAttribute(Qt::WA_TranslucentBackground, false);
    setFocusPolicy(Qt::StrongFocus);
}

Popup::~Popup() = default;

void Popup::showCentered(QWidget* window, double topFraction) {
    adjustSize();
    if (window == nullptr) {
        openAt(QPoint());
        return;
    }
    const QPoint origin = window->mapToGlobal(QPoint(0, 0));
    const int x = origin.x() + std::max(0, (window->width() - width()) / 2);
    const int y = origin.y() + static_cast<int>(window->height() * topFraction);
    openAt(QPoint(x, y));
}

void Popup::showBelow(QWidget* anchor) {
    adjustSize();
    openAt(anchor != nullptr ? anchor->mapToGlobal(QPoint(0, anchor->height())) : QPoint());
}

void Popup::openAt(QPoint globalTopLeft) {
    move(globalTopLeft);
    m_open = true;
    show();
    raise();
    activateWindow();
    if (QWidget* proxy = focusProxy(); proxy != nullptr) {
        proxy->setFocus(Qt::PopupFocusReason);
    } else {
        setFocus(Qt::PopupFocusReason);
    }
}

void Popup::closePopup(CloseReason reason) {
    if (!m_open) {
        return;
    }
    m_open = false;
    hide();
    emit closed(reason);
    if (m_deleteOnClose) {
        deleteLater();
    }
}

void Popup::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        event->accept();
        closePopup(CloseReason::Cancelled);
        return;
    }
    QFrame::keyPressEvent(event);
}

void Popup::hideEvent(QHideEvent* event) {
    QFrame::hideEvent(event);
    // Qt hides a popup on a click outside it; that is a dismissal.
    closePopup(CloseReason::Dismissed);
}

void Popup::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    const FrostStyle* style = FrostStyle::current();
    const QColor background =
        style != nullptr ? style->color(ColorKey::PopupBackground) : palette().color(QPalette::Window);
    const QColor border = style != nullptr ? style->color(ColorKey::PopupBorderColor) : palette().color(QPalette::Mid);
    painter.fillRect(rect(), background);
    painter.setPen(border);
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

ListPopup::ListPopup(QWidget* owner, const QString& title, bool filterable)
    : Popup(owner), m_title(new QLabel(title, this)), m_filter(new QLineEdit(this)), m_list(new QListWidget(this)) {
    setObjectName(QStringLiteral("listPopup"));
    m_title->setObjectName(QStringLiteral("popupTitle"));
    m_title->setVisible(!title.isEmpty());
    m_filter->setObjectName(QStringLiteral("popupFilter"));
    m_filter->setVisible(filterable);
    m_filter->setPlaceholderText(shell_detail::text("Type to filter"));
    m_list->setObjectName(QStringLiteral("popupList"));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setUniformItemSizes(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    layout->addWidget(m_title);
    layout->addWidget(m_filter);
    layout->addWidget(m_list, 1);
    setFocusProxy(filterable ? static_cast<QWidget*>(m_filter) : m_list);
    m_filter->installEventFilter(this);
    m_list->installEventFilter(this);
    connect(m_filter, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    connect(m_list, &QListWidget::itemActivated, this, [this] { acceptCurrent(); });
    resize(480, 320);
}

void ListPopup::setItems(std::vector<ListPopupItem> items) {
    m_items = std::move(items);
    m_list->clear();
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        const ListPopupItem& item = m_items[i];
        QString label = item.text;
        if (!item.detail.isEmpty()) {
            label += QStringLiteral("    ") + item.detail;
        }
        auto* row = new QListWidgetItem(item.icon, label, m_list);
        row->setData(Qt::UserRole, static_cast<int>(i));
    }
    applyFilter();
}

void ListPopup::setCurrentItem(int index) {
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem* item = m_list->item(row);
        if (item->data(Qt::UserRole).toInt() == index && !item->isHidden()) {
            m_list->setCurrentRow(row);
            return;
        }
    }
}

int ListPopup::currentItem() const {
    const QListWidgetItem* item = m_list->currentItem();
    return item != nullptr && !item->isHidden() ? item->data(Qt::UserRole).toInt() : -1;
}

int ListPopup::visibleCount() const {
    int visible = 0;
    for (int row = 0; row < m_list->count(); ++row) {
        visible += m_list->item(row)->isHidden() ? 0 : 1;
    }
    return visible;
}

void ListPopup::moveSelection(int delta) {
    std::vector<int> rows;
    for (int row = 0; row < m_list->count(); ++row) {
        if (!m_list->item(row)->isHidden()) {
            rows.push_back(row);
        }
    }
    if (rows.empty()) {
        return;
    }
    const auto it = std::ranges::find(rows, m_list->currentRow());
    const int size = static_cast<int>(rows.size());
    const int at = it == rows.end() ? (delta > 0 ? -1 : 0) : static_cast<int>(it - rows.begin());
    const int next = ((at + delta) % size + size) % size;
    m_list->setCurrentRow(rows[static_cast<std::size_t>(next)]);
}

void ListPopup::setFilterText(const QString& text) { m_filter->setText(text); }

void ListPopup::acceptCurrent() {
    const int index = currentItem();
    if (index < 0) {
        return;
    }
    const QVariant data = m_items[static_cast<std::size_t>(index)].data;
    closePopup(CloseReason::Accepted);
    emit itemAccepted(index, data);
}

bool ListPopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_filter || watched == m_list) {
        if (event->type() == QEvent::KeyPress) {
            return handleKey(*static_cast<QKeyEvent*>(event));
        }
        if (event->type() == QEvent::KeyRelease && m_acceptOnRelease &&
            static_cast<QKeyEvent*>(event)->key() == *m_acceptOnRelease) {
            acceptCurrent();
            return true;
        }
    }
    return Popup::eventFilter(watched, event);
}

bool ListPopup::handleKey(QKeyEvent& event) {
    const int page = std::max(1, m_list->height() / std::max(1, m_list->sizeHintForRow(0)));
    switch (event.key()) {
    case Qt::Key_Down: moveSelection(1); return true;
    case Qt::Key_Up: moveSelection(-1); return true;
    case Qt::Key_PageDown: moveSelection(page); return true;
    case Qt::Key_PageUp: moveSelection(-page); return true;
    case Qt::Key_Return:
    case Qt::Key_Enter: acceptCurrent(); return true;
    case Qt::Key_Escape: closePopup(CloseReason::Cancelled); return true;
    default: return false;
    }
}

void ListPopup::applyFilter() {
    const QString needle = m_filter->text().trimmed();
    int firstVisible = -1;
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem* item = m_list->item(row);
        const ListPopupItem& source = m_items[static_cast<std::size_t>(item->data(Qt::UserRole).toInt())];
        const bool visible = needle.isEmpty() || source.text.contains(needle, Qt::CaseInsensitive);
        item->setHidden(!visible);
        if (visible && firstVisible < 0) {
            firstVisible = row;
        }
    }
    const QListWidgetItem* current = m_list->currentItem();
    if (current == nullptr || current->isHidden()) {
        m_list->setCurrentRow(firstVisible);
    }
}

InputPopup::InputPopup(QWidget* owner, const QString& prompt, const QString& initial)
    : Popup(owner), m_field(new QLineEdit(initial, this)), m_error(new QLabel(this)) {
    setObjectName(QStringLiteral("inputPopup"));
    auto* label = new QLabel(prompt, this);
    label->setObjectName(QStringLiteral("popupPrompt"));
    m_field->setObjectName(QStringLiteral("popupField"));
    m_field->selectAll();
    m_error->setObjectName(QStringLiteral("popupError"));
    m_error->hide();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(4);
    layout->addWidget(label);
    layout->addWidget(m_field);
    layout->addWidget(m_error);
    setFocusProxy(m_field);
    m_field->installEventFilter(this);
    setMinimumWidth(320);
}

void InputPopup::submit() {
    const QString value = m_field->text();
    if (m_validator) {
        if (std::optional<QString> problem = m_validator(value); problem) {
            m_error->setText(*problem);
            m_error->show();
            adjustSize();
            return;
        }
    }
    closePopup(CloseReason::Accepted);
    emit submitted(value);
}

bool InputPopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_field && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            submit();
            return true;
        }
        if (key == Qt::Key_Escape) {
            closePopup(CloseReason::Cancelled);
            return true;
        }
    }
    return Popup::eventFilter(watched, event);
}

} // namespace frost::ui
