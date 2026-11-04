// SPDX-License-Identifier: Apache-2.0
//
// MainToolbar (frost/ui/shell/main_toolbar.hpp): JetBrains Rider New UI custom themed window
// header with FrostIDE app badge + main menu button, colored project avatar badge, run/build
// configuration controls, quick theme switcher, and integrated window caption buttons.

#include "frost/ui/shell/main_toolbar.hpp"

#include "frost/input/keymap.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/shell/shell_actions.hpp"

#include "shell_util.hpp"
#include <QAbstractItemModel>
#include <QActionEvent>
#include <QApplication>
#include <QComboBox>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QTimer>
#include <QWindow>

#include <array>
#include <cctype>
#include <utility>

namespace frost::ui {

using shell_detail::text;

namespace {

QColor themeColor(ColorKey key, QColor fallback) {
    if (const FrostStyle* style = FrostStyle::current(); style != nullptr) {
        return QColor::fromRgba(style->theme().color(key).argb);
    }
    return fallback;
}

int scaledPx(int logical) {
    if (const FrostStyle* style = FrostStyle::current(); style != nullptr) {
        return style->scale().px(logical);
    }
    return logical;
}

QString projectInitials(const QString& rawName) {
    const QString trimmed = rawName.trimmed();
    if (trimmed.isEmpty() || trimmed.compare(QStringLiteral("No project"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("FI");
    }
    QString first;
    QString second;
    bool prevSeparator = true;
    for (int i = 0; i < trimmed.size(); ++i) {
        const QChar ch = trimmed[i];
        if (!ch.isLetterOrNumber()) {
            prevSeparator = true;
            continue;
        }
        if (first.isEmpty()) {
            first = ch.toUpper();
            prevSeparator = false;
        } else if (prevSeparator || (ch.isUpper() && i > 0 && trimmed[i - 1].isLower())) {
            second = ch.toUpper();
            break;
        } else if (second.isEmpty()) {
            second = ch.toUpper();
        }
    }
    return first + second;
}

QColor projectAvatarColor(const QString& name) {
    static constexpr std::array<QRgb, 6> kAvatarColors = {
        0xFF3574F0U, // Rider Blue
        0xFF7C4DFFU, // Deep Violet
        0xFF009688U, // Teal Emerald
        0xFFE0565BU, // Coral Red
        0xFFD97724U, // Warm Amber
        0xFF268BD2U, // Ocean Blue
    };
    unsigned int hash = 2166136261U;
    const QByteArray utf8 = name.toUtf8();
    for (char c : utf8) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619U;
    }
    return QColor::fromRgba(kAvatarColors[hash % kAvatarColors.size()]);
}

// Far-left Rider header button: FrostIDE geometric badge + 3-bar Main Menu icon.
class RiderMenuBadgeButton final : public QToolButton {
public:
    explicit RiderMenuBadgeButton(QWidget* parent = nullptr) : QToolButton(parent) {
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
    }

    [[nodiscard]] QSize sizeHint() const override { return {scaledPx(56), scaledPx(30)}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool down = isDown() || (menu() != nullptr && menu()->isVisible());
        const bool hovered = underMouse() && isEnabled();
        if (down || hovered) {
            const QColor fill = down ? themeColor(ColorKey::ButtonPressedBackground, QColor(70, 74, 82))
                                     : themeColor(ColorKey::ButtonHoverBackground, QColor(58, 61, 68));
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            const qreal radius = scaledPx(6);
            painter.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), radius, radius);
        }

        // 1. FrostIDE geometric app badge on the left (22x22 rounded tile).
        const int badgeSize = scaledPx(22);
        const int badgeX = scaledPx(6);
        const int badgeY = (height() - badgeSize) / 2;
        const QRectF badgeRect(badgeX, badgeY, badgeSize, badgeSize);

        QLinearGradient grad(badgeRect.topLeft(), badgeRect.bottomRight());
        grad.setColorAt(0.0, QColor(0x35, 0x74, 0xF0));
        grad.setColorAt(0.55, QColor(0x6B, 0x4D, 0xFF));
        grad.setColorAt(1.0, QColor(0x00, 0xB4, 0xD8));
        painter.setPen(Qt::NoPen);
        painter.setBrush(grad);
        const qreal badgeRadius = scaledPx(6);
        painter.drawRoundedRect(badgeRect, badgeRadius, badgeRadius);

        // Crisp white "F" monogram inside the app badge.
        QFont badgeFont = font();
        badgeFont.setBold(true);
        badgeFont.setPixelSize(scaledPx(12));
        painter.setFont(badgeFont);
        painter.setPen(QColor(255, 255, 255));
        painter.drawText(badgeRect, Qt::AlignCenter, QStringLiteral("F"));

        // 2. Rider 3-bar hamburger menu glyph to the right of the badge.
        const QColor fg = themeColor(ColorKey::LabelForeground, QColor(220, 223, 228));
        QPen barPen(fg, std::max(1.5, scaledPx(2) * 0.85), Qt::SolidLine, Qt::RoundCap);
        painter.setPen(barPen);
        const qreal menuLeft = badgeRect.right() + scaledPx(8);
        const qreal menuRight = width() - scaledPx(7);
        const qreal cy = height() * 0.5;
        const qreal step = scaledPx(4);
        painter.drawLine(QPointF(menuLeft, cy - step), QPointF(menuRight, cy - step));
        painter.drawLine(QPointF(menuLeft, cy), QPointF(menuRight, cy));
        painter.drawLine(QPointF(menuLeft, cy + step), QPointF(menuRight, cy + step));
    }
};

// Rider Project Widget button: colored 2-letter project initials avatar + bold project name + chevron.
class RiderProjectButton final : public QToolButton {
public:
    explicit RiderProjectButton(QWidget* parent = nullptr) : QToolButton(parent) {
        setAutoRaise(true);
        setAttribute(Qt::WA_Hover, true);
    }

