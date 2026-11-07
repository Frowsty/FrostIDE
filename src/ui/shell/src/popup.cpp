// SPDX-License-Identifier: Apache-2.0
//
// Popup, ListPopup and InputPopup (frost/ui/shell/popup.hpp).

#include "frost/ui/shell/popup.hpp"

#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_ids.hpp"

#include "shell_util.hpp"
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace frost::ui {

namespace {

constexpr int kPrimaryTextRole = Qt::UserRole + 1;
constexpr int kDetailTextRole = Qt::UserRole + 2;
constexpr int kHeaderRole = Qt::UserRole + 3;
constexpr int kEnabledRole = Qt::UserRole + 4;

class ListPopupItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const bool isHeader = index.data(kHeaderRole).toBool();
        const int h = std::max(isHeader ? 26 : 28, option.fontMetrics.height() + (isHeader ? 8 : 10));
        return QSize(option.rect.width(), h);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const QRect rect = option.rect.adjusted(4, 1, -4, -1);
        const bool isHeader = index.data(kHeaderRole).toBool();
        const QVariant enabledVar = index.data(kEnabledRole);
        const bool isEnabled = !enabledVar.isValid() || enabledVar.toBool();
        QString primary = index.data(kPrimaryTextRole).toString();
        const QString detail = index.data(kDetailTextRole).toString();
        if (primary.isEmpty()) {
            primary = index.data(Qt::DisplayRole).toString();
        }

        if (isHeader) {
            QFont headerFont = option.font;
            headerFont.setBold(true);
            if (headerFont.pointSizeF() > 8.0) {
                headerFont.setPointSizeF(headerFont.pointSizeF() * 0.9);
            }
            painter->setFont(headerFont);
            QColor muted = option.palette.color(QPalette::PlaceholderText);
            if (!muted.isValid() || muted.alpha() == 0) {
                muted = option.palette.color(QPalette::Text);
                muted.setAlpha(155);
            }
            painter->setPen(muted);
            const QFontMetrics fm(headerFont);
            const int textWidth = fm.horizontalAdvance(primary);
            const QRect textRect(rect.left() + 6, rect.top(), textWidth + 6, rect.height());
            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, primary);

            QColor lineColor = option.palette.color(QPalette::Mid);
            lineColor.setAlpha(85);
            painter->setPen(QPen(lineColor, 1.0));
            const int lineY = rect.center().y();
            const int lineLeft = textRect.right() + 8;
            if (lineLeft < rect.right() - 6) {
                painter->drawLine(lineLeft, lineY, rect.right() - 6, lineY);
            }
            painter->restore();
            return;
        }

        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
        if (selected || hovered) {
            QColor fill = option.palette.color(QPalette::Highlight);
            fill.setAlpha(selected ? 65 : 28);
            painter->setPen(Qt::NoPen);
            painter->setBrush(fill);
            painter->drawRoundedRect(rect, 5.0, 5.0);
        }

        int leftX = rect.left() + 8;
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        if (!icon.isNull()) {
            const QRect iconRect(leftX, rect.top() + (rect.height() - 16) / 2, 16, 16);
            icon.paint(painter, iconRect, Qt::AlignCenter, isEnabled ? QIcon::Normal : QIcon::Disabled);
            leftX = iconRect.right() + 8;
        }

        int rightLimit = rect.right() - 8;
        if (!detail.isEmpty()) {
            QFont detailFont = option.font;
            if (detailFont.pointSizeF() > 8.0) {
                detailFont.setPointSizeF(detailFont.pointSizeF() * 0.92);
            }
            painter->setFont(detailFont);
            QColor detailColor = option.palette.color(QPalette::PlaceholderText);
            if (!detailColor.isValid() || detailColor.alpha() == 0) {
                detailColor = option.palette.color(QPalette::Text);
                detailColor.setAlpha(isEnabled ? 150 : 95);
            } else if (!isEnabled) {
                detailColor.setAlpha(95);
            }
            painter->setPen(detailColor);
            const QFontMetrics detailFm(detailFont);
            const int maxDetailWidth = std::max(60, rect.width() / 2);
            const QString elidedDetail = detailFm.elidedText(detail, Qt::ElideMiddle, maxDetailWidth);
            const int detailWidth = detailFm.horizontalAdvance(elidedDetail);
            const QRect detailRect(rightLimit - detailWidth, rect.top(), detailWidth, rect.height());
            painter->drawText(detailRect, Qt::AlignRight | Qt::AlignVCenter, elidedDetail);
            rightLimit = detailRect.left() - 12;
        }

        painter->setFont(option.font);
        QColor textColor = option.palette.color(isEnabled ? QPalette::Active : QPalette::Disabled, QPalette::Text);
        if (!isEnabled && textColor == option.palette.color(QPalette::Active, QPalette::Text)) {
            textColor.setAlpha(115);
        }
        painter->setPen(textColor);
        const int availWidth = std::max(24, rightLimit - leftX);
        const QFontMetrics mainFm(option.font);
        const QRect mainRect(leftX, rect.top(), availWidth, rect.height());
        painter->drawText(mainRect, Qt::AlignLeft | Qt::AlignVCenter,
                          mainFm.elidedText(primary, Qt::ElideRight, availWidth));

        painter->restore();
    }
};

} // namespace

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
    const QPoint origin = window->isVisible() ? window->mapToGlobal(QPoint(0, 0)) : window->geometry().topLeft();
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
    move(globalTopLeft);
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
    painter.setRenderHint(QPainter::Antialiasing, true);
    const FrostStyle* style = FrostStyle::current();
    const QColor background =
        style != nullptr ? style->color(ColorKey::PopupBackground) : palette().color(QPalette::Window);
    const QColor border = style != nullptr ? style->color(ColorKey::PopupBorderColor) : palette().color(QPalette::Mid);
    const QColor accent =
        style != nullptr ? style->color(ColorKey::TabbedPaneUnderlineColor) : QColor(0x35, 0x74, 0xF0);
    const bool isDark = background.lightness() < 128;

    painter.fillRect(rect(), background);
    shell_detail::paintEllipticalGlow(painter, QPointF(width() * 0.25, 0.0), width() * 0.65, std::min(160, height()),
                                      accent, isDark ? 32 : 18);
    shell_detail::paintFrostedGlassSheen(painter, rect(), isDark, true);

    painter.setPen(QPen(border, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8.0, 8.0);
}

