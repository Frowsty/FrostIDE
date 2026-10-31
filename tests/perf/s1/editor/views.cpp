// SPDX-License-Identifier: Apache-2.0
//
// The backend-independent parts of the S1 views (font grid, visible lines, glyph
// atlas, quad building) and the raster view: QPainter over a per-line QGlyphRun
// cache. See views.hpp.

#include "views.hpp"

#include "frost/perf/frame_marker.hpp"
#include "frost/perf/win/timing.hpp"

#include <QPaintEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace frost::s1 {

namespace {

constexpr int kMarkerCellPx = 4;

std::uint32_t packRgba(const QColor& color) {
    return static_cast<std::uint32_t>(color.red()) | (static_cast<std::uint32_t>(color.green()) << 8U) |
           (static_cast<std::uint32_t>(color.blue()) << 16U) | (static_cast<std::uint32_t>(color.alpha()) << 24U);
}

} // namespace

TextFont TextFont::make(int pixelSize) {
    TextFont result;
    // Consolas ships with Windows 11; the spike does not need a font fallback.
    result.font = QFont(QStringLiteral("Consolas"));
    result.font.setStyleHint(QFont::TypeWriter);
    result.font.setPixelSize(pixelSize);
    result.raw = QRawFont::fromFont(result.font);
    for (int c = 32; c < 127; ++c) {
        const QList<quint32> indexes = result.raw.glyphIndexesForString(QString(QChar(c)));
        result.glyphs[static_cast<std::size_t>(c)] = indexes.isEmpty() ? 0 : indexes.front();
    }
    const QList<QPointF> advances = result.raw.advancesForGlyphIndexes(QList<quint32>{result.glyphs['M']});
    result.advance = advances.isEmpty() ? pixelSize * 0.55 : advances.front().x();
    result.lineHeight =
        static_cast<int>(std::ceil(result.raw.ascent() + result.raw.descent() + std::max(0.0, result.raw.leading())));
    result.ascent = static_cast<int>(std::lround(result.raw.ascent()));
    return result;
}

VisibleLines visibleLines(const ViewState& state, int viewHeight, int lineHeight, std::size_t lineCount) {
    VisibleLines visible;
    if (lineHeight <= 0 || lineCount == 0 || viewHeight <= 0) {
        return visible;
    }
    const auto documentPx = static_cast<long long>(lineCount) * lineHeight;
    const long long scroll =
        std::clamp<long long>(std::llround(state.scrollPx), 0, std::max<long long>(0, documentPx - viewHeight));
    visible.first = static_cast<std::size_t>(scroll / lineHeight);
    visible.topPx = -static_cast<int>(scroll % lineHeight);
    const auto rows = static_cast<std::size_t>((viewHeight - visible.topPx + lineHeight - 1) / lineHeight);
    visible.count = std::min(rows, lineCount - visible.first);
    return visible;
}

GlyphRunCache::GlyphRunCache(const TextFont& font, std::size_t capacity, int maxColumns)
    : m_font(font), m_capacity(std::max<std::size_t>(capacity, 1)), m_maxColumns(maxColumns) {}

const std::vector<GlyphRunCache::Run>& GlyphRunCache::runs(const TextModel& model, std::size_t line) {
    if (const auto found = m_index.find(line); found != m_index.end()) {
        if (found->second->revision == model.revision(line)) {
            ++m_hits;
            m_lru.splice(m_lru.begin(), m_lru, found->second);
            return found->second->runs;
        }
        m_lru.erase(found->second);
        m_index.erase(found);
    }
    ++m_misses;
    // Monospace ASCII: glyph = table lookup, position = column * advance. No shaping.
    std::array<QList<quint32>, kTokenClasses> glyphs;
    std::array<QList<QPointF>, kTokenClasses> positions;
    const std::string_view text = model.line(line);
    const std::vector<TokenClass>& classes = model.classes(line);
    const std::size_t columns = std::min(text.size(), static_cast<std::size_t>(std::max(m_maxColumns, 0)));
    for (std::size_t column = 0; column < columns; ++column) {
        const auto c = static_cast<unsigned char>(text[column]);
        if (c == ' ' || c >= 128) {
            continue;
        }
        const auto cls = static_cast<std::size_t>(classes[column]);
        glyphs[cls].push_back(m_font.glyphs[c]);
        positions[cls].push_back(QPointF(static_cast<double>(column) * m_font.advance, 0.0));
    }
    Entry entry{line, model.revision(line), {}};
    for (std::size_t cls = 0; cls < kTokenClasses; ++cls) {
        if (glyphs[cls].isEmpty()) {
            continue;
        }
        Run run;
        run.cls = static_cast<TokenClass>(cls);
        run.run.setRawFont(m_font.raw);
        run.run.setGlyphIndexes(glyphs[cls]);
        run.run.setPositions(positions[cls]);
        entry.runs.push_back(std::move(run));
    }
    m_lru.push_front(std::move(entry));
    m_index[line] = m_lru.begin();
    while (m_lru.size() > m_capacity) {
        m_index.erase(m_lru.back().line);
        m_lru.pop_back();
    }
    return m_lru.front().runs;
}

