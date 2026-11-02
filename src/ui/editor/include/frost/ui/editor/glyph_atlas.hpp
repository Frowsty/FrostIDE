// SPDX-License-Identifier: Apache-2.0
//
// The QRhi editor backend's glyph atlas (ADR 0043): glyphs, discs and vector coverage (squiggles,
// chrome shapes) rasterised by Qt's own raster engine at the device pixel ratio, packed on shelves
// into one CPU image that the backend uploads as a texture. It holds coverage only (colour comes
// from each quad), so a theme change needs no rebuild; a font, size, zoom or device-pixel-ratio
// change resets it. GPU-free, so tests can check it without a QRhi.
#pragma once

#include "frost/compat/function_ref.hpp"

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QPoint>
#include <QRawFont>
#include <QRect>
#include <QSize>

#include <cstdint>
#include <vector>

class QPainter;

namespace frost::ui {

// One atlas cell: `rect` in atlas texels (empty for a glyph without ink, such as a space) and the
// offset of its top-left corner from the glyph's origin (the baseline's left end) in device pixels.
struct AtlasEntry {
    QRect rect;
    QPoint offset;
};

// Counters for tests and diagnostics.
struct GlyphAtlasStats {
    std::uint64_t glyphs = 0;   // Cells held now (glyphs and discs).
    std::uint64_t rebuilds = 0; // reset() calls, including the ones a full atlas forced.
    std::uint64_t rasterised = 0;
    int size = 0; // The atlas image's side in texels.
};

/*
 * A square atlas image (Format_RGBA8888_Premultiplied, coverage in every channel) that starts at
 * `initialSize` texels and doubles up to `maxSize` as cells are added. Texels (0, 0) to (3, 3) are
 * opaque: solid quads (fills, carets, lines) sample their middle.
 *
 * Glyphs are keyed by (font slot, glyph index, horizontal subpixel step): fontSlot() maps each
 * QRawFont a glyph run uses (the editor font, its italic, fallback fonts) to a small integer.
 * A glyph is rasterised by drawing it with QPainter on a transparent image whose device pixel ratio
 * is the atlas's, at kSubpixelSteps horizontal offsets per pixel, so cells match what the raster
 * backend draws at the same position.
 *
 * coverage() holds any other antialiased drawing (the QRhi backend's squiggles and chrome shapes,
 * drawn by the raster backend's own QPainter code), keyed by a byte string that describes it.
 *
 * Cells stay valid until reset(); a full atlas at maxSize returns nullptr from glyph(), disc() and
 * coverage(), and the caller resets and rebuilds its frame. dirtyRect() is the area changed since the last
 * markUploaded(); generation() changes whenever the image is replaced (reset or growth).
 * GUI thread only (QPainter on QImage). Not copyable.
 */
class EditorGlyphAtlas {
public:
    static constexpr int kSubpixelSteps = 4;
    static constexpr int kSolidSize = 4;

    explicit EditorGlyphAtlas(int initialSize = 512, int maxSize = 4096);
    EditorGlyphAtlas(const EditorGlyphAtlas&) = delete;
    EditorGlyphAtlas& operator=(const EditorGlyphAtlas&) = delete;

    // Drops every cell and font slot and starts over at `devicePixelRatio` (font, zoom or DPI change).
    void reset(double devicePixelRatio);
    [[nodiscard]] double devicePixelRatio() const noexcept { return m_dpr; }

    // The slot of `font`, added on first use. O(slots); slots are few (regular, italic, fallbacks).
    [[nodiscard]] int fontSlot(const QRawFont& font);
    // The cell of `glyph` in slot `slot` at subpixel step `subpixel` (0 to kSubpixelSteps - 1),
    // rasterised on first use; nullptr when the atlas is full. Amortised O(1) after the first use.
    [[nodiscard]] const AtlasEntry* glyph(int slot, quint32 glyph, int subpixel);
    // An antialiased filled disc `diameter` device pixels wide (gutter markers); nullptr when full.
    [[nodiscard]] const AtlasEntry* disc(int diameter);
    /*
     * The cell of what `paint` draws in white on a transparent `size` image (device pixels, the
     * painter antialiased), keyed by `key`, which must determine the drawing exactly (callers
     * serialise its geometry relative to the image). The entry's offset is the ink's top-left
     * within that image. `paint` runs on the first use only. nullptr when the atlas is full or
     * `size` is empty or wider or taller than maxSize. Amortised O(1) after the first use.
     */
    [[nodiscard]] const AtlasEntry* coverage(const QByteArray& key, QSize size, function_ref<void(QPainter&)> paint);
    // The texel solid quads sample (the middle of the opaque block).
    [[nodiscard]] static QPoint solidTexel() noexcept { return QPoint(kSolidSize / 2, kSolidSize / 2); }

    [[nodiscard]] const QImage& image() const noexcept { return m_image; }
    [[nodiscard]] int size() const noexcept { return m_image.width(); }
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }
    [[nodiscard]] QRect dirtyRect() const noexcept { return m_dirty; }
    void markUploaded() noexcept { m_dirty = QRect(); }
    [[nodiscard]] GlyphAtlasStats stats() const noexcept;

private:
    // Copies the `ink` part of `cell` (coverage in alpha) into a free spot and fills `entry`;
    // false when it does not fit at maxSize.
    [[nodiscard]] bool place(const QImage& cell, QRect ink, QPoint offset, AtlasEntry& entry);
    [[nodiscard]] const AtlasEntry* placeKeyed(quint64 key, const QImage& cell, QRect ink, QPoint offset);
    [[nodiscard]] bool reserve(QSize size, QPoint& at);
    void grow();
    void clearImage(int size);

    int m_initialSize;
    int m_maxSize;
    double m_dpr = 1.0;
    QImage m_image;
    QRect m_dirty;
    std::uint64_t m_generation = 0;
    std::vector<QRawFont> m_fonts;
    QHash<quint64, AtlasEntry> m_cells;
    QHash<QByteArray, AtlasEntry> m_coverage;
    // The shelf being filled: its top, its height so far, and the next free x.
    int m_shelfTop = 0;
    int m_shelfHeight = 0;
    int m_shelfX = 0;
    std::uint64_t m_rebuilds = 0;
    std::uint64_t m_rasterised = 0;
};

} // namespace frost::ui
