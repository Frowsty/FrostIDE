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
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyleOptionToolButton>
#include <QStylePainter>
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

} // namespace

StripeButton::StripeButton(ToolWindowSide side, QWidget* parent) : QToolButton(parent), m_side(side) {
    setCheckable(true);
    setAutoRaise(true);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover, true);
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
}

QSize StripeButton::sizeHint() const {
    if (m_side == ToolWindowSide::Bottom) {
        QSize base = QToolButton::sizeHint();
        base.setHeight(std::max(base.height(), 28));
        return base;
    }
    if (!icon().isNull()) {
        const int extent = std::max(iconSize().width(), iconSize().height()) + 16;
        return {extent, extent};
    }
    const QFontMetrics metrics(font());
    const int iconExtent = icon().isNull() ? 0 : iconSize().width() + 4;
    const int length = metrics.horizontalAdvance(text()) + iconExtent + 16;
    const int thickness = std::max(metrics.height(), iconSize().height()) + 10;
    return {thickness, length};
}

void StripeButton::paintEvent(QPaintEvent* event) {
    const FrostStyle* frost = FrostStyle::current();
    const auto themeColor = [frost](ColorKey key, QColor fallback) {
        return frost != nullptr ? QColor::fromRgba(frost->theme().color(key).argb) : fallback;
    };

    if (m_side == ToolWindowSide::Bottom) {
        QToolButton::paintEvent(event);
        if (isChecked()) {
            QPainter accentPainter(this);
            accentPainter.setRenderHint(QPainter::Antialiasing, true);
            accentPainter.setPen(Qt::NoPen);
            accentPainter.setBrush(themeColor(ColorKey::ComponentFocusColor, QColor(0x35, 0x74, 0xF0)));
            accentPainter.drawRoundedRect(QRectF(6.0, height() - 3.0, std::max(8, width() - 12), 3.0), 1.5, 1.5);
        }
        return;
    }
    QStylePainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QStyleOptionToolButton option;
    initStyleOption(&option);
    if (!icon().isNull()) {
        // The side rail uses upright icons. Its accessible name and tooltip retain the title.
        option.text.clear();
        option.toolButtonStyle = Qt::ToolButtonIconOnly;
        painter.drawComplexControl(QStyle::CC_ToolButton, option);
        if (isChecked()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(themeColor(ColorKey::ComponentFocusColor, QColor(0x35, 0x74, 0xF0)));
            const qreal barX = m_side == ToolWindowSide::Left ? 0.0 : width() - 3.0;
            painter.drawRoundedRect(QRectF(barX, 6.0, 3.0, std::max(8, height() - 12)), 1.5, 1.5);
        }
        return;
    }
    // The panel (hover, checked) unrotated; then the icon and text rotated.
    option.text.clear();
    option.icon = QIcon();
    painter.drawComplexControl(QStyle::CC_ToolButton, option);
    if (isChecked()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(themeColor(ColorKey::ComponentFocusColor, QColor(0x35, 0x74, 0xF0)));
        const qreal barX = m_side == ToolWindowSide::Left ? 0.0 : width() - 3.0;
        painter.drawRoundedRect(QRectF(barX, 6.0, 3.0, std::max(8, height() - 12)), 1.5, 1.5);
    }
    painter.save();
    if (m_side == ToolWindowSide::Left) {
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
        icon().paint(&painter, iconRect, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled,
                     isChecked() ? QIcon::On : QIcon::Off);
        area.setLeft(iconRect.right() + 5);
    }
    painter.setPen(palette().color(isEnabled() ? QPalette::ButtonText : QPalette::PlaceholderText));
    painter.drawText(area, Qt::AlignVCenter | Qt::AlignLeft, text());
    painter.restore();
}

