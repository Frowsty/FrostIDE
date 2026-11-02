// SPDX-License-Identifier: Apache-2.0
//
// The raster editor renderer (frost/ui/editor/editor_renderer.hpp): QPainter fills and lines, text
// as QGlyphRuns shaped once by QTextLayout and cached per (text, italic), range decorations (wavy
// and dotted squiggles, strike-outs, backgrounds), the error stripe and the chrome shapes.

#include "frost/ui/editor/editor_renderer.hpp"

#include <QPaintDevice>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>

#include <algorithm>

namespace frost::ui {

namespace {

// A wave along the decorated box's underline: half-periods of 2 * width pixels, amplitude `width`.
void drawWave(QPainter& painter, const DisplayDecoration& decoration) {
    const double step = 2.0 * decoration.width;
    const double amplitude = decoration.width;
    const double y = decoration.baseline + 1.5 * decoration.width + amplitude;
    QPainterPath path(QPointF(decoration.rect.left(), y));
    bool up = true;
    for (double x = decoration.rect.left(); x < decoration.rect.right(); x += step) {
        const double next = std::min(x + step, decoration.rect.right());
        path.lineTo(next, up ? y - amplitude : y + amplitude);
        up = !up;
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(decoration.color, decoration.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
}

void drawDecoration(QPainter& painter, const DisplayDecoration& decoration) {
    const QRectF& box = decoration.rect;
    const double underline = decoration.baseline + 1.5 * decoration.width;
    switch (decoration.style) {
    case DecorationStyle::Background: painter.fillRect(box, decoration.color); break;
    case DecorationStyle::WavyUnderline: drawWave(painter, decoration); break;
    case DecorationStyle::DottedUnderline: {
        QPen pen(decoration.color, decoration.width, Qt::DotLine, Qt::FlatCap);
        painter.setPen(pen);
        painter.drawLine(QLineF(box.left(), underline, box.right(), underline));
        break;
    }
    case DecorationStyle::Underline:
        painter.setPen(QPen(decoration.color, decoration.width));
        painter.drawLine(QLineF(box.left(), underline, box.right(), underline));
        break;
    case DecorationStyle::Strikeout: {
        // Through the middle of the x-height (about a third of the ascent above the baseline).
        const double y = decoration.baseline - (decoration.baseline - box.top()) * 0.33;
        painter.setPen(QPen(decoration.color, decoration.width));
        painter.drawLine(QLineF(box.left(), y, box.right(), y));
        break;
    }
    }
}

void drawShape(QPainter& painter, const DisplayShape& shape) {
    painter.setPen(shape.stroke.isValid()
                       ? QPen(shape.stroke, shape.strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)
                       : QPen(Qt::NoPen));
    painter.setBrush(shape.fill.isValid() ? QBrush(shape.fill) : QBrush(Qt::NoBrush));
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

} // namespace

RasterEditorRenderer::RasterEditorRenderer(const QFont& font) { setFont(font); }

void RasterEditorRenderer::setFont(const QFont& font) {
    m_font = font;
    m_italic = font;
    m_italic.setItalic(true);
    m_regularCache.clear();
    m_italicCache.clear();
}

GlyphCacheStats RasterEditorRenderer::cacheStats() const noexcept {
    return GlyphCacheStats{m_hits, m_misses, static_cast<std::uint64_t>(m_regularCache.size() + m_italicCache.size())};
}

const RasterEditorRenderer::Shaped& RasterEditorRenderer::shaped(const QString& text, bool italic) {
    QHash<QString, Shaped>& cache = italic ? m_italicCache : m_regularCache;
    if (const auto it = cache.constFind(text); it != cache.constEnd()) {
        ++m_hits;
        return *it;
    }
    ++m_misses;
    if (static_cast<std::uint64_t>(cache.size()) >= kMaxEntries) {
        cache.clear();
    }
    QTextLayout layout(text, italic ? m_italic : m_font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    layout.endLayout();
    Shaped entry;
    if (line.isValid()) {
        entry.ascent = line.ascent();
        entry.runs = layout.glyphRuns();
    }
    return *cache.insert(text, std::move(entry));
}

void RasterEditorRenderer::drawText(QPainter& painter, const DisplayText& run) {
    const Shaped& entry = shaped(run.text, run.italic);
    painter.setPen(run.color);
    // QTextLayout places the first line's top at y = 0, so its baseline is at y = ascent.
    const QPointF topLeft(run.origin.x(), run.origin.y() - entry.ascent);
    for (const QGlyphRun& glyphs : entry.runs) {
        painter.drawGlyphRun(topLeft, glyphs);
    }
}

void RasterEditorRenderer::render(const EditorDisplayList& list, QPaintDevice& device) {
    QPainter painter(&device);
    painter.fillRect(list.viewport, list.background);
    for (const DisplayFill& fill : list.fills) {
        painter.fillRect(fill.rect, fill.color);
    }
    painter.save();
    painter.setClipRect(list.textArea);
    for (const DisplayDecoration& decoration : list.decorations) {
        if (decoration.style == DecorationStyle::Background) {
            drawDecoration(painter, decoration);
        }
    }
    for (const DisplayLine& line : list.underlines) {
        painter.setPen(QPen(line.color, line.width));
        painter.drawLine(line.line);
    }
    for (const DisplayText& run : list.text) {
        drawText(painter, run);
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const DisplayDecoration& decoration : list.decorations) {
        if (decoration.style != DecorationStyle::Background) {
            drawDecoration(painter, decoration);
        }
    }
    painter.setRenderHint(QPainter::Antialiasing, false);
    for (const DisplayLine& line : list.overlays) {
        painter.setPen(QPen(line.color, line.width));
        painter.drawLine(line.line);
    }
    for (const QRectF& caret : list.carets) {
        painter.fillRect(caret, list.caretColor);
    }
    painter.restore();
    if (!list.gutter.isEmpty()) {
        painter.fillRect(list.gutter, list.gutterBackground);
        for (const DisplayText& run : list.gutterText) {
            drawText(painter, run);
        }
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        for (const DisplayMarker& marker : list.markers) {
            painter.setBrush(marker.color);
            painter.drawEllipse(marker.rect);
        }
        painter.setRenderHint(QPainter::Antialiasing, false);
    }
    if (!list.stripe.isEmpty()) {
        painter.fillRect(list.stripe, list.stripeBackground);
        for (const DisplayStripeMark& mark : list.stripeMarks) {
            painter.fillRect(mark.rect, mark.color);
        }
        if (!list.stripeSummary.isEmpty()) {
            painter.fillRect(list.stripeSummary, list.stripeSummaryColor);
        }
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const DisplayShape& shape : list.chrome) {
        drawShape(painter, shape);
    }
    for (const DisplayText& run : list.chromeText) {
        drawText(painter, run);
    }
}

} // namespace frost::ui
