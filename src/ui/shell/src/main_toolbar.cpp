// SPDX-License-Identifier: Apache-2.0
//
// MainToolbar (frost/ui/shell/main_toolbar.hpp): JetBrains Rider New UI custom themed window
// header with FrostIDE app badge + main menu button, colored project avatar badge, run/build
// configuration controls, quick theme switcher, and integrated window caption buttons.

#include "frost/ui/shell/main_toolbar.hpp"

#include "frost/input/keymap.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/glacial_logo.hpp"
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

#include <algorithm>
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

        // 1. FrostIDE Glacial Crystal logo badge on the left (22x22 rounded tile).
        const int badgeSize = scaledPx(22);
        const int badgeX = scaledPx(6);
        const int badgeY = (height() - badgeSize) / 2;
        const QRectF badgeRect(badgeX, badgeY, badgeSize, badgeSize);
        paintGlacialLogo(painter, badgeRect);

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

// Rider VCS Branch Widget button: crisp Git branch glyph + branch name + dropdown chevron.
class RiderVcsBranchButton final : public QToolButton {
public:
    explicit RiderVcsBranchButton(QWidget* parent = nullptr) : QToolButton(parent) {
        setAutoRaise(true);
        setAttribute(Qt::WA_Hover, true);
    }

    [[nodiscard]] QSize sizeHint() const override {
        const QFontMetrics fm(font());
        const int textWidth = fm.horizontalAdvance(text());
        const int chevronSpace = menu() != nullptr ? scaledPx(16) : scaledPx(6);
        const int w = scaledPx(8) + scaledPx(16) + scaledPx(6) + textWidth + chevronSpace + scaledPx(6);
        return {std::clamp(w, scaledPx(72), scaledPx(220)), scaledPx(30)};
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

        // Crisp Git branch glyph on the left (16x16 box).
        const QColor glyphColor = themeColor(ColorKey::LabelSecondaryForeground, QColor(165, 170, 180));
        painter.setPen(QPen(glyphColor, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);

        const qreal iconLeft = scaledPx(8);
        const qreal cy = height() * 0.5;
        const qreal r = scaledPx(2);
        const qreal x1 = iconLeft + scaledPx(4);
        const qreal x2 = iconLeft + scaledPx(12);
        const qreal yTop = cy - scaledPx(5);
        const qreal yBot = cy + scaledPx(5);
        const qreal yFork = cy + scaledPx(2);

        painter.drawLine(QPointF(x1, yTop + r), QPointF(x1, yBot - r));
        QPainterPath branchPath;
        branchPath.moveTo(x1, yFork);
        branchPath.cubicTo(x1 + (x2 - x1) * 0.6, yFork, x2, yFork - scaledPx(2), x2, yTop + r);
        painter.drawPath(branchPath);

        painter.drawEllipse(QPointF(x1, yTop), r, r);
        painter.drawEllipse(QPointF(x1, yBot), r, r);
        painter.drawEllipse(QPointF(x2, yTop), r, r);

        // Branch name text.
        painter.setFont(font());
        painter.setPen(themeColor(ColorKey::LabelForeground, QColor(220, 223, 228)));

        const int textLeft = scaledPx(8) + scaledPx(16) + scaledPx(6);
        const int chevronWidth = menu() != nullptr ? scaledPx(16) : scaledPx(6);
        const QRect textRect(textLeft, 0, std::max(0, width() - textLeft - chevronWidth - scaledPx(4)), height());
        const QString elided = painter.fontMetrics().elidedText(text(), Qt::ElideRight, textRect.width());
        painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);

        // Dropdown chevron when a VCS menu is attached.
        if (menu() != nullptr) {
            const QColor secondary = themeColor(ColorKey::LabelSecondaryForeground, QColor(155, 160, 170));
            painter.setPen(QPen(secondary, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const qreal cx = width() - scaledPx(11);
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
      m_mainMenu(new QMenu(m_mainMenuButton)), m_backButton(new QToolButton(this)),
      m_forwardButton(new QToolButton(this)), m_project(new RiderProjectButton(this)),
      m_projectMenu(new QMenu(m_project)), m_vcsBranch(new RiderVcsBranchButton(this)),
      m_vcsMenu(new QMenu(m_vcsBranch)), m_buildMenu(new QMenu(this)), m_configurations(new QComboBox(this)),
      m_themeButton(new QToolButton(this)),
      m_minimizeButton(new WindowCaptionButton(WindowCaptionButton::Kind::Minimize, this)),
      m_maximizeButton(new WindowCaptionButton(WindowCaptionButton::Kind::MaximizeRestore, this)),
      m_closeButton(new WindowCaptionButton(WindowCaptionButton::Kind::Close, this)) {
    setObjectName(QStringLiteral("mainToolbar"));
    setWindowTitle(text("Main Toolbar"));
    setMovable(false);
    setFloatable(false);
    setContextMenuPolicy(Qt::PreventContextMenu);
    setContentsMargins(scaledPx(6), scaledPx(3), scaledPx(6), scaledPx(4));
    setMinimumHeight(scaledPx(40));
    setStyleSheet(QStringLiteral("QToolBar#mainToolbar::separator { background: transparent; width: 4px; height: 0px; }"));

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

    // 1b. Rider Header Back / Forward navigation buttons (Qt::NoFocus so Tab focus order stays on dynamic slots).
    m_backButton->setObjectName(QStringLiteral("toolbar.navigate.back"));
    m_backButton->setAutoRaise(true);
    m_backButton->setFocusPolicy(Qt::NoFocus);
    m_backButton->setIcon(shell_detail::themedIcon(icons::kNavBack));
    m_backButton->setToolTip(text("Back"));
    m_backButton->setAccessibleName(text("Back"));
    m_backButton->setEnabled(false);
    connect(m_backButton, &QToolButton::clicked, this, [this] {
        if (m_invoke) {
            m_invoke("navigate.back");
        }
    });
    addWidget(m_backButton);

    m_forwardButton->setObjectName(QStringLiteral("toolbar.navigate.forward"));
    m_forwardButton->setAutoRaise(true);
    m_forwardButton->setFocusPolicy(Qt::NoFocus);
    m_forwardButton->setIcon(shell_detail::themedIcon(icons::kNavForward));
    m_forwardButton->setToolTip(text("Forward"));
    m_forwardButton->setAccessibleName(text("Forward"));
    m_forwardButton->setEnabled(false);
    connect(m_forwardButton, &QToolButton::clicked, this, [this] {
        if (m_invoke) {
            m_invoke("navigate.forward");
        }
    });
    addWidget(m_forwardButton);

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

    // 3. Rider VCS Branch Pill Widget (hidden until a branch name is set).
    m_vcsBranch->setObjectName(QStringLiteral("vcsBranchWidget"));
    m_vcsBranch->setAutoRaise(true);
    m_vcsBranch->setFocusPolicy(Qt::NoFocus);
    m_vcsBranch->setToolTip(text("Current VCS branch"));
    m_vcsMenu->setObjectName(QStringLiteral("vcsBranchMenu"));
    connect(m_vcsMenu, &QMenu::aboutToShow, this, [this] {
        m_vcsMenu->clear();
        if (m_vcsMenuBuilder) {
            m_vcsMenuBuilder(*m_vcsMenu);
        }
    });
    m_vcsAction = addWidget(m_vcsBranch);
    setVcsBranchName({});

    auto* spacer = new QWidget(this);
    spacer->setObjectName(QStringLiteral("toolbarSpacer"));
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    spacer->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    addWidget(spacer);

    m_configurations->setObjectName(QStringLiteral("runConfigurations"));
    m_configurations->setMinimumContentsLength(18);
    m_configurations->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_configurations->setPlaceholderText(text("No configurations"));
    m_configurations->setToolTip(text("Run/Build Configuration"));
    m_configurations->setAccessibleName(text("Run/Build Configuration"));
    m_configurations->setEnabled(false);
    addWidget(m_configurations);
    // The slot is usable as soon as a later slice adds configurations.
    const auto updateCombo = [this] { m_configurations->setEnabled(m_configurations->count() > 0); };
    connect(m_configurations->model(), &QAbstractItemModel::rowsInserted, this, updateCombo);
    connect(m_configurations->model(), &QAbstractItemModel::rowsRemoved, this, updateCombo);
    connect(m_configurations->model(), &QAbstractItemModel::modelReset, this, updateCombo);

    addSeparator();
    addSlot(kBuild, icons::kBuildBuild, "Build");
    m_buildMenu->setObjectName(QStringLiteral("buildToolbarMenu"));
    connect(m_buildMenu, &QMenu::aboutToShow, this, [this] {
        m_buildMenu->clear();
        if (m_buildMenuBuilder) {
            m_buildMenuBuilder(*m_buildMenu);
        }
    });
    addSeparator();
    addSlot(kRun, icons::kRunRun, "Run", IconRole::Success);
    addSlot(kDebug, icons::kRunDebug, "Debug", IconRole::Success);
    addSlot(kStop, icons::kRunStop, "Stop", IconRole::Error);
    auto* utilitySpacer = new QWidget(this);
    utilitySpacer->setObjectName(QStringLiteral("toolbarUtilitySpacer"));
    utilitySpacer->setFixedWidth(scaledPx(12));
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
    update();
    if (QWidget* top = window()) {
        top->update();
    }
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

void MainToolbar::setVcsBranchName(const QString& branchName) {
    const QString trimmed = branchName.trimmed();
    const bool hasBranch = !trimmed.isEmpty();
    m_vcsBranch->setText(trimmed);
    m_vcsBranch->setAccessibleName(hasBranch ? text("VCS Branch: %1").arg(trimmed) : text("VCS Branch"));
    if (m_vcsAction != nullptr) {
        m_vcsAction->setVisible(hasBranch);
    }
    m_vcsBranch->setVisible(hasBranch);
    m_vcsBranch->updateGeometry();
    m_vcsBranch->update();
    updateTabOrder();
}

void MainToolbar::setVcsMenuBuilder(std::function<void(QMenu&)> builder) {
    m_vcsMenu->close();
    m_vcsMenuBuilder = std::move(builder);
    const bool available = static_cast<bool>(m_vcsMenuBuilder);
    m_vcsBranch->setMenu(available ? m_vcsMenu : nullptr);
    m_vcsBranch->setPopupMode(available ? QToolButton::InstantPopup : QToolButton::DelayedPopup);
    m_vcsBranch->setFocusPolicy(available ? Qt::TabFocus : Qt::NoFocus);
    m_vcsBranch->setToolTip(available ? text("VCS branch and operations") : text("Current VCS branch"));
    m_vcsBranch->updateGeometry();
    m_vcsBranch->update();
    updateTabOrder();
}

void MainToolbar::setBuildMenuBuilder(std::function<void(QMenu&)> builder) {
    if (m_buildMenu != nullptr) {
        m_buildMenu->close();
    }
    m_buildMenuBuilder = std::move(builder);
    if (QToolButton* buildBtn = button(kBuild)) {
        const bool available = static_cast<bool>(m_buildMenuBuilder);
        buildBtn->setMenu(available ? m_buildMenu : nullptr);
        buildBtn->setPopupMode(available ? QToolButton::MenuButtonPopup : QToolButton::DelayedPopup);
        buildBtn->updateGeometry();
        buildBtn->update();
    }
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
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QColor baseBg = themeColor(ColorKey::MainWindowBackground, palette().color(QPalette::Window));
    const bool isDark = baseBg.lightness() < 128;
    painter.fillRect(rect(), baseBg);

    // 1. JetBrains Rider Project Gradient: wide horizontal + elliptical frosted color spill from
    // the project avatar badge across the left/center of the main header bar.
    const QString projText = m_project != nullptr ? m_project->text() : QString();
    const QColor projColor = shell_detail::projectAvatarColor(projText);
    const QPointF projCenter = (m_project != nullptr && m_project->isVisible())
                                   ? QPointF(m_project->geometry().center())
                                   : QPointF(scaledPx(140), height() * 0.5);

    const qreal washSpan = std::max<qreal>(scaledPx(480), width() * 0.44);
    QLinearGradient projectWash(0.0, 0.0, washSpan, 0.0);
    const qreal peakStop = std::clamp(projCenter.x() / std::max<qreal>(1.0, washSpan), 0.08, 0.55);
    auto withAlpha = [](QColor c, int a) {
        c.setAlpha(std::clamp(a, 0, 255));
        return c;
    };
    const int washPeak = isDark ? 44 : 26;
    projectWash.setColorAt(0.00, withAlpha(projColor, washPeak / 2));
    projectWash.setColorAt(peakStop, withAlpha(projColor, washPeak));
    projectWash.setColorAt(std::min(1.0, peakStop + 0.25), withAlpha(projColor, washPeak / 2));
    projectWash.setColorAt(1.00, withAlpha(projColor, 0));
    painter.fillRect(QRectF(0.0, 0.0, washSpan, height()), projectWash);

    const int projGlowAlpha = (m_project != nullptr && m_project->underMouse()) ? (isDark ? 88 : 54)
                                                                                : (isDark ? 68 : 42);
    shell_detail::paintEllipticalGlow(painter, projCenter,
                                      std::max<qreal>(scaledPx(280), width() * 0.22),
                                      height() * 2.5, projColor, projGlowAlpha);

    // 2. Frost Brand Badge top-left glacial cyan spill.
    const QPointF menuCenter = (m_mainMenuButton != nullptr && m_mainMenuButton->isVisible())
                                   ? QPointF(m_mainMenuButton->geometry().left() + scaledPx(18), height() * 0.5)
                                   : QPointF(scaledPx(22), height() * 0.5);
    shell_detail::paintEllipticalGlow(painter, menuCenter, scaledPx(95), height() * 1.9,
                                      QColor(0x21, 0xD4, 0xFD), isDark ? 28 : 16);

    // 3. VCS Branch pill subtle cool-indigo spill when visible.
    if (m_vcsBranch != nullptr && m_vcsBranch->isVisible()) {
        const int vcsAlpha = m_vcsBranch->underMouse() ? (isDark ? 46 : 28) : (isDark ? 22 : 14);
        shell_detail::paintEllipticalGlow(painter, QPointF(m_vcsBranch->geometry().center()),
                                          scaledPx(115), height() * 1.8,
                                          QColor(0x5C, 0x7C, 0xFA), vcsAlpha);
    }

    // 4. Run / Debug / Stop cluster ambient color spill into the frosted header.
    for (const Slot& slot : m_slots) {
        if (slot.button == nullptr || !slot.button->isVisible()) {
            continue;
        }
        const QPointF btnCenter(slot.button->geometry().center());
        if (slot.iconRole == IconRole::Success) {
            const int alpha = slot.button->underMouse()
                                  ? (isDark ? 68 : 44)
                                  : (slot.button->isEnabled() ? (isDark ? 38 : 24) : (isDark ? 18 : 10));
            shell_detail::paintEllipticalGlow(painter, btnCenter, scaledPx(82), height() * 1.9,
                                              QColor(0x3F, 0xB9, 0x50), alpha);
        } else if (slot.iconRole == IconRole::Error && slot.button->isEnabled()) {
            const int alpha = slot.button->underMouse() ? (isDark ? 72 : 48) : (isDark ? 46 : 28);
            shell_detail::paintEllipticalGlow(painter, btnCenter, scaledPx(86), height() * 1.9,
                                              QColor(0xE5, 0x53, 0x4B), alpha);
        }
    }

    // 5. Frosted glass surface sheen, top specular highlight, and 1px bottom separator.
    shell_detail::paintFrostedGlassSheen(painter, rect(), isDark, true);
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

void MainToolbar::addSlot(std::string_view actionId, std::string_view iconId, std::string_view fallbackText,
                          std::optional<IconRole> iconRole) {
    auto* button = new QToolButton(this);
    button->setObjectName(QStringLiteral("toolbar.") + shell_detail::fromUtf8(actionId));
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::TabFocus);
    const QIcon icon = shell_detail::themedIcon(iconId, iconRole);
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
    m_slots.push_back(Slot{id, id, std::string(iconId), iconRole, button});
}

void MainToolbar::refresh(input::ActionRegistry& registry, input::KeymapService& keymap,
                          const input::ActionContext& context) {
    for (Slot& slot : m_slots) {
        if (!slot.iconId.empty()) {
            const QIcon slotIcon = shell_detail::themedIcon(slot.iconId, slot.iconRole);
            if (!slotIcon.isNull()) {
                slot.button->setIcon(slotIcon);
            }
        }
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
    const auto refreshNavButton = [&](QToolButton* btn, std::string_view actionId, std::string_view iconId) {
        if (btn == nullptr) {
            return;
        }
        const QIcon navIcon = shell_detail::themedIcon(iconId);
        if (!navIcon.isNull()) {
            btn->setIcon(navIcon);
        }
        if (const std::optional<input::ActionId> id = registry.find(actionId); id) {
            QString tooltip = qtr(shell_detail::kActionsContext, registry.info(*id).name);
            if (const std::optional<input::Sequence> shortcut = keymap.primaryShortcut(*id); shortcut) {
                tooltip += QStringLiteral(" (") +
                           shell_detail::fromUtf8(input::toDisplayText(*shortcut, keymap.layout(), keymap.platform())) +
                           QStringLiteral(")");
            }
            btn->setToolTip(tooltip);
            btn->setEnabled(registry.isEnabled(*id, context));
        } else {
            btn->setEnabled(false);
        }
    };
    refreshNavButton(m_backButton, "navigate.back", icons::kNavBack);
    refreshNavButton(m_forwardButton, "navigate.forward", icons::kNavForward);
}

} // namespace frost::ui