void paintRaster(QPainter& painter, const TextModel& model, GlyphRunCache& cache, const TextFont& font,
                 const Theme& theme, const ViewState& state, QSize viewSize, const QRegion& clip) {
    const QRegion region = clip.isEmpty() ? QRegion(QRect(QPoint(0, 0), viewSize)) : clip;
    for (const QRect& rect : region) {
        painter.fillRect(rect, theme.background);
    }
    if (!state.fillOnly) {
        const VisibleLines visible = visibleLines(state, viewSize.height(), font.lineHeight, model.lineCount());
        for (std::size_t i = 0; i < visible.count; ++i) {
            const int top = visible.topPx + static_cast<int>(i) * font.lineHeight;
            if (!region.intersects(QRect(0, top, viewSize.width(), font.lineHeight))) {
                continue;
            }
            const QPointF baseline(0.0, top + font.ascent);
            for (const GlyphRunCache::Run& run : cache.runs(model, visible.first + i)) {
                painter.setPen(theme.classes[static_cast<std::size_t>(run.cls)]);
                painter.drawGlyphRun(baseline, run.run);
            }
        }
    }
    const perf::MarkerCells cells = perf::encodeMarker(state.marker);
    for (int i = 0; i < perf::kMarkerCells; ++i) {
        painter.fillRect(QRect(i * kMarkerCellPx, 0, kMarkerCellPx, kMarkerCellPx),
                         cells[static_cast<std::size_t>(i)] ? Qt::white : Qt::black);
    }
}

GlyphAtlas GlyphAtlas::build(const TextFont& font) {
    GlyphAtlas atlas;
    atlas.cellWidth = static_cast<int>(std::ceil(font.advance)) + 2;
    atlas.cellHeight = font.lineHeight;
    const int cells = 1 + (127 - 32);
    const int rows = (cells + atlas.columns - 1) / atlas.columns;
    atlas.image =
        QImage(atlas.columns * atlas.cellWidth, rows * atlas.cellHeight, QImage::Format_RGBA8888_Premultiplied);
    atlas.image.fill(Qt::transparent);
    QPainter painter(&atlas.image);
    painter.fillRect(atlas.cell('\0'), Qt::white);
    painter.setFont(font.font);
    painter.setPen(Qt::white);
    for (int c = 33; c < 127; ++c) {
        const QRect cell = atlas.cell(static_cast<char>(c));
        painter.drawText(QPointF(cell.x() + 1, cell.y() + font.ascent), QString(QChar(c)));
    }
    painter.end();
    return atlas;
}

QRect GlyphAtlas::cell(char c) const noexcept {
    const int index = c >= 32 && c < 127 ? c - 31 : 0;
    return {(index % columns) * cellWidth, (index / columns) * cellHeight, cellWidth, cellHeight};
}

