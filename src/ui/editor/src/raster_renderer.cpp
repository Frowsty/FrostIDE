// SPDX-License-Identifier: Apache-2.0
//
// The raster editor renderer (frost/ui/editor/editor_renderer.hpp): QPainter fills and lines, text
// as QGlyphRuns shaped once by QTextLayout and cached per (text, italic).

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

namespace {

// A diagnostic underline: a straight line, or a zigzag of half-period 2 * width and amplitude width
// along `decoration.line` (left to right; horizontal lines only).
void drawDecoration(QPainter& painter, const DisplayDecoration& decoration) {
    painter.setPen(QPen(decoration.color, decoration.width));
    if (decoration.style == UnderlineStyle::Straight) {
        painter.drawLine(decoration.line);
        return;
    }
    const double step = 2.0 * decoration.width;
    const double amplitude = decoration.width;
    const double y = decoration.line.y1();
    const double x0 = std::min(decoration.line.x1(), decoration.line.x2());
    const double x1 = std::max(decoration.line.x1(), decoration.line.x2());
    QPainterPath path(QPointF(x0, y));
    bool up = true;
    for (double x = x0 + step; x < x1 + step; x += step) {
        path.lineTo(QPointF(std::min(x, x1), up ? y - amplitude : y + amplitude));
        up = !up;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
    painter.restore();
}

} // namespace

void RasterEditorRenderer::render(const EditorDisplayList& list, QPaintDevice& device) {
    QPainter painter(&device);
    painter.fillRect(list.viewport, list.background);
    for (const DisplayFill& fill : list.fills) {
        painter.fillRect(fill.rect, fill.color);
    }
    painter.save();
    painter.setClipRect(list.textArea);
    for (const DisplayLine& line : list.underlines) {
        painter.setPen(QPen(line.color, line.width));
        painter.drawLine(line.line);
    }
    for (const DisplayText& run : list.text) {
        drawText(painter, run);
    }
    for (const DisplayDecoration& decoration : list.decorations) {
        drawDecoration(painter, decoration);
    }
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
    }
}

} // namespace frost::ui