ListPopup::ListPopup(QWidget* owner, const QString& title, bool filterable)
    : Popup(owner), m_title(new QLabel(title, this)), m_filter(new QLineEdit(this)), m_list(new QListWidget(this)) {
    setObjectName(QStringLiteral("listPopup"));
    m_title->setObjectName(QStringLiteral("popupTitle"));
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    m_title->setVisible(!title.isEmpty());
    m_scopeBar = new QWidget(this);
    m_scopeBar->setObjectName(QStringLiteral("popupScopeTabs"));
    m_scopeLayout = new QHBoxLayout(m_scopeBar);
    m_scopeLayout->setContentsMargins(0, 0, 0, 0);
    m_scopeLayout->setSpacing(4);
    m_scopeBar->setVisible(false);
    m_filter->setObjectName(QStringLiteral("popupFilter"));
    m_filter->setVisible(filterable);
    m_filter->setClearButtonEnabled(true);
    m_filter->setMinimumHeight(28);
    m_filter->setPlaceholderText(shell_detail::text("Type to filter"));
    if (filterable) {
        if (const QIcon searchIcon = shell_detail::themedIcon(icons::kActionsSearch); !searchIcon.isNull()) {
            m_filter->addAction(searchIcon, QLineEdit::LeadingPosition);
        }
    }
    m_list->setObjectName(QStringLiteral("popupList"));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setItemDelegate(new ListPopupItemDelegate(m_list));
    m_list->viewport()->setAttribute(Qt::WA_Hover, true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 6);
    layout->setSpacing(6);
    layout->addWidget(m_title);
    layout->addWidget(m_scopeBar);
    layout->addWidget(m_filter);
    auto* topDivider = new QFrame(this);
    topDivider->setFrameShape(QFrame::HLine);
    topDivider->setFrameShadow(QFrame::Plain);
    layout->addWidget(topDivider);
    layout->addWidget(m_list, 1);
    auto* footer = new QLabel(
        filterable
            ? shell_detail::text("Enter to accept  •  Up/Down to navigate  •  Tab to switch scope  •  Esc to close")
            : shell_detail::text("Enter to switch  •  Up/Down to navigate  •  Esc to cancel"),
        this);
    footer->setObjectName(QStringLiteral("popupFooter"));
    QFont footerFont = footer->font();
    if (footerFont.pointSizeF() > 8.0) {
        footerFont.setPointSizeF(footerFont.pointSizeF() * 0.88);
    }
    footer->setFont(footerFont);
    layout->addWidget(footer);
    setFocusProxy(filterable ? static_cast<QWidget*>(m_filter) : m_list);
    m_filter->installEventFilter(this);
    m_list->installEventFilter(this);
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString& value) {
        applyFilter();
        emit filterTextChanged(value);
    });
    connect(m_list, &QListWidget::itemActivated, this, [this] { acceptCurrent(); });
    setMinimumSize(420, 320);
    resize(580, 380);
}

void ListPopup::setFilterMode(FilterMode mode) {
    m_mode = mode;
    applyFilter();
}

void ListPopup::setTitle(const QString& title) {
    m_title->setText(title);
    m_title->setVisible(!title.isEmpty() && m_scopeButtons.empty());
}

