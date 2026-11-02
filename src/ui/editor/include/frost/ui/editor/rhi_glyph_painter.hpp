// SPDX-License-Identifier: Apache-2.0
//
// The QRhi editor backend's frame painter (ADR 0007 decision 1, ADR 0043): turns an
// EditorDisplayList into instanced quads (glyphs and vector coverage from the EditorGlyphAtlas,
// solid and antialiased quads for fills, lines, carets, the gutter and the error stripe) and records
// them on any QRhi render target. It draws every DisplayElementKind the raster backend draws (ADR
// 0043 "Parity"). It does not depend on QRhiWidget, so tests render it into an offscreen
// QRhiTextureRenderTarget and compare it with the raster backend. The only code besides
// rhi_renderer.cpp that includes Qt's QRhi headers.
#pragma once

#include "frost/core/error.hpp"
#include "frost/ui/editor/display_list.hpp"
#include "frost/ui/editor/glyph_atlas.hpp"

#include <QColor>
#include <QFont>
#include <QGlyphRun>
#include <QHash>
#include <QList>
#include <QRawFont>
#include <QRect>
#include <QSize>
#include <QString>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiRenderTarget;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;

namespace frost::ui {

// One instanced quad: position and size in device pixels (top-left origin), atlas texels, and an
// RGBA8 colour with straight alpha (byte order r, g, b, a). The layout is the vertex input layout.
struct GlyphQuad {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;
    float u0 = 0.0F;
    float v0 = 0.0F;
    float u1 = 0.0F;
    float v1 = 0.0F;
    std::uint32_t rgba = 0;
};

// What the last prepare() built and the last record() drew.
struct RhiFrameStats {
    std::uint64_t quads = 0;
    std::uint64_t glyphQuads = 0;
    std::uint64_t droppedGlyphs = 0; // Glyphs and coverage cells that found no room even in a fresh atlas.
    std::uint64_t draws = 0;
    std::uint64_t textureUploads = 0; // Atlas uploads (whole or dirty part) since construction.
    std::uint64_t frames = 0;         // record() calls since construction.
    // The quads each element kind gave in the last prepare(), indexed by DisplayElementKind.
    std::array<std::uint64_t, kDisplayElementKindCount> kindQuads{};

    [[nodiscard]] std::uint64_t quadsOf(DisplayElementKind kind) const noexcept {
        return kindQuads[static_cast<std::size_t>(kind)];
    }
};

/*
 * One editor surface's QRhi painter. A frame is prepare() (CPU: shaping through a QTextLayout
 * glyph-run cache like the raster backend's, atlas fills, quads in device pixels) followed by
 * record() (GPU: atlas and instance uploads, one render pass cleared to the list's background and
 * one instanced draw per run of kinds with the same clip, as the raster backend clips: fills, then
 * the text area's kinds, then the gutter, the error stripe and the chrome; three draws in all).
 *
 * Every DisplayElementKind is drawn in kDisplayPaintOrder, each the way the raster backend draws it:
 * aliased fills and lines as solid quads snapped to whole device pixels; antialiased straight
 * underlines, strike-outs and plain rectangles as exact-coverage quads (up to nine per rectangle);
 * squiggles, dotted underlines and the chrome's shapes as atlas coverage cells drawn by the raster
 * backend's own QPainter code (display_strokes.hpp), long squiggles in chunks of whole periods so
 * repeated chunks share a cell; glyphs (regular and italic) and discs from the atlas.
 *
 * Coordinates: the list is in logical pixels; prepare() multiplies by `devicePixelRatio` and snaps
 * fills and lines to whole device pixels, glyph origins to whole pixels vertically and quarter
 * pixels horizontally (the atlas's subpixel steps), coverage cells to whole pixels with their
 * geometry quantised to a sixteenth of a pixel. A new ratio resets the atlas (DPI change, monitor
 * move); setFont() (family, size, zoom, ligatures) resets it and the shaping cache.
 *
 * GPU resources belong to the QRhi passed to initialize(); calling it again with another QRhi or an
 * incompatible render pass recreates them, and releaseResources() drops them (the owner calls it
 * before its QRhi goes). The shaders are HLSL compiled at pipeline creation by the Direct3D
 * backends; the Null backend accepts them and draws nothing. GUI thread only. Not copyable.
 */
class RhiGlyphPainter {
public:
    explicit RhiGlyphPainter(const QFont& font);
    ~RhiGlyphPainter();
    RhiGlyphPainter(const RhiGlyphPainter&) = delete;
    RhiGlyphPainter& operator=(const RhiGlyphPainter&) = delete;

