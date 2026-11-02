// SPDX-License-Identifier: Apache-2.0
//
// The raster editor renderer (frost/ui/editor/editor_renderer.hpp): QPainter fills and lines, text
// as QGlyphRuns shaped once by QTextLayout and cached per (text, italic).

#include "frost/ui/editor/editor_renderer.hpp"

#include <QPaintDevice>
#include <QPainter>
#include <QPen>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>

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
