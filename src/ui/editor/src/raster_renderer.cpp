// SPDX-License-Identifier: Apache-2.0
//
// The raster editor renderer (frost/ui/editor/editor_renderer.hpp): QPainter fills and lines, text
// as QGlyphRuns shaped once by QTextLayout and cached per (text, italic), range decorations (wavy
// and dotted squiggles, strike-outs, backgrounds), the error stripe and the chrome shapes; the
// vector marks through display_strokes.hpp, which the QRhi backend rasterises too.

#include "frost/ui/editor/editor_renderer.hpp"

#include "display_strokes.hpp"
#include "editor_font.hpp"
#include <QPaintDevice>
#include <QPainter>
#include <QPen>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>

#include <algorithm>
#include <cmath>

namespace frost::ui {

namespace {

void drawDecoration(QPainter& painter, const DisplayDecoration& decoration) {
    const QRectF& box = decoration.rect;
    switch (decoration.style) {
    case DecorationStyle::Background: painter.fillRect(box, decoration.color); break;
    case DecorationStyle::WavyUnderline:
        detail::strokeWave(painter, box.left(), box.right(), detail::waveCentreY(decoration), decoration.width,
                           decoration.color);
        break;
    case DecorationStyle::DottedUnderline:
        detail::strokeDots(painter, box.left(), box.right(), detail::underlineY(decoration), decoration.width,
                           decoration.color);
        break;
    case DecorationStyle::Underline: {
        const double y = detail::underlineY(decoration);
        painter.setPen(QPen(decoration.color, decoration.width));
        painter.drawLine(QLineF(box.left(), y, box.right(), y));
        break;
    }
    case DecorationStyle::Strikeout: {
        const double y = detail::strikeoutY(decoration);
        painter.setPen(QPen(decoration.color, decoration.width));
        painter.drawLine(QLineF(box.left(), y, box.right(), y));
        break;
    }
    }
}

void drawShape(QPainter& painter, const DisplayShape& shape) {
    painter.setPen(shape.stroke.isValid() ? detail::shapePen(shape.stroke, shape.strokeWidth) : QPen(Qt::NoPen));
    painter.setBrush(shape.fill.isValid() ? QBrush(shape.fill) : QBrush(Qt::NoBrush));
    detail::drawShapeGeometry(painter, shape);
}

} // namespace

RasterEditorRenderer::RasterEditorRenderer(const QFont& font) { setFont(font); }

void RasterEditorRenderer::setFont(const QFont& font) {
    m_font = detail::editorFont(font);
    m_italic = m_font;
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
    // Transparent targets force grayscale coverage in Qt's glyph-run rasterizer. Bound the
    // scratch image to visible ink, with a margin for hinting and antialiasing. Its origin is on
    // the device-pixel grid, so fractional DPR and subpixel glyph positioning are preserved.
    const QPointF topLeft(run.origin.x(), run.origin.y() - entry.ascent);
    QRectF bounds;
    for (const QGlyphRun& glyphs : entry.runs) {
        bounds = bounds.united(glyphs.boundingRect().translated(topLeft));
    }
    const double dpr = painter.device()->devicePixelRatioF();
    QRectF visible = m_viewport;
    if (painter.hasClipping()) {
        visible = visible.intersected(painter.clipBoundingRect());
    }
    const QRectF ink = bounds.adjusted(-4.0, -4.0, 4.0, 4.0).intersected(visible);
    if (ink.isEmpty()) {
        return;
    }
    const int left = static_cast<int>(std::floor(ink.left() * dpr));
    const int top = static_cast<int>(std::floor(ink.top() * dpr));
    const int right = static_cast<int>(std::ceil(ink.right() * dpr));
    const int bottom = static_cast<int>(std::ceil(ink.bottom() * dpr));
    const QSize size(right - left, bottom - top);
    if (size.isEmpty()) {
        return;
    }
    if (m_textScratch.size() != size) {
        m_textScratch = QImage(size, QImage::Format_ARGB32_Premultiplied);
    }
    m_textScratch.setDevicePixelRatio(dpr);
    m_textScratch.fill(Qt::transparent);
    const QPointF origin(left / dpr, top / dpr);
    {
        QPainter scratch(&m_textScratch);
        scratch.setPen(run.color);
        for (const QGlyphRun& glyphs : entry.runs) {
            scratch.drawGlyphRun(topLeft - origin, glyphs);
        }
    }
    painter.drawImage(origin, m_textScratch);
}

void RasterEditorRenderer::render(const EditorDisplayList& list, QPaintDevice& device) {
    QPainter painter(&device);
    m_viewport = list.viewport;
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
        for (const DisplayChangeStripe& stripe : list.changeStripes) {
            painter.fillRect(stripe.rect, stripe.color);
        }
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        for (const DisplayMarker& marker : list.markers) {
            painter.setBrush(marker.color);
            if (marker.severity == MarkerSeverity::RunTest) {
                const QRectF& r = marker.rect;
                const QPointF playTriangle[] = {r.topLeft(), QPointF(r.right(), r.center().y()), r.bottomLeft()};
                painter.drawPolygon(playTriangle, 3);
            } else {
                painter.drawEllipse(marker.rect);
            }
        }
        for (const DisplayBreakpoint& breakpoint : list.breakpoints) {
            if (breakpoint.kind == BreakpointKind::Verified) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(breakpoint.color);
                painter.drawEllipse(breakpoint.rect);
            } else {
                // A hollow ring: the pen is centred on the path, so inset it by half its width.
                const double width = std::max(1.0, breakpoint.rect.width() / 7.0);
                painter.setPen(QPen(breakpoint.color, width));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(breakpoint.rect.adjusted(width / 2.0, width / 2.0, -width / 2.0, -width / 2.0));
            }
        }
        if (list.executionPoint) {
            const QRectF& r = list.executionPoint->rect;
            const QPointF arrow[] = {r.topLeft(), QPointF(r.right(), r.center().y()), r.bottomLeft()};
            painter.setPen(Qt::NoPen);
            painter.setBrush(list.executionPoint->color);
            painter.drawPolygon(arrow, 3);
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