    [[nodiscard]] QSize sizeHint() const override {
        QFont boldFont = font();
        boldFont.setBold(true);
        const QFontMetrics fm(boldFont);
        const int textWidth = fm.horizontalAdvance(text());
        const int chevronSpace = menu() != nullptr ? scaledPx(16) : scaledPx(6);
        const int w = scaledPx(8) + scaledPx(22) + scaledPx(8) + textWidth + chevronSpace + scaledPx(8);
        return {std::max(w, scaledPx(96)), scaledPx(30)};
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool down = isDown() || (menu() != nullptr && menu()->isVisible());
        const bool hovered = underMouse() && isEnabled();
        if (down || hovered) {
            const QColor fill = down ? themeColor(ColorKey::ButtonPressedBackground, QColor(70, 74, 82))
                                     : themeColor(ColorKey::ButtonHoverBackground, QColor(58, 61, 68));
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            const qreal radius = scaledPx(6);
            painter.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), radius, radius);
        }

        // Colored 22x22 rounded project avatar badge.
        const int avatarSize = scaledPx(22);
        const int avatarX = scaledPx(6);
        const int avatarY = (height() - avatarSize) / 2;
        const QRectF avatarRect(avatarX, avatarY, avatarSize, avatarSize);

        painter.setPen(Qt::NoPen);
        painter.setBrush(projectAvatarColor(text()));
        const qreal avatarRadius = scaledPx(6);
        painter.drawRoundedRect(avatarRect, avatarRadius, avatarRadius);

        QFont initialsFont = font();
        initialsFont.setBold(true);
        initialsFont.setPixelSize(scaledPx(10));
        painter.setFont(initialsFont);
        painter.setPen(QColor(255, 255, 255));
        painter.drawText(avatarRect, Qt::AlignCenter, projectInitials(text()));

        // Project name in bold LabelForeground.
        QFont titleFont = font();
        titleFont.setBold(true);
        painter.setFont(titleFont);
        painter.setPen(themeColor(ColorKey::LabelForeground, QColor(220, 223, 228)));

        const int textLeft = avatarX + avatarSize + scaledPx(8);
        const int chevronWidth = menu() != nullptr ? scaledPx(16) : scaledPx(6);
        const QRect textRect(textLeft, 0, std::max(0, width() - textLeft - chevronWidth - scaledPx(4)), height());
        const QString elided = painter.fontMetrics().elidedText(text(), Qt::ElideRight, textRect.width());
        painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);

        // Dropdown chevron when a project menu is attached.
        if (menu() != nullptr) {
            const QColor secondary = themeColor(ColorKey::LabelSecondaryForeground, QColor(155, 160, 170));
            painter.setPen(QPen(secondary, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const qreal cx = width() - scaledPx(11);
            const qreal cy = height() * 0.5;
            const qreal d = scaledPx(3);
            QPainterPath chevron;
            chevron.moveTo(cx - d, cy - d * 0.5);
            chevron.lineTo(cx, cy + d * 0.6);
            chevron.lineTo(cx + d, cy - d * 0.5);
            painter.drawPath(chevron);
        }
    }
};

