// SPDX-License-Identifier: Apache-2.0
//
// The QRhi editor backend's glyph atlas (frost/ui/editor/glyph_atlas.hpp): shelf packing into one
// coverage image, glyphs rasterised by QPainter at the atlas's device pixel ratio, discs and keyed
// vector coverage drawn by QPainter in device pixels.

#include "frost/ui/editor/glyph_atlas.hpp"

#include <QGlyphRun>
#include <QList>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace frost::ui {

namespace {

// Key layout: bit 62 marks a disc; glyphs pack the font slot, the subpixel step and the index.
constexpr quint64 kDiscKey = quint64{1} << 62;

[[nodiscard]] quint64 glyphKey(int slot, int subpixel, quint32 glyph) noexcept {
    return (static_cast<quint64>(slot & 0xFFFF) << 40) | (static_cast<quint64>(subpixel & 0xFF) << 32) | glyph;
}

// One texel of padding right of and below each cell, so nearest sampling never reads a neighbour.
constexpr int kPadding = 1;

// The bounding box of the pixels of `cell` with any coverage (empty when there is none).
[[nodiscard]] QRect inkBounds(const QImage& cell) {
    int minX = cell.width();
    int minY = cell.height();
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < cell.height(); ++y) {
        const auto* row = reinterpret_cast<const quint32*>(cell.constScanLine(y));
        for (int x = 0; x < cell.width(); ++x) {
            if ((row[x] >> 24) != 0) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
    }
    return maxX < 0 ? QRect() : QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

} // namespace

EditorGlyphAtlas::EditorGlyphAtlas(int initialSize, int maxSize)
    : m_initialSize(std::max(initialSize, 64)), m_maxSize(std::max(maxSize, std::max(initialSize, 64))) {
    reset(1.0);
    m_rebuilds = 0; // The construction is not a rebuild.
}

void EditorGlyphAtlas::clearImage(int size) {
    m_image = QImage(size, size, QImage::Format_RGBA8888_Premultiplied);
    m_image.fill(0);
    for (int y = 0; y < kSolidSize; ++y) {
        auto* row = reinterpret_cast<quint32*>(m_image.scanLine(y));
        std::fill(row, row + kSolidSize, 0xFFFFFFFFU);
    }
    m_dirty = m_image.rect();
    ++m_generation;
}

void EditorGlyphAtlas::reset(double devicePixelRatio) {
    m_dpr = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
    m_fonts.clear();
    m_cells.clear();
    m_coverage.clear();
    clearImage(m_initialSize);
    // The solid block takes the first shelf's left end.
    m_shelfTop = 0;
    m_shelfX = kSolidSize + kPadding;
    m_shelfHeight = kSolidSize + kPadding;
    ++m_rebuilds;
}

GlyphAtlasStats EditorGlyphAtlas::stats() const noexcept {
    return GlyphAtlasStats{static_cast<std::uint64_t>(m_cells.size() + m_coverage.size()), m_rebuilds, m_rasterised,
                           m_image.width()};
}

int EditorGlyphAtlas::fontSlot(const QRawFont& font) {
    for (std::size_t i = 0; i < m_fonts.size(); ++i) {
        if (m_fonts[i] == font) {
            return static_cast<int>(i);
        }
    }
    m_fonts.push_back(font);
    return static_cast<int>(m_fonts.size() - 1);
}

void EditorGlyphAtlas::grow() {
    const QImage old = m_image;
    clearImage(std::min(m_image.width() * 2, m_maxSize));
    // Existing cells keep their texel coordinates: the old image becomes the new one's top-left.
    for (int y = 0; y < old.height(); ++y) {
        std::copy_n(old.constScanLine(y), old.bytesPerLine(), m_image.scanLine(y));
    }
}

bool EditorGlyphAtlas::reserve(QSize size, QPoint& at) {
    const int w = size.width() + kPadding;
    const int h = size.height() + kPadding;
    if (w > m_maxSize || h > m_maxSize) {
        return false;
    }
    while (true) {
        const int side = m_image.width();
        if (w > side) {
            if (side >= m_maxSize) {
                return false;
            }
            grow();
            continue;
        }
        if (m_shelfX + w > side) {
            // Start the next shelf below this one.
            m_shelfTop += m_shelfHeight;
            m_shelfX = 0;
            m_shelfHeight = 0;
        }
        if (m_shelfTop + h > side) {
            if (side >= m_maxSize) {
                return false;
            }
            grow();
            continue;
        }
        at = QPoint(m_shelfX, m_shelfTop);
        m_shelfX += w;
        m_shelfHeight = std::max(m_shelfHeight, h);
        return true;
    }
}

bool EditorGlyphAtlas::place(const QImage& cell, QRect ink, QPoint offset, AtlasEntry& entry) {
    entry = AtlasEntry{QRect(), offset};
    if (!ink.isEmpty()) {
        QPoint at;
        if (!reserve(ink.size(), at)) {
            return false;
        }
        for (int y = 0; y < ink.height(); ++y) {
            const auto* source = reinterpret_cast<const quint32*>(cell.constScanLine(ink.top() + y)) + ink.left();
            uchar* target = m_image.scanLine(at.y() + y) + static_cast<std::ptrdiff_t>(at.x()) * 4;
            for (int x = 0; x < ink.width(); ++x) {
                // Coverage in every channel (premultiplied white); the shader reads alpha.
                const auto alpha = static_cast<uchar>(source[x] >> 24);
                std::fill_n(target + static_cast<std::ptrdiff_t>(x) * 4, 4, alpha);
            }
        }
        entry.rect = QRect(at, ink.size());
        m_dirty = m_dirty.united(entry.rect);
    }
    ++m_rasterised;
    return true;
}

const AtlasEntry* EditorGlyphAtlas::placeKeyed(quint64 key, const QImage& cell, QRect ink, QPoint offset) {
    AtlasEntry entry;
    if (!place(cell, ink, offset, entry)) {
        return nullptr;
    }
    return &*m_cells.insert(key, entry);
}

const AtlasEntry* EditorGlyphAtlas::glyph(int slot, quint32 glyph, int subpixel) {
    subpixel = std::clamp(subpixel, 0, kSubpixelSteps - 1);
    const quint64 key = glyphKey(slot, subpixel, glyph);
    if (const auto it = m_cells.constFind(key); it != m_cells.constEnd()) {
        return &*it;
    }
    if (slot < 0 || static_cast<std::size_t>(slot) >= m_fonts.size()) {
        return nullptr;
    }
    const QRawFont& font = m_fonts[static_cast<std::size_t>(slot)];
    // The glyph's bounds around its origin (logical pixels, y down), widened by a margin for
    // hinting and antialiasing, give the scratch cell in device pixels.
    const QRectF bounds = font.boundingRect(glyph);
    constexpr int kMargin = 2;
    const int left = static_cast<int>(std::floor(bounds.left() * m_dpr)) - kMargin;
    const int top = static_cast<int>(std::floor(bounds.top() * m_dpr)) - kMargin;
    const int right = static_cast<int>(std::ceil(bounds.right() * m_dpr)) + kMargin + 1;
    const int bottom = static_cast<int>(std::ceil(bounds.bottom() * m_dpr)) + kMargin;
    const QSize cellSize(std::max(1, right - left), std::max(1, bottom - top));

    QImage cell(cellSize, QImage::Format_ARGB32_Premultiplied);
    cell.fill(Qt::transparent);
    cell.setDevicePixelRatio(m_dpr);
    {
        QPainter painter(&cell);
        painter.setPen(Qt::white);
        QGlyphRun run;
        run.setRawFont(font);
        run.setGlyphIndexes(QList<quint32>{glyph});
        run.setPositions(QList<QPointF>{QPointF(0.0, 0.0)});
        // The origin sits at device pixel (-left + step / kSubpixelSteps, -top).
        const double step = static_cast<double>(subpixel) / kSubpixelSteps;
        painter.drawGlyphRun(QPointF((-left + step) / m_dpr, -top / m_dpr), run);
    }

    const QRect ink = inkBounds(cell);
    return placeKeyed(key, cell, ink, QPoint(ink.left() + left, ink.top() + top));
}

const AtlasEntry* EditorGlyphAtlas::disc(int diameter) {
    diameter = std::clamp(diameter, 1, 256);
    const quint64 key = kDiscKey | static_cast<quint64>(diameter);
    if (const auto it = m_cells.constFind(key); it != m_cells.constEnd()) {
        return &*it;
    }
    QImage cell(diameter, diameter, QImage::Format_ARGB32_Premultiplied);
    cell.fill(Qt::transparent);
    {
        QPainter painter(&cell);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawEllipse(QRectF(0.0, 0.0, diameter, diameter));
    }
    return placeKeyed(key, cell, cell.rect(), QPoint(0, 0));
}

const AtlasEntry* EditorGlyphAtlas::coverage(const QByteArray& key, QSize size, function_ref<void(QPainter&)> paint) {
    if (const auto it = m_coverage.constFind(key); it != m_coverage.constEnd()) {
        return &*it;
    }
    if (size.isEmpty() || size.width() > m_maxSize || size.height() > m_maxSize) {
        return nullptr;
    }
    QImage cell(size, QImage::Format_ARGB32_Premultiplied);
    cell.fill(Qt::transparent);
    {
        QPainter painter(&cell);
        painter.setRenderHint(QPainter::Antialiasing, true);
        paint(painter);
    }
    const QRect ink = inkBounds(cell);
    AtlasEntry entry;
    if (!place(cell, ink, ink.isEmpty() ? QPoint(0, 0) : ink.topLeft(), entry)) {
        return nullptr;
    }
    return &*m_coverage.insert(key, entry);
}

} // namespace frost::ui
