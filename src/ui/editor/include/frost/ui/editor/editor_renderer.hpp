// SPDX-License-Identifier: Apache-2.0
//
// The editor's rendering seam (PLAN.md section 2 "Editor rendering"; ADR 0011 A1, ADR 0037):
// IEditorRenderer draws an EditorDisplayList, and the raster backend does it with QPainter and a
// QGlyphRun cache. The QRhi glyph-atlas backend approved for Windows (ADR 0011 A1) implements the
// same interface in rhi_renderer.hpp (ADR 0043); editor_backend.hpp chooses between them.
#pragma once

#include "frost/ui/editor/display_list.hpp"

#include <QFont>
#include <QGlyphRun>
#include <QHash>
#include <QImage>
#include <QList>
#include <QString>

#include <cstdint>
#include <memory>
#include <string_view>

class QPaintDevice;
class QPainter;

namespace frost::ui {

/*
 * Draws display lists. The view owns one renderer for its lifetime and calls it from paintEvent
 * with its viewport; a renderer may keep caches keyed by text and font. GUI thread only.
 * A QRhi backend renders into its own QRhiWidget viewport; it receives the same lists, so the view
 * code that builds them is shared by every backend.
 */
class IEditorRenderer {
public:
    virtual ~IEditorRenderer() = default;

    // A short name for diagnostics ("raster").
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    // Draws `list` onto `device` (the editor's viewport), in the list's paint order.
    virtual void render(const EditorDisplayList& list, QPaintDevice& device) = 0;
    // The editor font changed (family, size, zoom, ligatures): drop what depends on it.
    virtual void setFont(const QFont& font) = 0;

protected:
    IEditorRenderer() = default;
    IEditorRenderer(const IEditorRenderer&) = default;
    IEditorRenderer& operator=(const IEditorRenderer&) = default;
};

// Counters of the raster backend's glyph-run cache, for tests.
struct GlyphCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t entries = 0;
};

/*
 * The raster backend: QPainter fills and lines, and text drawn as cached QGlyphRuns. A run's glyphs
 * come from QTextLayout once per (text, italic) and are drawn with QPainter::drawGlyphRun after
 * that, so repainting unchanged rows costs no shaping. The cache holds at most kMaxEntries runs and
 * is cleared when full or when the font changes. Stays the golden oracle and RDP fallback.
 */
class RasterEditorRenderer final : public IEditorRenderer {
public:
    static constexpr std::uint64_t kMaxEntries = 8192;

    explicit RasterEditorRenderer(const QFont& font);

    [[nodiscard]] std::string_view name() const noexcept override { return "raster"; }
    void render(const EditorDisplayList& list, QPaintDevice& device) override;
    void setFont(const QFont& font) override;

    [[nodiscard]] GlyphCacheStats cacheStats() const noexcept;

private:
    struct Shaped {
        QList<QGlyphRun> runs;
        double ascent = 0.0;
    };
    [[nodiscard]] const Shaped& shaped(const QString& text, bool italic);
    void drawText(QPainter& painter, const DisplayText& run);

    QFont m_font;
    QFont m_italic;
    QImage m_textScratch;
    QRectF m_viewport;
    QHash<QString, Shaped> m_regularCache;
    QHash<QString, Shaped> m_italicCache;
    std::uint64_t m_hits = 0;
    std::uint64_t m_misses = 0;
};

} // namespace frost::ui