// Integrated Window Caption Button (Minimize, Maximize/Restore, Close).
class WindowCaptionButton final : public QToolButton {
public:
    enum class Kind { Minimize, MaximizeRestore, Close };

    WindowCaptionButton(Kind kind, QWidget* parent = nullptr) : QToolButton(parent), m_kind(kind) {
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover, true);
    }

    [[nodiscard]] QSize sizeHint() const override { return {scaledPx(40), scaledPx(30)}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool down = isDown();
        const bool hovered = underMouse() && isEnabled();
        QColor glyphColor = themeColor(ColorKey::LabelForeground, QColor(215, 218, 224));

        if (down || hovered) {
            QColor fill;
            if (m_kind == Kind::Close) {
                fill = down ? QColor(0xC5, 0x0F, 0x1F) : QColor(0xE8, 0x11, 0x23);
                glyphColor = QColor(255, 255, 255);
            } else {
                fill = down ? themeColor(ColorKey::ButtonPressedBackground, QColor(70, 74, 82))
                            : themeColor(ColorKey::ButtonHoverBackground, QColor(58, 61, 68));
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            const qreal radius = scaledPx(5);
            painter.drawRoundedRect(QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0), radius, radius);
        }

        const qreal cx = width() * 0.5;
        const qreal cy = height() * 0.5;
        const qreal half = scaledPx(5);
        painter.setPen(QPen(glyphColor, 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);

        switch (m_kind) {
        case Kind::Minimize: painter.drawLine(QPointF(cx - half, cy), QPointF(cx + half, cy)); break;
        case Kind::MaximizeRestore: {
            const QWidget* top = window();
            const bool maximized = top != nullptr && top->isMaximized();
            if (maximized) {
                const qreal box = scaledPx(7);
                painter.drawRect(QRectF(cx - half + 2.0, cy - half, box, box));
                painter.fillRect(QRectF(cx - half, cy - half + 2.0, box, box),
                                 hovered ? themeColor(ColorKey::ButtonHoverBackground, QColor(58, 61, 68))
                                         : themeColor(ColorKey::MainWindowBackground, QColor(30, 31, 34)));
                painter.drawRect(QRectF(cx - half, cy - half + 2.0, box, box));
            } else {
                painter.drawRect(QRectF(cx - half, cy - half, half * 2.0, half * 2.0));
            }
            break;
        }
        case Kind::Close:
            painter.drawLine(QPointF(cx - half, cy - half), QPointF(cx + half, cy + half));
            painter.drawLine(QPointF(cx + half, cy - half), QPointF(cx - half, cy + half));
            break;
        }
    }

private:
    Kind m_kind;
};

} // namespace