void ListPopup::setScopeTabs(const QStringList& tabs, int activeIndex) {
    for (QToolButton* btn : m_scopeButtons) {
        delete btn;
    }
    m_scopeButtons.clear();
    while (QLayoutItem* item = m_scopeLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (tabs.isEmpty()) {
        m_activeScopeTab = -1;
        m_scopeBar->setVisible(false);
        m_title->setVisible(!m_title->text().isEmpty());
        return;
    }
    m_scopeButtons.reserve(static_cast<std::size_t>(tabs.size()));
    for (int i = 0; i < tabs.size(); ++i) {
        auto* btn = new QToolButton(m_scopeBar);
        btn->setObjectName(QStringLiteral("popupScopeTab.%1").arg(i));
        btn->setText(tabs[i]);
        btn->setCheckable(true);
        btn->setAutoRaise(true);
        btn->setFocusPolicy(Qt::NoFocus);
        connect(btn, &QToolButton::clicked, this, [this, i] {
            setActiveScopeTab(i);
            emit scopeTabClicked(i);
        });
        m_scopeLayout->addWidget(btn);
        m_scopeButtons.push_back(btn);
    }
    m_scopeLayout->addStretch(1);
    auto* hint = new QLabel(shell_detail::text("Tab to switch"), m_scopeBar);
    QFont hintFont = hint->font();
    if (hintFont.pointSizeF() > 8.0) {
        hintFont.setPointSizeF(hintFont.pointSizeF() * 0.88);
    }
    hint->setFont(hintFont);
    m_scopeLayout->addWidget(hint);
    m_title->setVisible(false);
    m_scopeBar->setVisible(true);
    setActiveScopeTab(activeIndex);
}

void ListPopup::setActiveScopeTab(int activeIndex) {
    m_activeScopeTab = activeIndex;
    for (std::size_t i = 0; i < m_scopeButtons.size(); ++i) {
        const bool active = static_cast<int>(i) == activeIndex;
        m_scopeButtons[i]->setChecked(active);
        QFont f = m_scopeButtons[i]->font();
        f.setBold(active);
        m_scopeButtons[i]->setFont(f);
    }
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
        row->setData(kPrimaryTextRole, item.text);
        row->setData(kDetailTextRole, item.detail);
        row->setData(kHeaderRole, item.header);
        row->setData(kEnabledRole, item.enabled);
        if (item.header) {
            row->setFlags(Qt::NoItemFlags);
            QFont font = row->font();
            font.setBold(true);
            row->setFont(font);
        } else if (!item.enabled) {
            row->setForeground(m_list->palette().brush(QPalette::Disabled, QPalette::Text));
        }
    }
    applyFilter();
}

bool ListPopup::selectable(int row) const {
    const QListWidgetItem* item = m_list->item(row);
    if (item == nullptr || item->isHidden()) {
        return false;
    }
    return !m_items[static_cast<std::size_t>(item->data(Qt::UserRole).toInt())].header;
}

void ListPopup::setCurrentItem(int index) {
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem* item = m_list->item(row);
        if (item->data(Qt::UserRole).toInt() == index && selectable(row)) {
            m_list->setCurrentRow(row);
            return;
        }
    }
}

int ListPopup::currentItem() const {
    const int row = m_list->currentRow();
    return row >= 0 && selectable(row) ? m_list->item(row)->data(Qt::UserRole).toInt() : -1;
}

int ListPopup::visibleCount() const {
    int visible = 0;
    for (int row = 0; row < m_list->count(); ++row) {
        visible += selectable(row) ? 1 : 0;
    }
    return visible;
}

void ListPopup::moveSelection(int delta) {
    std::vector<int> rows;
    for (int row = 0; row < m_list->count(); ++row) {
        if (selectable(row)) {
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
    if (index < 0 || !m_items[static_cast<std::size_t>(index)].enabled) {
        return;
    }
    const QVariant data = m_items[static_cast<std::size_t>(index)].data;
    closePopup(CloseReason::Accepted);
    emit itemAccepted(index, data);
}

bool ListPopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_filter || watched == m_list) {
        if (event->type() == QEvent::ShortcutOverride) {
            const auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Tab || keyEvent->key() == Qt::Key_Backtab) {
                event->accept();
                return true;
            }
        }
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
    case Qt::Key_Tab: emit tabPressed((event.modifiers() & Qt::ShiftModifier) != 0); return true;
    case Qt::Key_Backtab: emit tabPressed(true); return true;
    case Qt::Key_Return:
    case Qt::Key_Enter: acceptCurrent(); return true;
    case Qt::Key_Escape: closePopup(CloseReason::Cancelled); return true;
    default: return false;
    }
}

void ListPopup::applyFilter() {
    const QString needle = m_mode == FilterMode::Substring ? m_filter->text().trimmed() : QString();
    int firstVisible = -1;
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem* item = m_list->item(row);
        const ListPopupItem& source = m_items[static_cast<std::size_t>(item->data(Qt::UserRole).toInt())];
        const bool visible = needle.isEmpty() || source.text.contains(needle, Qt::CaseInsensitive);
        item->setHidden(!visible);
        if (visible && !source.header && firstVisible < 0) {
            firstVisible = row;
        }
    }
    const int current = m_list->currentRow();
    if (current < 0 || !selectable(current)) {
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
