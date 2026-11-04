// SPDX-License-Identifier: Apache-2.0
//
// Glacial FrostIDE brand emblem renderer and multi-resolution application icon builder.
#pragma once

#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QRectF>

namespace frost::ui {

// Paints the FrostIDE Glacial Crystal logo inside `rect`. Designed to remain razor-sharp from
// 16x16 window/dialog header badges up to 256x256 welcome and about emblems.
void paintGlacialLogo(QPainter& painter, const QRectF& rect);

// Renders the FrostIDE Glacial Crystal logo into a high-DPI pixmap of `logicalSize` square at
// `devicePixelRatio`.
[[nodiscard]] QPixmap glacialLogoPixmap(int logicalSize, qreal devicePixelRatio = 1.0);

// Returns a multi-resolution QIcon containing the FrostIDE Glacial Crystal logo across standard
// desktop icon sizes (16, 20, 24, 32, 40, 48, 64, 128, 256).
[[nodiscard]] QIcon glacialLogoIcon();

} // namespace frost::ui