MainToolbar::MainToolbar(Invoker invoke, QWidget* parent)
    : QToolBar(parent), m_invoke(std::move(invoke)), m_mainMenuButton(new RiderMenuBadgeButton(this)),
      m_mainMenu(new QMenu(m_mainMenuButton)), m_project(new RiderProjectButton(this)),
      m_projectMenu(new QMenu(m_project)), m_configurations(new QComboBox(this)), m_themeButton(new QToolButton(this)),
      m_minimizeButton(new WindowCaptionButton(WindowCaptionButton::Kind::Minimize, this)),
      m_maximizeButton(new WindowCaptionButton(WindowCaptionButton::Kind::MaximizeRestore, this)),
      m_closeButton(new WindowCaptionButton(WindowCaptionButton::Kind::Close, this)) {
    setObjectName(QStringLiteral("mainToolbar"));
    setWindowTitle(text("Main Toolbar"));
    setMovable(false);
    setFloatable(false);
    setContextMenuPolicy(Qt::PreventContextMenu);
    setContentsMargins(scaledPx(6), scaledPx(3), scaledPx(6), scaledPx(4));
    setMinimumHeight(scaledPx(38));

    // 1. Far-left FrostIDE badge + Main Menu button.
    m_mainMenuButton->setObjectName(QStringLiteral("toolbar.mainMenu"));
    m_mainMenuButton->setToolTip(text("Main Menu"));
    m_mainMenuButton->setAccessibleName(text("Main Menu"));
    m_mainMenu->setObjectName(QStringLiteral("headerMainMenu"));
    connect(m_mainMenu, &QMenu::aboutToShow, this, [this] {
        m_mainMenu->clear();
        if (m_mainMenuBuilder) {
            m_mainMenuBuilder(*m_mainMenu);
        }
    });
    m_mainMenuButton->setMenu(m_mainMenu);
    m_mainMenuButton->setPopupMode(QToolButton::InstantPopup);
    addWidget(m_mainMenuButton);

    // 2. Rider Project Widget (colored initials avatar + bold project name).
    m_project->setObjectName(QStringLiteral("projectWidget"));
    m_project->setAutoRaise(true);
    // Project identity is informational until project switching is implemented.
    m_project->setFocusPolicy(Qt::NoFocus);
    m_project->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_project->setIcon(shell_detail::themedIcon(icons::kNodesFolder));
    m_project->setToolTip(text("Current project"));
    m_projectMenu->setObjectName(QStringLiteral("projectSwitcherMenu"));
    connect(m_projectMenu, &QMenu::aboutToShow, this, [this] {
        m_projectMenu->clear();
        if (m_projectMenuBuilder) {
            m_projectMenuBuilder(*m_projectMenu);
        }
    });
    QFont projectFont = m_project->font();
    projectFont.setBold(true);
    m_project->setFont(projectFont);
    setProjectName({});
    addWidget(m_project);

    auto* spacer = new QWidget(this);
    spacer->setObjectName(QStringLiteral("toolbarSpacer"));
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    spacer->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    addWidget(spacer);

    m_configurations->setObjectName(QStringLiteral("runConfigurations"));
    m_configurations->setMinimumContentsLength(18);
    m_configurations->setPlaceholderText(text("No configurations"));
    m_configurations->setToolTip(text("Run/Build Configuration"));
    m_configurations->setEnabled(false);
    addWidget(m_configurations);
    // The slot is usable as soon as a later slice adds configurations.
    const auto updateCombo = [this] { m_configurations->setEnabled(m_configurations->count() > 0); };
    connect(m_configurations->model(), &QAbstractItemModel::rowsInserted, this, updateCombo);
    connect(m_configurations->model(), &QAbstractItemModel::rowsRemoved, this, updateCombo);
    connect(m_configurations->model(), &QAbstractItemModel::modelReset, this, updateCombo);

    addSeparator();
    addSlot(kBuild, icons::kBuildBuild, "Build");
    addSeparator();
    addSlot(kRun, icons::kRunRun, "Run");
    addSlot(kDebug, icons::kRunDebug, "Debug");
    addSlot(kStop, icons::kRunStop, "Stop");
    auto* utilitySpacer = new QWidget(this);
    utilitySpacer->setObjectName(QStringLiteral("toolbarUtilitySpacer"));
    utilitySpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    utilitySpacer->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    addWidget(utilitySpacer);
    addSeparator();
    addSlot(kSearch, icons::kActionsSearch, "Search");

    // Quick Switch Theme button (Qt::NoFocus so it does not alter toolbar Tab focus order).
    m_themeButton->setObjectName(QStringLiteral("toolbar.view.quickSwitchScheme"));
    m_themeButton->setAutoRaise(true);
    m_themeButton->setFocusPolicy(Qt::NoFocus);
    const QIcon paletteIcon = shell_detail::themedIcon(icons::kActionsShow);
    if (!paletteIcon.isNull()) {
        m_themeButton->setIcon(paletteIcon);
    } else {
        m_themeButton->setText(text("Theme"));
    }
    m_themeButton->setToolTip(text("Quick Switch Theme... (Ctrl+`)"));
    m_themeButton->setAccessibleName(text("Quick Switch Theme"));
    connect(m_themeButton, &QToolButton::clicked, this, [this] {
        if (m_invoke) {
            m_invoke(actions::kQuickSwitchTheme);
        }
    });
    addWidget(m_themeButton);

    addSlot(kSettings, icons::kActionsSettings, "Settings");

    // Integrated Window Caption Buttons on the far right (Minimize, Maximize/Restore, Close).
    addSeparator();
    m_minimizeButton->setObjectName(QStringLiteral("windowMinimizeButton"));
    m_minimizeButton->setToolTip(text("Minimize"));
    m_minimizeButton->setAccessibleName(text("Minimize"));
    connect(m_minimizeButton, &QToolButton::clicked, this, [this] {
        if (QWidget* top = window()) {
            top->showMinimized();
        }
    });
    addWidget(m_minimizeButton);

    m_maximizeButton->setObjectName(QStringLiteral("windowMaximizeButton"));
    m_maximizeButton->setToolTip(text("Maximize / Restore"));
    m_maximizeButton->setAccessibleName(text("Maximize"));
    connect(m_maximizeButton, &QToolButton::clicked, this, [this] {
        if (QWidget* top = window()) {
            if (top->isMaximized()) {
                top->showNormal();
            } else {
                top->showMaximized();
            }
            m_maximizeButton->update();
        }
    });
    addWidget(m_maximizeButton);

    m_closeButton->setObjectName(QStringLiteral("windowCloseButton"));
    m_closeButton->setToolTip(text("Close"));
    m_closeButton->setAccessibleName(text("Close"));
    connect(m_closeButton, &QToolButton::clicked, this, [this] {
        if (QWidget* top = window()) {
            top->close();
        }
    });
    addWidget(m_closeButton);
}

