// SPDX-License-Identifier: Apache-2.0
//
// Device-pixel scale and snapping shared by FrostStyle, IslandFrame and FrostIconEngine
// (internal to frost::ui). At fractional scales a widget's logical origin can fall between device pixels,
// and QWidget::render (fractional painter translation) and the backing store (whole device
// pixel offsets) would then antialias the same edge differently. Snapping each drawn shape's
// edges to device pixels makes borders and icons crisp and both render paths identical.
#pragma once

#include <QPaintDevice>
#include <QPainter>
#include <QPointF>
#include <QRectF>
#include <QTransform>

#include <cmath>

namespace frost::ui::detail {

// The painter's logical-to-device-pixel transform. QPainter::deviceTransform() includes the
// device's pixel ratio (checked by FrostStyle.BordersSnapToDevicePixels).
[[nodiscard]] inline QTransform toDevicePixels(const QPainter& painter) { return painter.deviceTransform(); }

/*
 * Device pixels per logical pixel for `painter`: the scale of its device transform. This is the
 * dpr every drawing path uses. painter.device()->devicePixelRatio() is not: QWidget::render into
 * a QImage redirects the painter to the widget, whose ratio is its screen's, while the device
 * transform carries the target's scale. Falls back to the device's ratio for a rotated or
 * sheared painter. O(1).
 */
[[nodiscard]] inline qreal devicePixelScale(const QPainter& painter) {
    const QTransform transform = toDevicePixels(painter);
    if (transform.type() <= QTransform::TxScale && transform.m11() > 0.0) {
        return transform.m11();
    }
    return painter.device() != nullptr ? painter.device()->devicePixelRatio() : 1.0;
}

/*
 * `rect` (painter coordinates) with each edge moved to the nearest device-pixel boundary. Only
 * translation and scale transforms are snapped; a rotation or shear returns `rect` unchanged.
 * O(1), no allocation.
 */
[[nodiscard]] inline QRectF snapToDevicePixels(const QPainter& painter, const QRectF& rect) {
    const QTransform transform = toDevicePixels(painter);
    if (transform.type() > QTransform::TxScale) {
        return rect;
    }
    bool invertible = false;
    const QTransform inverse = transform.inverted(&invertible);
    if (!invertible) {
        return rect;
    }
    const QRectF device = transform.mapRect(rect);
    const QRectF snapped(QPointF(std::round(device.left()), std::round(device.top())),
                         QPointF(std::round(device.right()), std::round(device.bottom())));
    return inverse.mapRect(snapped);
}

// `point` moved to the nearest device-pixel corner; as snapToDevicePixels(rect).
[[nodiscard]] inline QPointF snapToDevicePixels(const QPainter& painter, QPointF point) {
    const QTransform transform = toDevicePixels(painter);
    if (transform.type() > QTransform::TxScale) {
        return point;
    }
    bool invertible = false;
    const QTransform inverse = transform.inverted(&invertible);
    if (!invertible) {
        return point;
    }
    const QPointF device = transform.map(point);
    return inverse.map(QPointF(std::round(device.x()), std::round(device.y())));
}

} // namespace frost::ui::detail
