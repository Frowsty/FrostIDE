// SPDX-License-Identifier: Apache-2.0
// Navigation bar over a live project tree model.
#include "frost/ui/shell/navigation_bar.hpp"

#include "frost/ui/frost_style.hpp"

#include <QAbstractItemModel>
#include <QAction>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPersistentModelIndex>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>

namespace frost::ui {

namespace {

// Crisp vector chevron (›) separator between breadcrumb segments in the status bar.
class BreadcrumbSeparator final : public QLabel {
public:
    explicit BreadcrumbSeparator(QWidget* parent = nullptr) : QLabel(QStringLiteral(u"\u203A"), parent) {
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(14, 20);
        setAlignment(Qt::AlignCenter);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const FrostStyle* frost = FrostStyle::current();
        const QColor color = frost != nullptr ? frost->color(ColorKey::LabelSecondaryForeground)
                                              : palette().color(QPalette::PlaceholderText);
        painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        const qreal cx = width() * 0.5;
        const qreal cy = height() * 0.5;
        QPainterPath path;
        path.moveTo(cx - 2.0, cy - 3.5);
        path.lineTo(cx + 1.8, cy);
        path.lineTo(cx - 2.0, cy + 3.5);
        painter.drawPath(path);
    }
};

// Clean Rider-style breadcrumb button without a redundant InstantPopup menu indicator arrow.
class BreadcrumbButton final : public QToolButton {
public:
    explicit BreadcrumbButton(QWidget* parent = nullptr) : QToolButton(parent) {
        setAutoRaise(true);
        setAttribute(Qt::WA_Hover, true);
    }

    [[nodiscard]] QSize sizeHint() const override {
        const QFontMetrics fm(font());
        const int w = fm.horizontalAdvance(text()) + 12;
        return {std::max(w, 20), 20};
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const FrostStyle* frost = FrostStyle::current();
        const bool hovered = underMouse() && isEnabled();
        const bool down = isDown() || (menu() != nullptr && menu()->isVisible());

        if (down || hovered) {
            const QColor fill = frost != nullptr ? frost->color(down ? ColorKey::ButtonPressedBackground
                                                                     : ColorKey::ButtonHoverBackground)
                                                 : palette().color(QPalette::AlternateBase);
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), 4.0, 4.0);
        }
        if (hasFocus()) {
            const QColor focusCol =
                frost != nullptr ? frost->color(ColorKey::ComponentFocusColor) : QColor(0x35, 0x74, 0xF0);
            painter.setPen(QPen(focusCol, 1.2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), 4.0, 4.0);
        }