void MainToolbar::setMainMenuBuilder(std::function<void(QMenu&)> builder) {
    m_mainMenu->close();
    m_mainMenuBuilder = std::move(builder);
}

void MainToolbar::setProjectName(const QString& name) {
    m_project->setText(name.isEmpty() ? text("No project") : name);
    m_project->setAccessibleName(text("Project: %1").arg(m_project->text()));
    m_project->updateGeometry();
    m_project->update();
}

void MainToolbar::setProjectMenuBuilder(std::function<void(QMenu&)> builder) {
    m_projectMenu->close();
    m_projectMenuBuilder = std::move(builder);
    const bool available = static_cast<bool>(m_projectMenuBuilder);
    m_project->setMenu(available ? m_projectMenu : nullptr);
    m_project->setPopupMode(available ? QToolButton::InstantPopup : QToolButton::DelayedPopup);
    m_project->setFocusPolicy(available ? Qt::TabFocus : Qt::NoFocus);
    m_project->setToolTip(available ? text("Open or switch project") : text("Current project"));
    m_project->updateGeometry();
    m_project->update();
    updateTabOrder();
}

QToolButton* MainToolbar::button(std::string_view actionId) const {
    for (const Slot& slot : m_slots) {
        if (slot.slotId == actionId || slot.actionId == actionId) {
            return slot.button;
        }
    }
    return nullptr;
}

void MainToolbar::setSlotAction(std::string_view slotId, std::string_view actionId) {
    for (Slot& slot : m_slots) {
        if (slot.slotId == slotId) {
            slot.actionId = std::string(actionId);
        }
    }
}

std::vector<QWidget*> MainToolbar::focusOrder() const {
    std::vector<QWidget*> order;
    // Workbench integrations insert their profile and target combos dynamically. Walk the
    // toolbar's actual action order so keyboard focus follows the controls currently displayed.
    for (QAction* action : actions()) {
        QWidget* candidate = widgetForAction(action);
        if (candidate != nullptr && candidate->isVisible() && candidate->isEnabled() &&
            (candidate->focusPolicy() & Qt::TabFocus) != 0) {
            order.push_back(candidate);
        }
    }
    return order;
}

void MainToolbar::actionEvent(QActionEvent* event) {
    QToolBar::actionEvent(event);
    if (event->type() != QEvent::ActionAdded && event->type() != QEvent::ActionRemoved) {
        return;
    }
    // insertWidget() can add controls after construction. Defer until Qt has associated the
    // widget with its action; QObject ownership cancels this callback on destruction.
    QTimer::singleShot(0, this, [this] { updateTabOrder(); });
}

