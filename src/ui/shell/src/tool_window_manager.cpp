// SPDX-License-Identifier: Apache-2.0
//
// ToolWindowManager and StripeButton (frost/ui/shell/tool_window_manager.hpp).
/*
 * Layout inside the host:
 *
 *   [left stripe] [ horizontal splitter: left panel | vertical splitter: center / bottom panel |
 *                   right panel ] [right stripe]
 *   [bottom stripe]
 *
 * Side panels have stretch factor 0 and the centre 1, so resizing the window keeps the panels'
 * sizes; hidden panels take no space. The remembered sizes are the model; the splitters follow it
 * on every show and hide and feed it back after a drag (splitterMoved).
 */

#include "frost/ui/shell/tool_window_manager.hpp"

#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/island_frame.hpp"

#include "shell_util.hpp"
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStackedWidget>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace frost::ui {

using shell_detail::text;

namespace {

bool isValidWindowId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64 || id.front() < 'a' || id.front() > 'z') {
        return false;
    }
    return std::ranges::all_of(
        id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
}

constexpr std::size_t index(ToolWindowSide side) noexcept { return static_cast<std::size_t>(side); }

// Secondary tool windows are unpinned from the vertical stripe by default in Rider New UI and
// appear on the stripe when opened or pinned via the More Tool Windows (⋯) menu.
bool isSecondaryToolWindow(std::string_view id) noexcept {
    static constexpr std::array<std::string_view, 11> kSecondary{
        "fileSystem", "cmakeTargets", "nuget",      "unitTests",   "findUsages", "todo",
        "gitDiff",    "gitConsole",   "cmake",      "frostEngine", "unityControls",
    };
    return std::ranges::find(kSecondary, id) != kSecondary.end();
}

// Canonical Rider vertical stripe order for top-left and bottom-left tool window groups.
int stripeOrderRank(ToolWindowSide side, std::string_view id) noexcept {
    if (side == ToolWindowSide::Left) {
        if (id == "project")
            return 0;
        if (id == "commit")
            return 1;
        if (id == "structure")
            return 2;
        if (id == "fileSystem")
            return 3;
        if (id == "cmakeTargets")
            return 4;
        return 100;
    }
    if (side == ToolWindowSide::Bottom) {
        if (id == "build")
            return 0;
        if (id == "run")
            return 1;
        if (id == "debug")
            return 2;
        if (id == "find")
            return 3;
        if (id == "terminal")
            return 4;
        if (id == "problems")
            return 5;
        if (id == "gitLog")
            return 6;
        if (id == "unitTests")
            return 7;
        if (id == "nuget")
            return 8;
        if (id == "findUsages")
            return 9;
        if (id == "todo")
            return 10;
        if (id == "cmake")
            return 11;
        return 100;
    }
    if (id == "notifications")
        return 0;
    if (id == "asmViewer")
        return 1;
    return 100;
}

class ToolWindowHeaderSeparator final : public QFrame {
public:
    explicit ToolWindowHeaderSeparator(QWidget* parent = nullptr) : QFrame(parent) {
        setFrameShape(QFrame::NoFrame);
        setFixedHeight(1);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const FrostStyle* frost = FrostStyle::current();
        const QColor color = frost != nullptr ? frost->color(ColorKey::SeparatorColor) : palette().color(QPalette::Mid);
        painter.fillRect(rect(), color);
    }
};

class RiderSplitterHandle final : public QSplitterHandle {
public:
    RiderSplitterHandle(Qt::Orientation orientation, QSplitter* parent) : QSplitterHandle(orientation, parent) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const FrostStyle* frost = FrostStyle::current();
        const QColor color = frost != nullptr ? frost->color(ColorKey::SeparatorColor) : palette().color(QPalette::Mid);
        painter.fillRect(rect(), color);
    }
};

class RiderSplitter final : public QSplitter {
public:
    RiderSplitter(Qt::Orientation orientation, QWidget* parent = nullptr) : QSplitter(orientation, parent) {
        setHandleWidth(1);
        setChildrenCollapsible(false);
    }

protected:
    QSplitterHandle* createHandle() override { return new RiderSplitterHandle(orientation(), this); }
};

void applyToolWindowChromePalette(QWidget* root) {
    if (root == nullptr) {
        return;
    }
    const FrostStyle* frost = FrostStyle::current();
    if (frost == nullptr) {
        return;
    }
    const QColor chromeBg = frost->color(ColorKey::MainWindowBackground);
    const auto applyOne = [&chromeBg](QWidget* w) {
        if (w == nullptr || qobject_cast<QLineEdit*>(w) != nullptr || qobject_cast<QComboBox*>(w) != nullptr) {
            return;
        }
        QPalette pal = w->palette();
        pal.setColor(QPalette::Window, chromeBg);
        if (qobject_cast<QAbstractItemView*>(w) != nullptr) {
            pal.setColor(QPalette::Base, chromeBg);
            if (QWidget* vp = static_cast<QAbstractItemView*>(w)->viewport()) {
                QPalette vpPal = vp->palette();
                vpPal.setColor(QPalette::Base, chromeBg);
                vpPal.setColor(QPalette::Window, chromeBg);
                vp->setPalette(vpPal);
            }
        }
        w->setPalette(pal);
    };
    applyOne(root);
    const QList<QWidget*> descendants = root->findChildren<QWidget*>();
    for (QWidget* child : descendants) {
        applyOne(child);
    }
}