ToolWindowManager::ToolWindowManager(QWidget& host, QWidget& center, IShellSettings& settings, QObject* parent)
    : QObject(parent), m_host(host), m_center(center), m_settings(settings),
      m_horizontal(new QSplitter(Qt::Horizontal, &host)), m_vertical(new QSplitter(Qt::Vertical, m_horizontal)) {
    setObjectName(QStringLiteral("toolWindowManager"));
    m_horizontal->setObjectName(QStringLiteral("toolWindowHorizontalSplitter"));
    m_vertical->setObjectName(QStringLiteral("toolWindowVerticalSplitter"));
    m_horizontal->setChildrenCollapsible(false);
    m_vertical->setChildrenCollapsible(false);
    // Leave a visible gutter between islands while retaining a generous resize target.
    m_horizontal->setHandleWidth(6);
    m_vertical->setHandleWidth(6);
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

    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(side(ToolWindowSide::Left).stripe);
    row->addWidget(m_horizontal, 1);
    row->addWidget(side(ToolWindowSide::Right).stripe);
    auto* outer = new QVBoxLayout(&host);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addLayout(row, 1);
    outer->addWidget(side(ToolWindowSide::Bottom).stripe);

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

    auto* panel = new IslandFrame();
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
    optionsButton->setText(QStringLiteral("⋮"));
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
            connect(optionsMenu->addAction(text("Move to Left")), &QAction::triggered, this,
                    [this, shown] { moveToSide(shown, ToolWindowSide::Left); });
        }
        if (which != ToolWindowSide::Right) {
            connect(optionsMenu->addAction(text("Move to Right")), &QAction::triggered, this,
                    [this, shown] { moveToSide(shown, ToolWindowSide::Right); });
        }
        if (which != ToolWindowSide::Bottom) {
            connect(optionsMenu->addAction(text("Move to Bottom")), &QAction::triggered, this,
                    [this, shown] { moveToSide(shown, ToolWindowSide::Bottom); });
        }
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
    auto* header = new QHBoxLayout();
    header->setContentsMargins(10, 4, 6, 4);
    header->setSpacing(2);
    header->addWidget(s.title, 1);
    header->addWidget(optionsButton);
    header->addWidget(hideButton);
    auto* separator = new QFrame(panel);
    separator->setObjectName(QStringLiteral("toolWindowHeaderSeparator"));
    separator->setFrameShape(QFrame::HLine);
    separator->setFixedHeight(1);
    s.stack = new QStackedWidget(panel);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(separator);
    layout->addWidget(s.stack, 1);

    s.stripe = new QWidget(&m_host);
    s.stripe->setObjectName(QStringLiteral("toolWindowStripe.") + name);
    QBoxLayout* stripeLayout = which == ToolWindowSide::Bottom ? static_cast<QBoxLayout*>(new QHBoxLayout(s.stripe))
                                                               : static_cast<QBoxLayout*>(new QVBoxLayout(s.stripe));
    stripeLayout->setContentsMargins(3, 3, 3, 3);
    stripeLayout->setSpacing(4);
    stripeLayout->addStretch(1);
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
    if (!spec.iconId.empty()) {
        button->setIcon(shell_detail::themedIcon(spec.iconId));
    }
    button->setToolTip(spec.shortcutText.isEmpty() ? spec.title
                                                   : QStringLiteral("%1 (%2)").arg(spec.title, spec.shortcutText));
    auto* stripeLayout = static_cast<QBoxLayout*>(s.stripe->layout());
    stripeLayout->insertWidget(stripeLayout->count() - 1, button);
    s.stripe->show();

    const std::string id = spec.id;
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
    Window& window = m_windows.emplace_back(Window{std::move(spec), button, nullptr, defaultSize, false});
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
    const ToolWindowSide oldSide = window->spec.side;
    const bool wasVisible = window->visible;
    if (wasVisible) {
        setVisible(*window, false, false);
    }
    Side& from = side(oldSide);
    Side& to = side(newSide);
    if (window->content != nullptr) {
        from.stack->removeWidget(window->content);
        to.stack->addWidget(window->content);
    }
    if (window->button != nullptr) {
        from.stripe->layout()->removeWidget(window->button);
        window->button->setParent(to.stripe);
        window->button->setSide(newSide);
        auto* toStripeLayout = static_cast<QBoxLayout*>(to.stripe->layout());
        toStripeLayout->insertWidget(toStripeLayout->count() - 1, window->button);
        window->button->show();
        to.stripe->show();
    }
    window->spec.side = newSide;
    const bool oldHasAny =
        std::ranges::any_of(m_windows, [oldSide](const Window& w) { return w.spec.side == oldSide; });
    if (!oldHasAny) {
        from.stripe->hide();
    }
    if (wasVisible) {
        setVisible(*window, true, true);
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
}

void ToolWindowManager::setVisible(Window& window, bool visible, bool focus) {
    Side& s = side(window.spec.side);
    if (visible) {
        if (!s.shown.empty() && s.shown != window.spec.id) {
            if (Window* previous = find(s.shown); previous != nullptr) {
                previous->visible = false;
                previous->button->setChecked(false);
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
        m_lastActive = window.spec.id;
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
        applySizes();
        if (hadFocus) {
            m_center.setFocus(Qt::OtherFocusReason);
        }
        emit visibilityChanged(shell_detail::fromUtf8(window.spec.id), false);
    }
    storeLayout();
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