void MainToolbar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), themeColor(ColorKey::MainWindowBackground, palette().color(QPalette::Window)));
    painter.fillRect(QRect(0, height() - 1, width(), 1),
                     themeColor(ColorKey::SeparatorColor, palette().color(QPalette::Mid)));
}

void MainToolbar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (QWidget* top = window(); top != nullptr && top->isWindow()) {
            m_dragStartGlobal = event->globalPosition().toPoint();
            m_windowStartPos = top->pos();
            m_draggingWindow = true;
            event->accept();
            return;
        }
    }
    QToolBar::mousePressEvent(event);
}

void MainToolbar::mouseMoveEvent(QMouseEvent* event) {
    if (m_draggingWindow && (event->buttons() & Qt::LeftButton) != 0) {
        if (QWidget* top = window(); top != nullptr && top->isWindow()) {
            const QPoint delta = event->globalPosition().toPoint() - m_dragStartGlobal;
            if (delta.manhattanLength() >= QApplication::startDragDistance()) {
                if (QWindow* handle = top->windowHandle(); handle != nullptr && handle->startSystemMove()) {
                    m_draggingWindow = false;
                    event->accept();
                    return;
                }
                if (!top->isMaximized()) {
                    top->move(m_windowStartPos + delta);
                }
                event->accept();
                return;
            }
        }
    }
    QToolBar::mouseMoveEvent(event);
}

void MainToolbar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_draggingWindow) {
        m_draggingWindow = false;
        event->accept();
        return;
    }
    QToolBar::mouseReleaseEvent(event);
}

void MainToolbar::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (QWidget* top = window(); top != nullptr && top->isWindow()) {
            if (top->isMaximized()) {
                top->showNormal();
            } else {
                top->showMaximized();
            }
            if (m_maximizeButton != nullptr) {
                m_maximizeButton->update();
            }
            event->accept();
            return;
        }
    }
    QToolBar::mouseDoubleClickEvent(event);
}

void MainToolbar::updateTabOrder() {
    QWidget* previous = nullptr;
    for (QAction* action : actions()) {
        QWidget* candidate = widgetForAction(action);
        if (candidate == nullptr || (candidate->focusPolicy() & Qt::TabFocus) == 0) {
            continue;
        }
        if (previous != nullptr) {
            QWidget::setTabOrder(previous, candidate);
        }
        previous = candidate;
    }
}

void MainToolbar::addSlot(std::string_view actionId, std::string_view iconId, std::string_view fallbackText) {
    auto* button = new QToolButton(this);
    button->setObjectName(QStringLiteral("toolbar.") + shell_detail::fromUtf8(actionId));
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::TabFocus);
    const QIcon icon = shell_detail::themedIcon(iconId);
    if (icon.isNull()) {
        button->setText(text(fallbackText));
    } else {
        button->setIcon(icon);
    }
    button->setToolTip(text(fallbackText));
    button->setEnabled(false);
    const std::string id(actionId);
    // The slot's current action is looked up at click time (setSlotAction may rebind it).
    connect(button, &QToolButton::clicked, this, [this, id] {
        for (const Slot& slot : m_slots) {
            if (slot.slotId == id && m_invoke) {
                m_invoke(slot.actionId);
            }
        }
    });
    addWidget(button);
    m_slots.push_back(Slot{id, id, button});
}

void MainToolbar::refresh(input::ActionRegistry& registry, input::KeymapService& keymap,
                          const input::ActionContext& context) {
    for (Slot& slot : m_slots) {
        const std::optional<input::ActionId> id = registry.find(slot.actionId);
        if (!id) {
            slot.button->setEnabled(false);
            continue;
        }
        QString tooltip = qtr(shell_detail::kActionsContext, registry.info(*id).name);
        if (const std::optional<input::Sequence> shortcut = keymap.primaryShortcut(*id); shortcut) {
            tooltip += QStringLiteral(" (") +
                       shell_detail::fromUtf8(input::toDisplayText(*shortcut, keymap.layout(), keymap.platform())) +
                       QStringLiteral(")");
        }
        slot.button->setToolTip(tooltip);
        slot.button->setEnabled(registry.isEnabled(*id, context));
    }
    const QIcon paletteIcon = shell_detail::themedIcon(icons::kActionsShow);
    if (!paletteIcon.isNull()) {
        m_themeButton->setIcon(paletteIcon);
    }
}

} // namespace frost::ui
