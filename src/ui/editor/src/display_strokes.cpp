// SPDX-License-Identifier: Apache-2.0
//
// The editor backends' shared vector drawing (display_strokes.hpp): squiggles, dotted lines and
// chrome shapes as QPainter calls.

#include "display_strokes.hpp"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace frost::ui::detail {

double underlineY(const DisplayDecoration& decoration) noexcept { return decoration.baseline + 1.5 * decoration.width; }

double waveCentreY(const DisplayDecoration& decoration) noexcept {
    return underlineY(decoration) + waveAmplitude(decoration.width);
}

double strikeoutY(const DisplayDecoration& decoration) noexcept {
    return decoration.baseline - (decoration.baseline - decoration.rect.top()) * 0.33;
}

void strokeWave(QPainter& painter, double left, double right, double y, double width, const QColor& color) {
    const double step = 2.0 * width;
    const double amplitude = waveAmplitude(width);
    QPainterPath path(QPointF(left, y));
    bool up = true;
    for (double x = left; x < right; x += step) {
        const double next = std::min(x + step, right);
        path.lineTo(next, up ? y - amplitude : y + amplitude);
        up = !up;
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
}

void strokeDots(QPainter& painter, double left, double right, double y, double width, const QColor& color) {
    painter.setPen(QPen(color, width, Qt::DotLine, Qt::FlatCap));
    painter.drawLine(QLineF(left, y, right, y));
}

QPen shapePen(const QColor& color, double strokeWidth) {
    return QPen(color, strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

void drawShapeGeometry(QPainter& painter, const DisplayShape& shape) {
    switch (shape.kind) {
    case DisplayShape::Kind::Rect:
        if (shape.radius > 0.0) {
            painter.drawRoundedRect(shape.rect, shape.radius, shape.radius);
        } else {
            painter.drawRect(shape.rect);
        }
        break;
    case DisplayShape::Kind::Ellipse: painter.drawEllipse(shape.rect); break;
    case DisplayShape::Kind::Polygon: painter.drawPolygon(shape.points); break;
    case DisplayShape::Kind::Polyline:
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(shape.points);
        break;
    }
}

} // namespace frost::ui::detail
