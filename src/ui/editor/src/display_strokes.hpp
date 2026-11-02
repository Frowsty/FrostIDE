// SPDX-License-Identifier: Apache-2.0
//
// The editor backends' shared vector drawing (ADR 0043 "Parity"); not part of frost::ui_editor's
// interface. The raster backend draws squiggles, dotted underlines and chrome shapes with these
// QPainter helpers, and the QRhi backend rasterises the same calls (in white, in device pixels)
// into its atlas's coverage cells, so both draw identical marks by construction.
#pragma once

#include "frost/ui/editor/display_list.hpp"

#include <QColor>
#include <QPen>

class QPainter;

namespace frost::ui::detail {

// The y of a decoration's straight and dotted underline (1.5 strokes below the baseline).
[[nodiscard]] double underlineY(const DisplayDecoration& decoration) noexcept;
// The y of a wavy underline's centre line (one amplitude below underlineY).
[[nodiscard]] double waveCentreY(const DisplayDecoration& decoration) noexcept;
// The y of a strike-out: through the middle of the x-height (a third of the ascent above the baseline).
[[nodiscard]] double strikeoutY(const DisplayDecoration& decoration) noexcept;

// The squiggle repeats every wavePeriod pixels after its first half-period; the dots every dotPeriod.
[[nodiscard]] constexpr double wavePeriod(double width) noexcept { return 4.0 * width; }
[[nodiscard]] constexpr double dotPeriod(double width) noexcept { return 3.0 * width; }
// The squiggle's amplitude: it spans y +- amplitude around its centre line.
[[nodiscard]] constexpr double waveAmplitude(double width) noexcept { return width; }

/*
 * Rider's squiggle from `left` to `right` around centre line `y`: a polyline that starts on the
 * centre line and alternates between y - amplitude and y + amplitude every 2 * width pixels,
 * stroked `width` wide with round caps and joins. Uses no brush; leaves the pen set.
 */
void strokeWave(QPainter& painter, double left, double right, double y, double width, const QColor& color);
// A dotted line (Qt::DotLine: dots of `width`, gaps of 2 * width, flat caps) from left to right at y.
void strokeDots(QPainter& painter, double left, double right, double y, double width, const QColor& color);

// The pen a chrome shape is stroked with (round caps and joins), in `color`.
[[nodiscard]] QPen shapePen(const QColor& color, double strokeWidth);
// Draws `shape`'s geometry (a rect with its corner radius, an ellipse, a polygon or a polyline)
// with the painter's current pen and brush; a polyline drops the brush first.
void drawShapeGeometry(QPainter& painter, const DisplayShape& shape);

} // namespace frost::ui::detail
