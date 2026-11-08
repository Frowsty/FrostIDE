// SPDX-License-Identifier: Apache-2.0
//
// Helpers shared by the language-feature widgets' translation units (lang_widgets.cpp,
// lang_widgets_panels.cpp and lang_widgets_popups.cpp), moved here unchanged when lang_widgets.cpp
// was split (FND-12a): the item data roles, popup placement, path names and the toggle buttons.
// Private to frost::ui_lang; GUI thread only.

#pragma once

#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QString>
#include <QToolButton>
#include <QWidget>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace frost::ui::lang {

inline constexpr int kDetailRole = Qt::UserRole + 1; // The completion detail drawn right-aligned.
inline constexpr int kItemRole = Qt::UserRole + 2;   // The model item index of a completion row.
inline constexpr int kTargetRole = Qt::UserRole + 3; // The (group, usage) or symbol index of a row.
inline constexpr int kRangeStartRole = Qt::UserRole + 4;
inline constexpr int kRangeEndRole = Qt::UserRole + 5;
inline constexpr int kPathRole = Qt::UserRole + 6;

// Places `widget` (already sized) below `anchor`, or above it when there is no room below,
// inside the anchor's screen.
inline void placeNear(QWidget* widget, const QRect& anchor, bool above) {
    QScreen* screen = QGuiApplication::screenAt(anchor.center());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    const QRect area = screen != nullptr ? screen->availableGeometry() : QRect(0, 0, 4096, 4096);
    const QSize size = widget->size();
    int y = above ? anchor.top() - size.height() - 1 : anchor.bottom() + 2;
    if (!above && y + size.height() > area.bottom()) {
        y = anchor.top() - size.height() - 1;
    } else if (above && y < area.top()) {
        y = anchor.bottom() + 2;
    }
    const int x = std::clamp(anchor.left(), area.left(), std::max(area.left(), area.right() - size.width()));
    widget->move(x, std::max(area.top(), y));
}

inline std::string fileName(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

inline QToolButton* makeToggleToolButton(QWidget* parent, const QString& text, const QString& toolTip, bool checked) {
    auto* btn = new QToolButton(parent);
    btn->setText(text);
    btn->setToolTip(toolTip);
    btn->setCheckable(true);
    btn->setChecked(checked);
    btn->setAutoRaise(true);
    return btn;
}

} // namespace frost::ui::lang