        const QColor fg = frost != nullptr ? frost->color(isEnabled() ? ColorKey::LabelForeground
                                                                      : ColorKey::LabelSecondaryForeground)
                                           : palette().color(QPalette::WindowText);
        painter.setFont(font());
        painter.setPen(fg);
        painter.drawText(rect().adjusted(6, 0, -6, 0), Qt::AlignCenter, text());
    }
};

} // namespace
NavigationBar::NavigationBar(QWidget* parent) : QWidget(parent), m_layout(new QHBoxLayout(this)) {
    setObjectName(QStringLiteral("navigationBar"));
    m_layout->setContentsMargins(6, 2, 6, 2);
    m_layout->setSpacing(2);
    hide();
}
void NavigationBar::clearModel() {
    ++m_generation;
    if (m_menu)
        m_menu->close();
    if (m_model)
        disconnect(m_model, nullptr, this, nullptr);
    m_model = nullptr;
    m_find = {};
    m_directory = {};
    m_open = {};
    rebuild();
}
void NavigationBar::setModel(QAbstractItemModel* model, int pathRole, Find find, IsDirectory directory, Open open) {
    clearModel();
    m_model = model;
    m_pathRole = pathRole;
    m_find = std::move(find);
    m_directory = std::move(directory);
    m_open = std::move(open);
    if (model) {
        connect(model, &QAbstractItemModel::modelAboutToBeReset, this, [this] {
            ++m_generation;
            if (m_menu)
                m_menu->close();
        });
        connect(model, &QAbstractItemModel::modelReset, this, [this] {
            // The reset can occur in a popup's nested event loop. Let that loop unwind before
            // rebuilding the buttons and destroying the menu that just closed.
            QTimer::singleShot(0, this, [this] { rebuild(); });
        });
        connect(model, &QObject::destroyed, this, [this] { clearModel(); });
    }
    rebuild();
}
void NavigationBar::setPath(std::string path) {
    ++m_generation;
    m_path = std::move(path);
    if (m_menu)
        m_menu->close();
    rebuild();
}
void NavigationBar::rebuild() {
    while (auto* item = m_layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (m_path.empty()) {
        hide();
        emit contentVisibilityChanged(false);
        return;
    }
    QModelIndex index = m_model && m_find ? m_find(m_path) : QModelIndex();
    if (!index.isValid()) {
        const QString path = QString::fromUtf8(m_path.data(), static_cast<qsizetype>(m_path.size()));
        auto* label = new QLabel(QFileInfo(path).fileName(), this);
        label->setToolTip(path);
        m_layout->addWidget(label);
        m_layout->addStretch();
        show();
        emit contentVisibilityChanged(true);
        return;
    }
    QList<QPersistentModelIndex> ancestors;
    while (index.isValid()) {
        ancestors.prepend(index);
        index = index.parent();
    }
    for (qsizetype i = 0; i < ancestors.size(); ++i) {
        if (i > 0) {
            auto* sep = new BreadcrumbSeparator(this);
            m_layout->addWidget(sep);
        }
        const auto& node = ancestors[i];
        auto* button = new BreadcrumbButton(this);
        button->installEventFilter(this);
        button->setAutoRaise(true);
        button->setText(node.data(Qt::DisplayRole).toString());
        button->setAccessibleName(button->text());
        if (m_directory(node) && m_model->rowCount(node) > 0) {
            button->setFocusPolicy(Qt::TabFocus);
            button->setPopupMode(QToolButton::InstantPopup);
            auto* menu = new QMenu(button);
            connect(menu, &QMenu::aboutToShow, this, [this, menu, node] {
                m_menu = menu;
                menu->clear();
                if (node.isValid())
                    populate(menu, node);
            });
            button->setMenu(menu);
        } else {
            button->setFocusPolicy(Qt::NoFocus);
            button->setEnabled(false);
        }
        m_layout->addWidget(button);
    }
    m_layout->addStretch();
    show();
    emit contentVisibilityChanged(true);
}
void NavigationBar::populate(QMenu* menu, const QModelIndex& parent) {
    if (!m_model || !parent.isValid())
        return;
    for (int row = 0; row < m_model->rowCount(parent); ++row) {
        const QPersistentModelIndex child(m_model->index(row, 0, parent));
        if (!child.isValid())
            continue;
        const QString name = child.data(Qt::DisplayRole).toString();
        if (m_directory(child) && m_model->rowCount(child) == 0) {
            auto* action = menu->addAction(name);
            action->setEnabled(false);
        } else if (m_directory(child)) {
            auto* sub = menu->addMenu(name);
            connect(sub, &QMenu::aboutToShow, this, [this, sub, child] {
                sub->clear();
                if (child.isValid())
                    populate(sub, child);
            });
        } else {
            auto* action = menu->addAction(name);
            connect(action, &QAction::triggered, this, [this, child] {
                if (!child.isValid())
                    return;
                const std::string path = child.data(m_pathRole).toString().toUtf8().toStdString();
                const auto generation = m_generation;
                QTimer::singleShot(0, this, [this, generation, path] {
                    if (generation == m_generation && m_open)
                        m_open(path);
                });
            });
        }
    }
}
void NavigationBar::focusNavigation() {
    for (QObject* child : children()) {
        if (auto* button = qobject_cast<QToolButton*>(child); button && button->isEnabled() && button->menu()) {
            button->setFocus(Qt::ShortcutFocusReason);
            return;
        }
    }
}

bool NavigationBar::hasNavigation() const {
    for (QObject* child : children()) {
        if (auto* button = qobject_cast<QToolButton*>(child); button && button->isEnabled() && button->menu()) {
            return true;
        }
    }
    return false;
}

bool NavigationBar::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::FocusIn) {
        if (auto* button = qobject_cast<QToolButton*>(watched)) {
            for (QWidget* ancestor = parentWidget(); ancestor != nullptr; ancestor = ancestor->parentWidget()) {
                if (auto* scroll = qobject_cast<QScrollArea*>(ancestor)) {
                    scroll->ensureWidgetVisible(button);
                    break;
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void NavigationBar::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    QPainter painter(this);
    const FrostStyle* frost = FrostStyle::current();
    const QColor bg =
        frost != nullptr ? frost->color(ColorKey::MainWindowBackground) : palette().color(QPalette::Window);
    painter.fillRect(rect(), bg);
}
} // namespace frost::ui