void buildQuads(const TextModel& model, const TextFont& font, const GlyphAtlas& atlas, const Theme& theme,
                const ViewState& state, QSize view, std::vector<QuadInstance>& out) {
    out.clear();
    const auto width = static_cast<float>(atlas.image.width());
    const auto height = static_cast<float>(atlas.image.height());
    if (!state.fillOnly) {
        std::array<std::uint32_t, kTokenClasses> colours{};
        for (std::size_t cls = 0; cls < kTokenClasses; ++cls) {
            colours[cls] = packRgba(theme.classes[cls]);
        }
        const VisibleLines visible = visibleLines(state, view.height(), font.lineHeight, model.lineCount());
        const auto maxColumns = static_cast<std::size_t>(std::ceil(view.width() / font.advance));
        for (std::size_t i = 0; i < visible.count; ++i) {
            const std::size_t line = visible.first + i;
            const std::string_view text = model.line(line);
            const std::vector<TokenClass>& classes = model.classes(line);
            const auto top = static_cast<float>(visible.topPx + static_cast<int>(i) * font.lineHeight);
            const std::size_t columns = std::min(text.size(), maxColumns);
            for (std::size_t column = 0; column < columns; ++column) {
                const char c = text[column];
                if (c == ' ') {
                    continue;
                }
                const QRect cell = atlas.cell(c);
                out.push_back(
                    QuadInstance{static_cast<float>(std::lround(static_cast<double>(column) * font.advance) - 1), top,
                                 static_cast<float>(atlas.cellWidth), static_cast<float>(atlas.cellHeight),
                                 static_cast<float>(cell.x()) / width, static_cast<float>(cell.y()) / height,
                                 static_cast<float>(cell.x() + cell.width()) / width,
                                 static_cast<float>(cell.y() + cell.height()) / height,
                                 colours[static_cast<std::size_t>(classes[column])]});
            }
        }
    }
    // Marker cells sample the centre of the solid cell, so filtering cannot blur them.
    const QRect solid = atlas.cell('\0');
    const float u = (static_cast<float>(solid.x()) + static_cast<float>(solid.width()) / 2.0F) / width;
    const float v = (static_cast<float>(solid.y()) + static_cast<float>(solid.height()) / 2.0F) / height;
    const perf::MarkerCells cells = perf::encodeMarker(state.marker);
    for (int i = 0; i < perf::kMarkerCells; ++i) {
        const std::uint32_t colour = cells[static_cast<std::size_t>(i)] ? 0xFFFFFFFFU : 0xFF000000U;
        out.push_back(QuadInstance{static_cast<float>(i * kMarkerCellPx), 0.0F, kMarkerCellPx, kMarkerCellPx, u, v, u,
                                   v, colour});
    }
}

bool fitQuads(std::vector<QuadInstance>& quads, std::size_t capacity) {
    if (quads.size() <= capacity) {
        return false;
    }
    // Move the trailing marker quads into the last slots that fit, then cut.
    constexpr auto kMarker = static_cast<std::ptrdiff_t>(perf::kMarkerCells);
    std::copy(quads.end() - kMarker, quads.end(), quads.begin() + (static_cast<std::ptrdiff_t>(capacity) - kMarker));
    quads.resize(capacity);
    return true;
}

RasterView::RasterView(const TextModel& model, const TextFont& font, const Theme& theme)
    : m_model(model), m_font(font), m_theme(theme), m_cache(font, 4096, 1024) {
    // Every pixel is painted, so Qt need not erase first.
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
}

void RasterView::repaintNow(const QRegion& dirty) {
    const long long target = std::llround(m_state.scrollPx);
    const long long delta = target - m_paintedScroll;
    const QRect marker(0, 0, kMarkerCellPx * perf::kMarkerCells, kMarkerCellPx);
    if (dirty.isEmpty() && m_scrollBlit && m_paintedScroll >= 0 && delta != 0 && std::llabs(delta) < height() / 2) {
        // Moves the pixels and marks the exposed strip dirty; repaint() of the marker
        // then paints every dirty region and flushes synchronously.
        scroll(0, static_cast<int>(-delta));
        repaint(marker);
    } else if (dirty.isEmpty()) {
        repaint();
    } else {
        repaint(dirty);
    }
    m_paintedScroll = target;
}

void RasterView::paintEvent(QPaintEvent* event) {
    // Budget: B5, B6.
    const std::int64_t begin = perf::win::qpcNow();
    QPainter painter(this);
    // Integer scroll positions, so blitted and repainted pixels line up.
    ViewState state = m_state;
    state.scrollPx = static_cast<double>(std::llround(state.scrollPx));
    paintRaster(painter, m_model, m_cache, m_font, m_theme, state, size(), event->region());
    painter.end();
    if (m_state.injectDelayMs > 0) {
        perf::win::spinUntil(perf::win::qpcNow() + perf::win::qpcRate().fromMs(m_state.injectDelayMs));
    }
    logPaint(m_state.marker, begin, perf::win::qpcNow());
}

} // namespace frost::s1