class ToolWindowPanel final : public QFrame {
public:
    explicit ToolWindowPanel(QWidget* parent = nullptr) : QFrame(parent) { setFrameShape(QFrame::NoFrame); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const FrostStyle* frost = FrostStyle::current();
        const QColor bg =
            frost != nullptr ? frost->color(ColorKey::MainWindowBackground) : palette().color(QPalette::Window);
        painter.fillRect(rect(), bg);
    }

    void changeEvent(QEvent* event) override {
        if (event != nullptr && event->type() == QEvent::StyleChange) {
            applyToolWindowChromePalette(this);
            update();
        }
        QFrame::changeEvent(event);
    }
};

class ToolWindowHeaderBar final : public QWidget {
public:
    explicit ToolWindowHeaderBar(ToolWindowSide side, QWidget* parent = nullptr)
        : QWidget(parent), m_side(side) {
        setFixedHeight(35);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const FrostStyle* frost = FrostStyle::current();
        const QColor bg =
            frost != nullptr ? frost->color(ColorKey::MainWindowBackground) : palette().color(QPalette::Window);
        const bool isDark = bg.lightness() < 128;
        painter.fillRect(rect(), bg);

        if (m_side == ToolWindowSide::Left) {
            const QColor projColor = shell_detail::activeProjectColor(this);
            shell_detail::paintEllipticalGlow(painter, QPointF(72.0, 0.0), 240.0, height() * 2.4,
                                              projColor, isDark ? 38 : 22);
        } else {
            const QColor accent = frost != nullptr ? frost->color(ColorKey::TabbedPaneUnderlineColor)
                                                   : QColor(0x35, 0x74, 0xF0);
            shell_detail::paintEllipticalGlow(painter, QPointF(56.0, height() * 0.5), 170.0, height() * 2.0,
                                              accent, isDark ? 26 : 16);
        }
        shell_detail::paintFrostedGlassSheen(painter, rect(), isDark, true);
    }

private:
    ToolWindowSide m_side;
};

class ToolWindowSidebar final : public QWidget {
public:
    ToolWindowSidebar(bool borderOnRight, QWidget* parent = nullptr)
        : QWidget(parent), m_borderOnRight(borderOnRight) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const FrostStyle* frost = FrostStyle::current();
        const QColor bg =
            frost != nullptr ? frost->color(ColorKey::MainWindowBackground) : palette().color(QPalette::Window);
        const QColor sep = frost != nullptr ? frost->color(ColorKey::SeparatorColor) : palette().color(QPalette::Mid);
        const QColor accent =
            frost != nullptr ? frost->color(ColorKey::TabbedPaneUnderlineColor) : QColor(0x35, 0x74, 0xF0);
        const bool isDark = bg.lightness() < 128;
        painter.fillRect(rect(), bg);

        // Top ambient color continuity from the Main Header's Project Gradient.
        if (m_borderOnRight) {
            const QColor projColor = shell_detail::activeProjectColor(this);
            shell_detail::paintEllipticalGlow(painter, QPointF(width() * 0.5, 0.0), width() * 2.4, 130.0,
                                              projColor, isDark ? 38 : 22);
        }

        // Ambient frosted glow behind active (checked) and hovered stripe buttons.
        const QList<QToolButton*> buttons = findChildren<QToolButton*>();
        for (const QToolButton* btn : buttons) {
            if (btn == nullptr || !btn->isVisible()) {
                continue;
            }
            const bool checked = btn->isCheckable() && btn->isChecked();
            const bool hovered = btn->underMouse() && btn->isEnabled();
            if (!checked && !hovered) {
                continue;
            }
            const QPoint centerInSidebar = mapFromGlobal(btn->mapToGlobal(btn->rect().center()));
            const int alpha = checked ? (isDark ? 56 : 36) : (isDark ? 30 : 18);
            shell_detail::paintEllipticalGlow(painter, QPointF(centerInSidebar), width() * 1.4, 38.0,
                                              accent, alpha);
        }

        if (m_borderOnRight) {
            painter.fillRect(QRect(width() - 1, 0, 1, height()), sep);
        } else {
            painter.fillRect(QRect(0, 0, 1, height()), sep);
        }
    }

private:
    bool m_borderOnRight = true;
};

// Rider's More Tool Windows (⋯) button in the Left Sidebar.
class MoreToolWindowsButton final : public QToolButton {
public:
    explicit MoreToolWindowsButton(QWidget* parent = nullptr) : QToolButton(parent) {
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover, true);
        setIconSize(QSize(20, 20));
    }

    [[nodiscard]] QSize sizeHint() const override { return {32, 32}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {32, 32}; }

protected:
    void paintEvent(QPaintEvent*) override {
        const FrostStyle* frost = FrostStyle::current();
        const auto themeColor = [frost](ColorKey key, QColor fallback) {
            return frost != nullptr ? QColor::fromRgba(frost->theme().color(key).argb) : fallback;
        };

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool down = isDown() || (menu() != nullptr && menu()->isVisible());
        const bool hovered = underMouse() && isEnabled();
        if (down || hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(down ? themeColor(ColorKey::ButtonPressedBackground, QColor(70, 74, 82))
                                  : themeColor(ColorKey::ButtonHoverBackground, QColor(58, 61, 68)));
            painter.drawRoundedRect(QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0), 6.0, 6.0);
        }

        // Draw crisp horizontal 3-dots (⋯) glyph matching Rider's More Tool Windows icon.
        const QColor dotColor = themeColor(ColorKey::LabelSecondaryForeground, QColor(165, 170, 180));
        painter.setPen(Qt::NoPen);
        painter.setBrush(dotColor);
        const qreal cy = height() * 0.5;
        const qreal cx = width() * 0.5;
        const qreal r = 1.6;
        for (const qreal dx : {-5.0, 0.0, 5.0}) {
            painter.drawEllipse(QPointF(cx + dx, cy), r, r);
        }
    }
};

} // namespace