    // The editor font changed: drops the shaping cache and resets the atlas.
    void setFont(const QFont& font);
    [[nodiscard]] const QFont& font() const noexcept { return m_font; }

    // CPU half of a frame. O(runs + glyphs), plus rasterising glyphs the atlas lacks. Budget: B5, B6.
    void prepare(const EditorDisplayList& list, double devicePixelRatio);
    [[nodiscard]] const std::vector<GlyphQuad>& quads() const noexcept { return m_quads; }
    [[nodiscard]] const EditorGlyphAtlas& atlas() const noexcept { return m_atlas; }

    /*
     * Creates the GPU resources for `rhi` and render passes compatible with `pass` (the target
     * record() draws to). Errors: Unsupported when a buffer, the atlas texture, the sampler, the
     * bindings or the pipeline (HLSL compilation) cannot be created; the painter is then left
     * without resources, and the owner falls back to raster.
     */
    [[nodiscard]] Result<void> initialize(QRhi& rhi, QRhiRenderPassDescriptor& pass);
    [[nodiscard]] bool initialized() const noexcept { return m_pipeline != nullptr; }
    void releaseResources();

    // GPU half: records the frame prepare() built into `cb` on `target`, whose pixel size is the
    // list's viewport times the ratio. Precondition: initialize() succeeded for cb's QRhi.
    void record(QRhiCommandBuffer& cb, QRhiRenderTarget& target);

    [[nodiscard]] const RhiFrameStats& stats() const noexcept { return m_stats; }

private:
    struct ShapedRun {
        QRawFont font;
        QList<quint32> glyphs;
        QList<QPointF> positions;
    };
    struct Shaped {
        std::vector<ShapedRun> runs;
        double ascent = 0.0;
    };
    // A contiguous range of quads drawn with one scissor rectangle (device pixels, top-left origin).
    struct Range {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        QRect scissor;
    };

    [[nodiscard]] const Shaped& shaped(const QString& text, bool italic);
    // Builds the quads; false when the atlas filled up (the caller resets it and builds again).
    [[nodiscard]] bool buildQuads(const EditorDisplayList& list, bool dropWhenFull);
    // The quads of one element kind (the parity switch); false as buildQuads.
    [[nodiscard]] bool addKind(const EditorDisplayList& list, DisplayElementKind kind, bool dropWhenFull);
    void addSolid(QRectF logical, const QColor& color);
    // An antialiased axis-aligned rectangle in device pixels: exact area coverage per pixel.
    void addCoverageRect(QRectF device, const QColor& color);
    void addLine(const DisplayLine& line);
    [[nodiscard]] bool addText(const DisplayText& run, bool dropWhenFull);
    [[nodiscard]] bool addMarker(const DisplayMarker& marker, bool dropWhenFull);
    [[nodiscard]] bool addDecoration(const DisplayDecoration& decoration, bool dropWhenFull);
    [[nodiscard]] bool addStrokeStrip(const DisplayDecoration& decoration, bool dropWhenFull);
    [[nodiscard]] bool addShape(const DisplayShape& shape, bool dropWhenFull);
    // A quad of atlas cell `cell` whose image's top-left lies at device pixel `origin`; a null cell
    // is a full atlas (false unless dropWhenFull, which counts it as dropped).
    [[nodiscard]] bool addCell(const AtlasEntry* cell, QPoint origin, const QColor& color, bool dropWhenFull);
    [[nodiscard]] bool ensureInstanceCapacity(std::size_t count);

    QFont m_font;
    QFont m_italic;
    QHash<QString, Shaped> m_regular;
    QHash<QString, Shaped> m_italicRuns;
    EditorGlyphAtlas m_atlas;
    double m_dpr = 1.0;
    QSize m_pixelSize;
    QColor m_background;
    std::vector<GlyphQuad> m_quads;
    std::vector<Range> m_ranges;
    RhiFrameStats m_stats;

    // GPU state.
    QRhi* m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> m_corners;
    std::unique_ptr<QRhiBuffer> m_instances;
    std::unique_ptr<QRhiBuffer> m_uniforms;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiShaderResourceBindings> m_bindings;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
    QList<quint32> m_passFormat;
    std::size_t m_instanceCapacity = 0;
    std::uint64_t m_uploadedGeneration = 0;
    bool m_cornersUploaded = false;
};

} // namespace frost::ui
