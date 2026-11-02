// SPDX-License-Identifier: Apache-2.0
//
// The QRhi editor backend's frame painter (ADR 0007 decision 1, ADR 0043): turns an
// EditorDisplayList into instanced quads (glyphs from the EditorGlyphAtlas, solid quads for fills,
// lines, carets and the gutter) and records them on any QRhi render target. It does not depend on
// QRhiWidget, so tests render it into an offscreen QRhiTextureRenderTarget and compare it with the
// raster backend. The only code besides rhi_renderer.cpp that includes Qt's QRhi headers.
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
    std::uint64_t droppedGlyphs = 0; // Glyphs that found no room even in a fresh atlas.
    std::uint64_t draws = 0;
    std::uint64_t textureUploads = 0; // Atlas uploads (whole or dirty part) since construction.
    std::uint64_t frames = 0;         // record() calls since construction.
};

/*
 * One editor surface's QRhi painter. A frame is prepare() (CPU: shaping through a QTextLayout
 * glyph-run cache like the raster backend's, atlas fills, quads in device pixels) followed by
 * record() (GPU: atlas and instance uploads, one render pass cleared to the list's background and
 * up to three instanced draws, scissored as the raster backend clips: fills, then the text area's
 * lines, text, preedit and carets, then the gutter).
 *
 * Coordinates: the list is in logical pixels; prepare() multiplies by `devicePixelRatio` and snaps
 * fills and lines to whole device pixels, glyph origins to whole pixels vertically and quarter
 * pixels horizontally (the atlas's subpixel steps). A new ratio resets the atlas (DPI change,
 * monitor move); setFont() (family, size, zoom, ligatures) resets it and the shaping cache.
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
    void addSolid(QRectF logical, const QColor& color);
    void addLine(const DisplayLine& line);
    [[nodiscard]] bool addText(const DisplayText& run, bool dropWhenFull);
    [[nodiscard]] bool addMarker(const DisplayMarker& marker, bool dropWhenFull);
    void beginRange(std::size_t index, QRectF logicalScissor);
    void endRange(std::size_t index);
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
    std::array<Range, 3> m_ranges{};
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