StripeButton::StripeButton(ToolWindowSide side, QWidget* parent) : QToolButton(parent), m_side(side) {
    setCheckable(true);
    setAutoRaise(true);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover, true);
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setIconSize(QSize(20, 20));
}

QSize StripeButton::sizeHint() const {
    if (!icon().isNull() && !m_showLabel) {
        return {32, 32};
    }
    const QFontMetrics metrics(font());
    const int iconExtent = icon().isNull() ? 0 : iconSize().width() + 4;
    const int length = metrics.horizontalAdvance(text()) + iconExtent + 18;
    const int thickness = std::max(metrics.height(), iconSize().height()) + 12;
    return {thickness, length};
}

void StripeButton::paintEvent(QPaintEvent* /*event*/) {
    const FrostStyle* frost = FrostStyle::current();
    const auto themeColor = [frost](ColorKey key, QColor fallback) {
        return frost != nullptr ? QColor::fromRgba(frost->theme().color(key).argb) : fallback;
    };

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool hovered = underMouse() && isEnabled();
    const bool checked = isChecked();
    const bool pressed = isDown() && isEnabled();

    const QColor accentColor = themeColor(ColorKey::TabbedPaneUnderlineColor,
                                          themeColor(ColorKey::ComponentFocusColor, QColor(0x35, 0x74, 0xF0)));

    const QRectF pillRect = QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0);
    if (checked) {
        // Rider New UI active tool window button: luminous #3574F0 rounded square with glass rim.
        QLinearGradient pillGrad(pillRect.topLeft(), pillRect.bottomRight());
        const QColor baseAccent = pressed ? accentColor.darker(112) : accentColor;
        pillGrad.setColorAt(0.0, baseAccent.lighter(112));
        pillGrad.setColorAt(1.0, baseAccent.darker(106));
        painter.setPen(QPen(QColor(255, 255, 255, 48), 1.0));
        painter.setBrush(pillGrad);
        painter.drawRoundedRect(pillRect, 6.0, 6.0);
    } else if (pressed) {
        painter.setPen(QPen(QColor(255, 255, 255, 28), 1.0));
        painter.setBrush(themeColor(ColorKey::ButtonPressedBackground, palette().color(QPalette::Midlight)));
        painter.drawRoundedRect(pillRect, 6.0, 6.0);
    } else if (hovered) {
        painter.setPen(QPen(QColor(255, 255, 255, 22), 1.0));
        painter.setBrush(themeColor(ColorKey::ButtonHoverBackground, palette().color(QPalette::AlternateBase)));
        painter.drawRoundedRect(pillRect, 6.0, 6.0);
    }
    if (hasFocus()) {
        painter.setPen(QPen(themeColor(ColorKey::ComponentFocusColor, QColor(0x35, 0x74, 0xF0)), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(pillRect.adjusted(1.0, 1.0, -1.0, -1.0), 5.0, 5.0);
    }

    const QIcon::Mode iconMode = !isEnabled() ? QIcon::Disabled : (checked ? QIcon::Selected : QIcon::Normal);

    if (!icon().isNull() && !m_showLabel) {
        const QSize extent = iconSize();
        const QRect iconRect((width() - extent.width()) / 2, (height() - extent.height()) / 2, extent.width(),
                             extent.height());
        icon().paint(&painter, iconRect, Qt::AlignCenter, iconMode, checked ? QIcon::On : QIcon::Off);
        return;
    }

    painter.save();
    if (m_side != ToolWindowSide::Right) {
        painter.translate(0, height());
        painter.rotate(-90);
    } else {
        painter.translate(width(), 0);
        painter.rotate(90);
    }
    QRect area(0, 0, height(), width());
    area.adjust(8, 0, -8, 0);
    if (!icon().isNull()) {
        const QSize iconExtent = iconSize();
        const QRect iconRect(area.left(), area.top() + (area.height() - iconExtent.height()) / 2, iconExtent.width(),
                             iconExtent.height());
        icon().paint(&painter, iconRect, Qt::AlignCenter, iconMode, checked ? QIcon::On : QIcon::Off);
        area.setLeft(iconRect.right() + 5);
    }
    painter.setPen(!isEnabled() ? themeColor(ColorKey::LabelDisabledForeground, palette().color(QPalette::PlaceholderText))
                   : checked    ? themeColor(ColorKey::ButtonDefaultForeground, QColor(255, 255, 255))
                                : themeColor(ColorKey::LabelForeground, palette().color(QPalette::ButtonText)));
    painter.drawText(area, Qt::AlignVCenter | Qt::AlignLeft, text());
    painter.restore();
}

ToolWindowManager::ToolWindowManager(QWidget& host, QWidget& center, IShellSettings& settings, QObject* parent)
    : QObject(parent), m_host(host), m_center(center), m_settings(settings),
      m_leftSidebar(new ToolWindowSidebar(true, &host)), m_moreButton(new MoreToolWindowsButton(m_leftSidebar)),
      m_horizontal(new RiderSplitter(Qt::Horizontal, &host)),
      m_vertical(new RiderSplitter(Qt::Vertical, m_horizontal)) {
    setObjectName(QStringLiteral("toolWindowManager"));
    m_leftSidebar->setObjectName(QStringLiteral("toolWindowLeftSidebar"));
    m_moreButton->setObjectName(QStringLiteral("moreToolWindowsButton"));
    m_moreButton->setToolTip(text("More Tool Windows"));
    m_moreButton->setAccessibleName(text("More Tool Windows"));
    m_moreButton->setPopupMode(QToolButton::InstantPopup);
    auto* moreMenu = new QMenu(m_moreButton);
    moreMenu->setObjectName(QStringLiteral("moreToolWindowsMenu"));
    connect(moreMenu, &QMenu::aboutToShow, this, [this, moreMenu] {
        moreMenu->clear();
        const auto addSideGroup = [&](ToolWindowSide whichSide) {
            bool addedAny = false;
            for (const Window& w : m_windows) {
                if (w.spec.side != whichSide) {
                    continue;
                }
                const QString label = w.spec.shortcutText.isEmpty()
                                          ? w.spec.title
                                          : QStringLiteral("%1\t%2").arg(w.spec.title, w.spec.shortcutText);
                QAction* act = moreMenu->addAction(w.button != nullptr ? w.button->icon() : QIcon(), label);
                act->setCheckable(true);
                act->setChecked(w.visible);
                const std::string id = w.spec.id;
                connect(act, &QAction::triggered, this, [this, id] { toggle(id); });
                addedAny = true;
            }
            return addedAny;
        };
        if (addSideGroup(ToolWindowSide::Left)) {
            moreMenu->addSeparator();
        }
        if (addSideGroup(ToolWindowSide::Bottom)) {
            moreMenu->addSeparator();
        }
        if (addSideGroup(ToolWindowSide::Right)) {
            moreMenu->addSeparator();
        }
        QAction* showNames = moreMenu->addAction(text("Show Tool Window Names"));
        showNames->setCheckable(true);
        showNames->setChecked(m_showStripeLabels);
        connect(showNames, &QAction::toggled, this, [this](bool checked) { setShowStripeLabels(checked); });
    });
    m_moreButton->setMenu(moreMenu);

    m_horizontal->setObjectName(QStringLiteral("toolWindowHorizontalSplitter"));
    m_vertical->setObjectName(QStringLiteral("toolWindowVerticalSplitter"));
    for (const ToolWindowSide which : {ToolWindowSide::Left, ToolWindowSide::Right, ToolWindowSide::Bottom}) {
        buildSide(which);
    }
    m_horizontal->addWidget(side(ToolWindowSide::Left).panel);
    m_horizontal->addWidget(m_vertical);
    m_horizontal->addWidget(side(ToolWindowSide::Right).panel);
    m_vertical->addWidget(&center);
    m_vertical->addWidget(side(ToolWindowSide::Bottom).panel);
    m_horizontal->setStretchFactor(0, 0);
    m_horizontal->setStretchFactor(1, 1);
    m_horizontal->setStretchFactor(2, 0);
    m_vertical->setStretchFactor(0, 1);
    m_vertical->setStretchFactor(1, 0);

    // Rider New UI Left Sidebar: top-left stripe + More Tool Windows (⋯) + stretch + bottom-left stripe.
    auto* leftSidebarLayout = new QVBoxLayout(m_leftSidebar);
    leftSidebarLayout->setContentsMargins(0, 2, 1, 2);
    leftSidebarLayout->setSpacing(2);
    leftSidebarLayout->addWidget(side(ToolWindowSide::Left).stripe);
    auto* moreRow = new QHBoxLayout();
    moreRow->setContentsMargins(4, 0, 3, 2);
    moreRow->setSpacing(0);
    moreRow->addWidget(m_moreButton, 0, Qt::AlignHCenter);
    leftSidebarLayout->addLayout(moreRow);
    leftSidebarLayout->addStretch(1);
    leftSidebarLayout->addWidget(side(ToolWindowSide::Bottom).stripe);

    auto* islandCanvas = new QHBoxLayout();
    islandCanvas->setContentsMargins(0, 0, 0, 0);
    islandCanvas->setSpacing(0);
    islandCanvas->addWidget(m_horizontal, 1);

    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(m_leftSidebar);
    row->addLayout(islandCanvas, 1);
    row->addWidget(side(ToolWindowSide::Right).stripe);
    auto* outer = new QVBoxLayout(&host);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addLayout(row, 1);

    m_leftSidebar->hide();
    for (Side& each : m_sides) {
        each.panel->hide();
        each.stripe->hide();
    }
    connect(m_horizontal, &QSplitter::splitterMoved, this, [this] { syncSizesFromSplitters(); });
    connect(m_vertical, &QSplitter::splitterMoved, this, [this] { syncSizesFromSplitters(); });
}

ToolWindowManager::~ToolWindowManager() = default;

void ToolWindowManager::buildSide(ToolWindowSide which) {
    Side& s = side(which);
    static constexpr std::array<std::string_view, 3> kNames{"left", "right", "bottom"};
    const QString name = shell_detail::fromUtf8(kNames[index(which)]);

    auto* panel = new ToolWindowPanel();
    panel->setObjectName(QStringLiteral("toolWindowPanel.") + name);
    panel->setFocusPolicy(Qt::StrongFocus);
    s.panel = panel;
    s.title = new QLabel(panel);
    s.title->setObjectName(QStringLiteral("toolWindowTitle"));
    QFont bold = s.title->font();
    bold.setBold(true);
    s.title->setFont(bold);

    auto* optionsButton = new QToolButton(panel);
    optionsButton->setObjectName(QStringLiteral("toolWindowOptions"));
    optionsButton->setAutoRaise(true);
    optionsButton->setFocusPolicy(Qt::NoFocus);
    optionsButton->setToolTip(text("Options"));
    const QIcon moreIcon = shell_detail::themedIcon(icons::kActionsMore);
    if (moreIcon.isNull()) {
        optionsButton->setText(QStringLiteral("⋮"));
    } else {
        optionsButton->setIcon(moreIcon);
    }
    optionsButton->setPopupMode(QToolButton::InstantPopup);
    auto* optionsMenu = new QMenu(optionsButton);
    optionsMenu->setObjectName(QStringLiteral("toolWindowOptionsMenu"));
    connect(optionsMenu, &QMenu::aboutToShow, this, [this, which, optionsMenu] {
        optionsMenu->clear();
        const std::string shown = side(which).shown;
        if (shown.empty()) {
            return;
        }
        connect(optionsMenu->addAction(text("Expand Size (+60 px)")), &QAction::triggered, this,
                [this, shown] { setSize(shown, size(shown) + 60); });
        connect(optionsMenu->addAction(text("Shrink Size (-60 px)")), &QAction::triggered, this,
                [this, shown] { setSize(shown, std::max(120, size(shown) - 60)); });
        connect(optionsMenu->addAction(text("Reset Default Size")), &QAction::triggered, this,
                [this, which, shown] { setSize(shown, which == ToolWindowSide::Bottom ? 220 : 280); });
        optionsMenu->addSeparator();
        if (which != ToolWindowSide::Left) {
            connect(optionsMenu->addAction(text("Move to Left - Top")), &QAction::triggered, this,
                    [this, shown] { moveToSide(shown, ToolWindowSide::Left); });
        }
        if (which != ToolWindowSide::Bottom) {
            connect(optionsMenu->addAction(text("Move to Left - Bottom")), &QAction::triggered, this,
                    [this, shown] { moveToSide(shown, ToolWindowSide::Bottom); });
        }
        if (which != ToolWindowSide::Right) {
            connect(optionsMenu->addAction(text("Move to Right")), &QAction::triggered, this,
                    [this, shown] { moveToSide(shown, ToolWindowSide::Right); });
        }
        optionsMenu->addSeparator();
        QAction* showNames = optionsMenu->addAction(text("Show Tool Window Names"));
        showNames->setCheckable(true);
        showNames->setChecked(m_showStripeLabels);
        connect(showNames, &QAction::toggled, this, [this](bool checked) { setShowStripeLabels(checked); });
        optionsMenu->addSeparator();
        connect(optionsMenu->addAction(text("Hide\tShift+Esc")), &QAction::triggered, this,
                [this, shown] { hide(shown); });
        connect(optionsMenu->addAction(text("Hide All Tool Windows\tCtrl+Shift+F12")), &QAction::triggered, this,
                [this] { hideAll(); });
    });
    optionsButton->setMenu(optionsMenu);

    auto* hideButton = new QToolButton(panel);
    hideButton->setObjectName(QStringLiteral("toolWindowHide"));
    hideButton->setAutoRaise(true);
    hideButton->setFocusPolicy(Qt::NoFocus);
    hideButton->setToolTip(text("Hide (Shift+Esc)"));
    const QIcon minimize = shell_detail::themedIcon(icons::kWindowMinimize);
    if (minimize.isNull()) {
        hideButton->setText(QStringLiteral("–"));
    } else {
        hideButton->setIcon(minimize);
    }
    connect(hideButton, &QToolButton::clicked, this, [this, which] {
        if (const std::string shown = side(which).shown; !shown.empty()) {
            hide(shown);
        }
    });
    auto* headerWidget = new ToolWindowHeaderBar(which, panel);
    auto* header = new QHBoxLayout(headerWidget);
    header->setContentsMargins(12, 4, 6, 4);
    header->setSpacing(4);
    header->addWidget(s.title, 1);
    header->addWidget(optionsButton);
    header->addWidget(hideButton);
    auto* separator = new ToolWindowHeaderSeparator(panel);
    separator->setObjectName(QStringLiteral("toolWindowHeaderSeparator"));
    s.stack = new QStackedWidget(panel);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(headerWidget);
    layout->addWidget(separator);
    layout->addWidget(s.stack, 1);

    if (which == ToolWindowSide::Right) {
        s.stripe = new ToolWindowSidebar(false, &m_host);
    } else {
        s.stripe = new QWidget(m_leftSidebar);
    }
    s.stripe->setObjectName(QStringLiteral("toolWindowStripe.") + name);
    s.stripe->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(s.stripe, &QWidget::customContextMenuRequested, this, [this, stripeWidget = s.stripe](const QPoint& pos) {
        QMenu menu(stripeWidget);
        menu.setObjectName(QStringLiteral("toolWindowStripeContextMenu"));
        QAction* showNames = menu.addAction(text("Show Tool Window Names"));
        showNames->setCheckable(true);
        showNames->setChecked(m_showStripeLabels);
        connect(showNames, &QAction::toggled, this, [this](bool checked) { setShowStripeLabels(checked); });
        menu.exec(stripeWidget->mapToGlobal(pos));
    });
    auto* stripeLayout = new QVBoxLayout(s.stripe);
    if (which == ToolWindowSide::Right) {
        stripeLayout->setContentsMargins(4, 4, 3, 4);
        stripeLayout->setSpacing(4);
        stripeLayout->addStretch(1);
    } else {
        stripeLayout->setContentsMargins(4, 2, 3, 2);
        stripeLayout->setSpacing(4);
    }
}

Result<void> ToolWindowManager::add(ToolWindowSpec spec) {
    if (!isValidWindowId(spec.id)) {
        return fail(ErrorCode::InvalidArgument, "invalid tool window id '" + spec.id + "'");
    }
    if (contains(spec.id)) {
        return fail(ErrorCode::AlreadyExists, "tool window '" + spec.id + "' is already registered");
    }
    Side& s = side(spec.side);
    auto* button = new StripeButton(spec.side, s.stripe);
    button->setObjectName(QStringLiteral("stripeButton.") + shell_detail::fromUtf8(spec.id));
    button->setText(spec.title);
    button->setAccessibleName(spec.title);
    button->setShowLabel(m_showStripeLabels);
    if (!spec.iconId.empty()) {
        button->setIcon(shell_detail::themedIcon(spec.iconId));
    }
    button->setToolTip(spec.shortcutText.isEmpty() ? spec.title
                                                   : QStringLiteral("%1 (%2)").arg(spec.title, spec.shortcutText));
    const std::string id = spec.id;
    button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(button, &QWidget::customContextMenuRequested, this, [this, id, button](const QPoint& pos) {
        Window* w = find(id);
        if (w == nullptr) {
            return;
        }
        QMenu menu(button);
        menu.setObjectName(QStringLiteral("stripeButtonContextMenu"));
        if (w->spec.side != ToolWindowSide::Left) {
            connect(menu.addAction(text("Move to Left - Top")), &QAction::triggered, this,
                    [this, id] { moveToSide(id, ToolWindowSide::Left); });
        }
        if (w->spec.side != ToolWindowSide::Bottom) {
            connect(menu.addAction(text("Move to Left - Bottom")), &QAction::triggered, this,
                    [this, id] { moveToSide(id, ToolWindowSide::Bottom); });
        }
        if (w->spec.side != ToolWindowSide::Right) {
            connect(menu.addAction(text("Move to Right")), &QAction::triggered, this,
                    [this, id] { moveToSide(id, ToolWindowSide::Right); });
        }
        menu.addSeparator();
        QAction* pinAct = menu.addAction(text("Pin to Sidebar"));
        pinAct->setCheckable(true);
        pinAct->setChecked(w->pinned);
        connect(pinAct, &QAction::toggled, this, [this, id](bool checked) {
            if (Window* win = find(id); win != nullptr) {
                win->pinned = checked;
                win->button->setVisible(win->pinned || win->visible);
                updateSidebarVisibility();
            }
        });
        QAction* showNames = menu.addAction(text("Show Tool Window Names"));
        showNames->setCheckable(true);
        showNames->setChecked(m_showStripeLabels);
        connect(showNames, &QAction::toggled, this, [this](bool checked) { setShowStripeLabels(checked); });
        if (w->visible) {
            menu.addSeparator();
            connect(menu.addAction(text("Hide")), &QAction::triggered, this, [this, id] { hide(id); });
        }
        menu.exec(button->mapToGlobal(pos));
    });

    auto* stripeLayout = static_cast<QBoxLayout*>(s.stripe->layout());
    const int newRank = stripeOrderRank(spec.side, spec.id);
    const int maxInsertIndex = spec.side == ToolWindowSide::Right ? stripeLayout->count() - 1 : stripeLayout->count();
    int insertAt = maxInsertIndex;
    for (int i = 0; i < maxInsertIndex; ++i) {
        if (const QWidget* existingWidget = stripeLayout->itemAt(i)->widget(); existingWidget != nullptr) {
            const QString objName = existingWidget->objectName();
            if (objName.startsWith(QStringLiteral("stripeButton."))) {
                const std::string existingId = shell_detail::toUtf8(objName.mid(13));
                if (stripeOrderRank(spec.side, existingId) > newRank) {
                    insertAt = i;
                    break;
                }
            }
        }
    }
    stripeLayout->insertWidget(insertAt, button);

    connect(button, &QToolButton::clicked, this, [this, id] {
        toggle(id);
        // The checked state follows visibility, not the click.
        if (const Window* window = find(id); window != nullptr) {
            window->button->setChecked(window->visible);
        }
    });

    const int defaultSize = spec.defaultSize > 0
                                ? spec.defaultSize
                                : (spec.side == ToolWindowSide::Bottom ? kDefaultBottomSize : kDefaultSideSize);
    const bool pinnedByDefault = !isSecondaryToolWindow(spec.id);
    button->setVisible(pinnedByDefault);
    Window& window = m_windows.emplace_back(Window{std::move(spec), button, nullptr, defaultSize, false, pinnedByDefault});
    updateSidebarVisibility();
    if (const Result<ShellLayout> layout = m_settings.load(); layout) {
        if (const ToolWindowLayout* remembered = layout->find(id); remembered != nullptr) {
            if (remembered->size > 0) {
                window.size = std::max(remembered->size, kMinimumSize);
            }
            if (remembered->visible && side(window.spec.side).shown.empty()) {
                setVisible(window, true, false);
            }
        }
    }
    return {};
}

bool ToolWindowManager::contains(std::string_view id) const { return find(id) != nullptr; }

std::vector<std::string> ToolWindowManager::ids() const {
    std::vector<std::string> out;
    for (const Window& window : m_windows) {
        out.push_back(window.spec.id);
    }
    return out;
}

void ToolWindowManager::show(std::string_view id, bool focus) {
    if (Window* window = find(id); window != nullptr) {
        setVisible(*window, true, focus);
    }
}

void ToolWindowManager::hide(std::string_view id) {
    if (Window* window = find(id); window != nullptr && window->visible) {
        setVisible(*window, false, false);
    }
}

void ToolWindowManager::toggle(std::string_view id) {
    const Window* window = find(id);
    if (window == nullptr) {
        return;
    }
    if (window->visible) {
        hide(id);
    } else {
        show(id, true);
    }
}

void ToolWindowManager::activate(std::string_view id) {
    const Window* window = find(id);
    if (window == nullptr) {
        return;
    }
    if (!window->visible) {
        show(id, true);
    } else if (focusedWindow() != id) {
        show(id, true);
    } else {
        hide(id);
    }
}

void ToolWindowManager::hideActive() {
    std::string target = focusedWindow();
    if (target.empty() && isVisible(m_lastActive)) {
        target = m_lastActive;
    }
    if (!target.empty()) {
        hide(target);
    }
}

void ToolWindowManager::hideAll() {
    std::vector<std::string> hidden;
    for (Window& window : m_windows) {
        if (window.visible) {
            hidden.push_back(window.spec.id);
            setVisible(window, false, false);
        }
    }
    if (!hidden.empty()) {
        m_hiddenByHideAll = std::move(hidden);
    }
}

void ToolWindowManager::jumpToLast() {
    // Right after Hide All, F12 brings back what it hid; otherwise the last active window.
    const bool anyVisible = std::ranges::any_of(m_windows, [](const Window& w) { return w.visible; });
    const std::string last = m_lastActive;
    if (!anyVisible && !m_hiddenByHideAll.empty()) {
        for (const std::string& id : std::exchange(m_hiddenByHideAll, {})) {
            show(id, false);
        }
    }
    if (!last.empty()) {
        show(last, true);
    }
}

bool ToolWindowManager::isVisible(std::string_view id) const {
    const Window* window = find(id);
    return window != nullptr && window->visible;
}

std::string ToolWindowManager::focusedWindow() const {
    const QWidget* focus = QApplication::focusWidget();
    if (focus == nullptr) {
        return {};
    }
    for (const Side& s : m_sides) {
        if (!s.shown.empty() && (s.panel == focus || s.panel->isAncestorOf(focus))) {
            return s.shown;
        }
    }
    return {};
}

QWidget* ToolWindowManager::content(std::string_view id) const {
    const Window* window = find(id);
    return window != nullptr ? window->content : nullptr;
}

void ToolWindowManager::setContent(std::string_view id, QWidget* replacement) {
    Window* window = find(id);
    if (window == nullptr || replacement == nullptr) {
        return;
    }
    Side& s = side(window->spec.side);
    QWidget* old = std::exchange(window->content, replacement);
    s.stack->addWidget(replacement);
    applyToolWindowChromePalette(replacement);
    if (window->visible) {
        s.stack->setCurrentWidget(replacement);
    }
    if (old != nullptr) {
        s.stack->removeWidget(old);
        delete old;
    }
}

int ToolWindowManager::size(std::string_view id) const {
    const Window* window = find(id);
    return window != nullptr ? window->size : 0;
}

void ToolWindowManager::setSize(std::string_view id, int newSize) {
    Window* window = find(id);
    if (window == nullptr) {
        return;
    }
    window->size = std::max(newSize, kMinimumSize);
    if (window->visible) {
        applySizes();
    }
    storeLayout();
}

void ToolWindowManager::moveToSide(std::string_view id, ToolWindowSide newSide) {
    Window* window = find(id);
    if (window == nullptr || window->spec.side == newSide) {
        return;
    }
    const bool wasVisible = window->visible;
    if (wasVisible) {
        setVisible(*window, false, false);
    }
    Side& from = side(window->spec.side);
    Side& to = side(newSide);
    if (window->content != nullptr) {
        from.stack->removeWidget(window->content);
        to.stack->addWidget(window->content);
    }
    window->pinned = true;
    if (window->button != nullptr) {
        from.stripe->layout()->removeWidget(window->button);
        window->button->setParent(to.stripe);
        window->button->setSide(newSide);
        auto* toStripeLayout = static_cast<QBoxLayout*>(to.stripe->layout());
        const int insertAt = newSide == ToolWindowSide::Right ? toStripeLayout->count() - 1 : toStripeLayout->count();
        toStripeLayout->insertWidget(insertAt, window->button);
        window->button->show();
    }
    window->spec.side = newSide;
    updateSidebarVisibility();
    if (wasVisible) {
        setVisible(*window, true, true);
    }
}

void ToolWindowManager::setShowStripeLabels(bool show) {
    if (m_showStripeLabels == show) {
        return;
    }
    m_showStripeLabels = show;
    for (Window& window : m_windows) {
        if (window.button != nullptr) {
            window.button->setShowLabel(show);
        }
    }
    for (Side& s : m_sides) {
        if (s.stripe != nullptr) {
            s.stripe->updateGeometry();
            s.stripe->update();
        }
    }
    if (m_leftSidebar != nullptr) {
        m_leftSidebar->updateGeometry();
        m_leftSidebar->update();
    }
}

void ToolWindowManager::syncSizesFromSplitters() {
    if (m_applying) {
        return;
    }
    const QList<int> horizontal = m_horizontal->sizes();
    const QList<int> vertical = m_vertical->sizes();
    if (Window* left = find(side(ToolWindowSide::Left).shown); left != nullptr && horizontal.size() == 3) {
        left->size = std::max(horizontal[0], kMinimumSize);
    }
    if (Window* right = find(side(ToolWindowSide::Right).shown); right != nullptr && horizontal.size() == 3) {
        right->size = std::max(horizontal[2], kMinimumSize);
    }
    if (Window* bottom = find(side(ToolWindowSide::Bottom).shown); bottom != nullptr && vertical.size() == 2) {
        bottom->size = std::max(vertical[1], kMinimumSize);
    }
    storeLayout();
}

StripeButton* ToolWindowManager::stripeButton(std::string_view id) const {
    const Window* window = find(id);
    return window != nullptr ? window->button : nullptr;
}

QWidget* ToolWindowManager::panel(ToolWindowSide which) const noexcept { return m_sides[index(which)].panel; }

QWidget* ToolWindowManager::stripe(ToolWindowSide which) const noexcept { return m_sides[index(which)].stripe; }

ToolWindowManager::Window* ToolWindowManager::find(std::string_view id) {
    const auto it = std::ranges::find(m_windows, id, [](const Window& w) -> std::string_view { return w.spec.id; });
    return it == m_windows.end() ? nullptr : &*it;
}

const ToolWindowManager::Window* ToolWindowManager::find(std::string_view id) const {
    const auto it = std::ranges::find(m_windows, id, [](const Window& w) -> std::string_view { return w.spec.id; });
    return it == m_windows.end() ? nullptr : &*it;
}

void ToolWindowManager::ensureContent(Window& window) {
    if (window.content != nullptr) {
        return;
    }
    QWidget* created = window.spec.createContent ? window.spec.createContent() : nullptr;
    if (created == nullptr) {
        auto* label = new QLabel(window.spec.title);
        label->setAlignment(Qt::AlignCenter);
        created = label;
    }
    window.content = created;
    side(window.spec.side).stack->addWidget(created);
    applyToolWindowChromePalette(created);
}

void ToolWindowManager::setVisible(Window& window, bool visible, bool focus) {
    Side& s = side(window.spec.side);
    if (visible) {
        if (!s.shown.empty() && s.shown != window.spec.id) {
            if (Window* previous = find(s.shown); previous != nullptr) {
                previous->visible = false;
                previous->button->setChecked(false);
                previous->button->setVisible(previous->pinned || previous->visible);
                emit visibilityChanged(shell_detail::fromUtf8(previous->spec.id), false);
            }
        }
        ensureContent(window);
        s.stack->setCurrentWidget(window.content);
        s.title->setText(window.spec.title);
        const bool changed = !window.visible;
        s.shown = window.spec.id;
        window.visible = true;
        window.button->setChecked(true);
        window.button->setVisible(window.pinned || window.visible);
        m_lastActive = window.spec.id;
        updateSidebarVisibility();
        applySizes();
        if (focus) {
            if (window.content->focusPolicy() != Qt::NoFocus) {
                window.content->setFocus(Qt::OtherFocusReason);
            } else {
                s.panel->setFocus(Qt::OtherFocusReason);
            }
        }
        if (changed) {
            emit visibilityChanged(shell_detail::fromUtf8(window.spec.id), true);
        }
    } else {
        const QWidget* focusWidget = QApplication::focusWidget();
        const bool hadFocus = focusWidget != nullptr && (s.panel == focusWidget || s.panel->isAncestorOf(focusWidget));
        if (s.shown == window.spec.id) {
            s.shown.clear();
        }
        window.visible = false;
        window.button->setChecked(false);
        window.button->setVisible(window.pinned || window.visible);
        updateSidebarVisibility();
        applySizes();
        if (hadFocus) {
            m_center.setFocus(Qt::OtherFocusReason);
        }
        emit visibilityChanged(shell_detail::fromUtf8(window.spec.id), false);
    }
    storeLayout();
}

void ToolWindowManager::updateSidebarVisibility() {
    for (const ToolWindowSide which : {ToolWindowSide::Left, ToolWindowSide::Bottom, ToolWindowSide::Right}) {
        const bool anyOnSide = std::ranges::any_of(
            m_windows, [which](const Window& w) { return w.spec.side == which && (w.pinned || w.visible); });
        side(which).stripe->setVisible(anyOnSide);
    }
    if (m_leftSidebar != nullptr) {
        const bool anyLeftOrBottom = !m_windows.empty() &&
                                     std::ranges::any_of(m_windows, [](const Window& w) {
                                         return w.spec.side == ToolWindowSide::Left ||
                                                w.spec.side == ToolWindowSide::Bottom;
                                     });
        m_leftSidebar->setVisible(anyLeftOrBottom);
    }
}

void ToolWindowManager::applySizes() {
    m_applying = true;
    const Window* left = find(side(ToolWindowSide::Left).shown);
    const Window* right = find(side(ToolWindowSide::Right).shown);
    const Window* bottom = find(side(ToolWindowSide::Bottom).shown);
    side(ToolWindowSide::Left).panel->setVisible(left != nullptr);
    side(ToolWindowSide::Right).panel->setVisible(right != nullptr);
    side(ToolWindowSide::Bottom).panel->setVisible(bottom != nullptr);

    const int width = std::max(m_horizontal->width(), 800);
    const int leftSize = left != nullptr ? left->size : 0;
    const int rightSize = right != nullptr ? right->size : 0;
    m_horizontal->setSizes({leftSize, std::max(width - leftSize - rightSize, kMinimumSize), rightSize});
    const int height = std::max(m_vertical->height(), 500);
    const int bottomSize = bottom != nullptr ? bottom->size : 0;
    m_vertical->setSizes({std::max(height - bottomSize, kMinimumSize), bottomSize});
    m_applying = false;
}

void ToolWindowManager::storeLayout() {
    Result<ShellLayout> loaded = m_settings.load();
    ShellLayout layout = loaded ? std::move(*loaded) : ShellLayout{};
    for (const Window& window : m_windows) {
        ToolWindowLayout& entry = layout.entry(window.spec.id);
        entry.visible = window.visible;
        entry.size = window.size;
    }
    if (const Result<void> stored = m_settings.store(layout); !stored) {
        if (!m_storeFailed) {
            m_storeFailed = true;
            emit storeFailed(shell_detail::fromUtf8(stored.error().message()));
        }
    } else {
        m_storeFailed = false;
    }
}

} // namespace frost::ui
