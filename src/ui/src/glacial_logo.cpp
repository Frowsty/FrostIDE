// SPDX-License-Identifier: Apache-2.0
//
// Glacial FrostIDE brand emblem renderer (frost/ui/glacial_logo.hpp).

#include "frost/ui/glacial_logo.hpp"

#include <QLinearGradient>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QRadialGradient>

#include <algorithm>
#include <array>

namespace frost::ui {

void paintGlacialLogo(QPainter& painter, const QRectF& rect) {
    if (rect.width() <= 0.0 || rect.height() <= 0.0) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qreal side = std::min(rect.width(), rect.height());
    const QRectF box(rect.center().x() - side * 0.5, rect.center().y() - side * 0.5, side, side);
    const auto pt = [&box](qreal nx, qreal ny) {
        return QPointF(box.left() + nx * box.width(), box.top() + ny * box.height());
    };

    // 1. Deep abyssal glacier squircle tile.
    const QRectF tile = box.adjusted(side * 0.03, side * 0.03, -side * 0.03, -side * 0.03);
    const qreal radius = side * 0.22;

    QLinearGradient baseGrad(pt(0.08, 0.04), pt(0.92, 0.96));
    baseGrad.setColorAt(0.0, QColor(0x06, 0x10, 0x1E));
    baseGrad.setColorAt(0.52, QColor(0x0B, 0x1F, 0x3A));
    baseGrad.setColorAt(1.0, QColor(0x13, 0x33, 0x5C));
    painter.setPen(Qt::NoPen);
    painter.setBrush(baseGrad);
    painter.drawRoundedRect(tile, radius, radius);

    // Subtle polar aurora glow in the upper-right quadrant.
    QRadialGradient auroraGrad(pt(0.76, 0.22), side * 0.62);
    auroraGrad.setColorAt(0.0, QColor(0x38, 0xBD, 0xF8, 64));
    auroraGrad.setColorAt(0.55, QColor(0x25, 0x63, 0xEB, 28));
    auroraGrad.setColorAt(1.0, QColor(0x0B, 0x1F, 0x3A, 0));
    painter.setBrush(auroraGrad);
    painter.drawRoundedRect(tile, radius, radius);

    // 2. Multi-faceted hexagonal glacial crystal prism behind the monogram.
    const QPolygonF leftFacet{pt(0.50, 0.10), pt(0.18, 0.29), pt(0.18, 0.71), pt(0.50, 0.90), pt(0.50, 0.50)};
    QLinearGradient leftFacetGrad(pt(0.18, 0.14), pt(0.50, 0.86));
    leftFacetGrad.setColorAt(0.0, QColor(0x38, 0xBD, 0xF8, 78));
    leftFacetGrad.setColorAt(1.0, QColor(0x02, 0x84, 0xC7, 42));
    painter.setBrush(leftFacetGrad);
    painter.drawPolygon(leftFacet);

    const QPolygonF rightFacet{pt(0.50, 0.10), pt(0.82, 0.29), pt(0.82, 0.71), pt(0.50, 0.90), pt(0.50, 0.50)};
    QLinearGradient rightFacetGrad(pt(0.50, 0.14), pt(0.82, 0.86));
    rightFacetGrad.setColorAt(0.0, QColor(0x25, 0x63, 0xEB, 88));
    rightFacetGrad.setColorAt(1.0, QColor(0x4F, 0x46, 0xE5, 52));
    painter.setBrush(rightFacetGrad);
    painter.drawPolygon(rightFacet);

    const QPolygonF crownFacet{pt(0.50, 0.10), pt(0.82, 0.29), pt(0.50, 0.45), pt(0.18, 0.29)};
    QLinearGradient crownGrad(pt(0.50, 0.10), pt(0.50, 0.45));
    crownGrad.setColorAt(0.0, QColor(0xBA, 0xE6, 0xFD, 92));
    crownGrad.setColorAt(1.0, QColor(0x38, 0xBD, 0xF8, 24));
    painter.setBrush(crownGrad);
    painter.drawPolygon(crownFacet);

    if (side >= 20.0) {
        const QPolygonF hexOutline{pt(0.50, 0.10), pt(0.82, 0.29), pt(0.82, 0.71),
                                   pt(0.50, 0.90), pt(0.18, 0.71), pt(0.18, 0.29)};
        painter.setPen(QPen(QColor(0x7D, 0xD3, 0xFC, 72), std::max<qreal>(1.0, side * 0.025), Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(hexOutline);
        painter.setPen(Qt::NoPen);
    }

    // 3. Foreground chiseled ice-shard 'F' monogram.
    // Blade A: Vertical glacial spine with beveled crown and pointed ice base.
    const QPolygonF spineShard{pt(0.26, 0.26), pt(0.34, 0.18), pt(0.42, 0.18),
                               pt(0.42, 0.77), pt(0.33, 0.86), pt(0.26, 0.79)};
    QLinearGradient spineGrad(pt(0.26, 0.18), pt(0.42, 0.86));
    spineGrad.setColorAt(0.0, QColor(0xFF, 0xFF, 0xFF));
    spineGrad.setColorAt(0.48, QColor(0xBA, 0xE6, 0xFD));
    spineGrad.setColorAt(1.0, QColor(0x38, 0xBD, 0xF8));
    painter.setBrush(spineGrad);
    painter.drawPolygon(spineShard);

    // Blade B: Upper arctic wing sweeping to a sharp diagonal ice tip.
    const QPolygonF topWing{pt(0.34, 0.18), pt(0.77, 0.18), pt(0.68, 0.31), pt(0.42, 0.31)};
    QLinearGradient topWingGrad(pt(0.34, 0.18), pt(0.77, 0.31));
    topWingGrad.setColorAt(0.0, QColor(0xFF, 0xFF, 0xFF));
    topWingGrad.setColorAt(0.60, QColor(0xE0, 0xF2, 0xFE));
    topWingGrad.setColorAt(1.0, QColor(0x7D, 0xD3, 0xFC));
    painter.setBrush(topWingGrad);
    painter.drawPolygon(topWing);

    // Blade C: Mid-glacier crossbar shard.
    const QPolygonF midBar{pt(0.42, 0.44), pt(0.68, 0.44), pt(0.60, 0.57), pt(0.42, 0.57)};
    QLinearGradient midBarGrad(pt(0.42, 0.44), pt(0.68, 0.57));
    midBarGrad.setColorAt(0.0, QColor(0xE0, 0xF2, 0xFE));
    midBarGrad.setColorAt(0.55, QColor(0x38, 0xBD, 0xF8));
    midBarGrad.setColorAt(1.0, QColor(0x02, 0x84, 0xC7));
    painter.setBrush(midBarGrad);
    painter.drawPolygon(midBar);

    // 4. Four-point glacial frost star in the lower-right negative space.
    const QPolygonF frostStar{pt(0.64, 0.60), pt(0.665, 0.675), pt(0.74, 0.70), pt(0.665, 0.725),
                              pt(0.64, 0.80), pt(0.615, 0.725), pt(0.54, 0.70), pt(0.615, 0.675)};
    QLinearGradient starGrad(pt(0.54, 0.60), pt(0.74, 0.80));
    starGrad.setColorAt(0.0, QColor(0xFF, 0xFF, 0xFF));
    starGrad.setColorAt(0.5, QColor(0x7D, 0xD3, 0xFC));
    starGrad.setColorAt(1.0, QColor(0x00, 0xE5, 0xFF));
    painter.setBrush(starGrad);
    painter.drawPolygon(frostStar);

    // 5. Specular icy rim border around the squircle tile.
    QLinearGradient rimGrad(tile.topLeft(), tile.bottomRight());
    rimGrad.setColorAt(0.0, QColor(0x7D, 0xD3, 0xFC, 185));
    rimGrad.setColorAt(0.5, QColor(0x38, 0xBD, 0xF8, 120));
    rimGrad.setColorAt(1.0, QColor(0x25, 0x63, 0xEB, 115));
    const qreal rimWidth = std::max<qreal>(1.0, side * 0.032);
    painter.setPen(QPen(QBrush(rimGrad), rimWidth));
    painter.setBrush(Qt::NoBrush);
    const qreal halfRim = rimWidth * 0.5;
    painter.drawRoundedRect(tile.adjusted(halfRim, halfRim, -halfRim, -halfRim), radius - halfRim, radius - halfRim);

    painter.restore();
}

QPixmap glacialLogoPixmap(int logicalSize, qreal devicePixelRatio) {
    const int safeLogical = std::max(1, logicalSize);
    const qreal safeDpr = std::max<qreal>(1.0, devicePixelRatio);
    const int pixelSide = std::max(1, qRound(safeLogical * safeDpr));
    QPixmap pixmap(pixelSide, pixelSide);
    pixmap.setDevicePixelRatio(safeDpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    paintGlacialLogo(painter, QRectF(0.0, 0.0, safeLogical, safeLogical));
    painter.end();
    return pixmap;
}

QIcon glacialLogoIcon() {
    QIcon icon;
    constexpr std::array<int, 7> kSizes{16, 20, 24, 32, 40, 48, 64};
    for (const int size : kSizes) {
        icon.addPixmap(glacialLogoPixmap(size, 1.0));
        icon.addPixmap(glacialLogoPixmap(size, 2.0));
    }
    return icon;
}

} // namespace frost::ui
