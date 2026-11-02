// SPDX-License-Identifier: Apache-2.0
//
// The QRhi editor frame painter (frost/ui/editor/rhi_glyph_painter.hpp): display list to quads, and
// quads to one render pass. The pipeline follows the S1 spike's (tests/perf/s1/editor/rhi_view.cpp,
// ADR 0007): HLSL compiled by Qt's D3D11 backend at pipeline creation, so no qsb is needed, one
// static strip of four corners and one instance per quad.

#include "frost/ui/editor/rhi_glyph_painter.hpp"

#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace frost::ui {

namespace {

// Vertex inputs use TEXCOORD<location>, the semantics Qt's D3D11 backend binds
// QRhiVertexInputAttribute locations to. `sizes`: the target's pixel size, then the atlas's.
constexpr char kVertexShader[] = R"(
cbuffer Frame : register(b0) { float4 sizes; };
struct VSIn { float2 corner : TEXCOORD0; float4 rect : TEXCOORD1; float4 uv : TEXCOORD2; float4 colour : TEXCOORD3; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 colour : TEXCOORD1; };
VSOut main(VSIn i) {
    VSOut o;
    float2 p = i.rect.xy + i.corner * i.rect.zw;
    o.pos = float4(p.x / sizes.x * 2.0 - 1.0, 1.0 - p.y / sizes.y * 2.0, 0.0, 1.0);
    o.uv = lerp(i.uv.xy, i.uv.zw, i.corner) / sizes.zw;
    o.colour = i.colour;
    return o;
}
)";

// Premultiplied output: the atlas holds coverage, the quad its straight-alpha colour.
constexpr char kFragmentShader[] = R"(
Texture2D atlas : register(t1);
SamplerState atlasSampler : register(s1);
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 colour : TEXCOORD1; };
float4 main(PSIn i) : SV_TARGET {
    float coverage = atlas.Sample(atlasSampler, i.uv).a * i.colour.a;
    return float4(i.colour.rgb * coverage, coverage);
}
)";

constexpr std::array<float, 8> kCorners{0, 0, 1, 0, 0, 1, 1, 1};
constexpr std::size_t kInitialInstances = 4096;
// The raster backend's cache bound, per style.
constexpr qsizetype kMaxShapedRuns = 8192;
// Glyphs whose origin lies this far outside the scissor are not drawn (a long line's tail).
constexpr double kCullMargin = 64.0;

[[nodiscard]] QShader hlslShader(QShader::Stage stage, const char* source,
                                 const QShader::NativeResourceBindingMap& bindings) {
    const QShaderKey key(QShader::HlslShader, QShaderVersion(50));
    QShader shader;
    shader.setStage(stage);
    shader.setShader(key, QShaderCode(QByteArray(source), QByteArrayLiteral("main")));
    // QRhi binding -> HLSL register (b/t, s), as the shaders' register() clauses say.
    shader.setResourceBindingMap(key, bindings);
    return shader;
}

[[nodiscard]] std::uint32_t packColor(const QColor& color) noexcept {
    const QRgb value = color.rgba();
    return static_cast<std::uint32_t>(qRed(value)) | (static_cast<std::uint32_t>(qGreen(value)) << 8) |
           (static_cast<std::uint32_t>(qBlue(value)) << 16) | (static_cast<std::uint32_t>(qAlpha(value)) << 24);
}

// `rect` in device pixels with each edge rounded to the pixel grid, as an aliased QPainter fill
// lands; a non-empty rectangle keeps at least one pixel per axis.
[[nodiscard]] QRect snap(QRectF rect, double dpr) {
    const QRectF r = rect.normalized();
    int x0 = static_cast<int>(std::lround(r.left() * dpr));
    int y0 = static_cast<int>(std::lround(r.top() * dpr));
    int x1 = static_cast<int>(std::lround(r.right() * dpr));
    int y1 = static_cast<int>(std::lround(r.bottom() * dpr));
    if (r.width() > 0.0 && x1 <= x0) {
        x1 = x0 + 1;
    }
    if (r.height() > 0.0 && y1 <= y0) {
        y1 = y0 + 1;
    }
    return QRect(QPoint(x0, y0), QPoint(x1 - 1, y1 - 1));
}

} // namespace

RhiGlyphPainter::RhiGlyphPainter(const QFont& font) { setFont(font); }

RhiGlyphPainter::~RhiGlyphPainter() { releaseResources(); }

void RhiGlyphPainter::setFont(const QFont& font) {
    m_font = font;
    m_italic = font;
    m_italic.setItalic(true);
    m_regular.clear();
    m_italicRuns.clear();
    m_atlas.reset(m_atlas.devicePixelRatio());
}

const RhiGlyphPainter::Shaped& RhiGlyphPainter::shaped(const QString& text, bool italic) {
    QHash<QString, Shaped>& cache = italic ? m_italicRuns : m_regular;
    if (const auto it = cache.constFind(text); it != cache.constEnd()) {
        return *it;
    }
    if (cache.size() >= kMaxShapedRuns) {
        cache.clear();
    }
    // Shaped exactly as the raster backend shapes (no wrap, the same fonts), so both draw the same
    // glyphs at the same positions.
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
        for (const QGlyphRun& run : layout.glyphRuns()) {
            entry.runs.push_back(ShapedRun{run.rawFont(), run.glyphIndexes(), run.positions()});
        }
    }
    return *cache.insert(text, std::move(entry));
}

void RhiGlyphPainter::prepare(const EditorDisplayList& list, double devicePixelRatio) {
    const double dpr = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
    if (std::abs(dpr - m_atlas.devicePixelRatio()) > 1e-6) {
        m_atlas.reset(dpr); // A DPI change or a move to another monitor.
    }
    m_dpr = dpr;
    m_pixelSize = QSize(static_cast<int>(std::lround(list.viewport.width() * dpr)),
                        static_cast<int>(std::lround(list.viewport.height() * dpr)));
    m_background = list.background;
    m_stats.droppedGlyphs = 0;
    if (!buildQuads(list, false)) {
        // The atlas filled up: start a fresh one with only this frame's glyphs.
        m_atlas.reset(dpr);
        (void)buildQuads(list, true);
    }
    m_stats.quads = m_quads.size();
}

void RhiGlyphPainter::beginRange(std::size_t index, QRectF logicalScissor) {
    m_ranges[index] = Range{static_cast<std::uint32_t>(m_quads.size()), 0, snap(logicalScissor, m_dpr)};
}

void RhiGlyphPainter::endRange(std::size_t index) {
    m_ranges[index].count = static_cast<std::uint32_t>(m_quads.size()) - m_ranges[index].first;
}

bool RhiGlyphPainter::buildQuads(const EditorDisplayList& list, bool dropWhenFull) {
    m_quads.clear();
    m_stats.glyphQuads = 0;
    // Paint order and clipping as the raster backend: fills unclipped, the text area's lines,
    // text, preedit and carets clipped to it, then the gutter over everything.
    beginRange(0, list.viewport);
    for (const DisplayFill& fill : list.fills) {
        addSolid(fill.rect, fill.color);
    }
    endRange(0);
    beginRange(1, list.textArea);
    for (const DisplayLine& line : list.underlines) {
        addLine(line);
    }
    for (const DisplayText& run : list.text) {
        if (!addText(run, dropWhenFull)) {
            return false;
        }
    }
    for (const DisplayLine& line : list.overlays) {
        addLine(line);
    }
    for (const QRectF& caret : list.carets) {
        addSolid(caret, list.caretColor);
    }
    endRange(1);
    beginRange(2, list.viewport);
    if (!list.gutter.isEmpty()) {
        addSolid(list.gutter, list.gutterBackground);
        for (const DisplayText& run : list.gutterText) {
            if (!addText(run, dropWhenFull)) {
                return false;
            }
        }
        for (const DisplayMarker& marker : list.markers) {
            if (!addMarker(marker, dropWhenFull)) {
                return false;
            }
        }
    }
    endRange(2);
    return true;
}

void RhiGlyphPainter::addSolid(QRectF logical, const QColor& color) {
    if (color.alpha() == 0 || logical.isEmpty()) {
        return;
    }
    const QRect r = snap(logical, m_dpr);
    if (r.isEmpty()) {
        return;
    }
    const QPoint texel = EditorGlyphAtlas::solidTexel();
    const auto u = static_cast<float>(texel.x()) + 0.5F;
    const auto v = static_cast<float>(texel.y()) + 0.5F;
    m_quads.push_back(GlyphQuad{static_cast<float>(r.x()), static_cast<float>(r.y()), static_cast<float>(r.width()),
                                static_cast<float>(r.height()), u, v, u, v, packColor(color)});
}

void RhiGlyphPainter::addLine(const DisplayLine& line) {
    // The editor's lines are axis-aligned (the right margin, the preedit underline); a pen of
    // width w covers [x, x + w) right of a vertical line and [y, y + w) below a horizontal one.
    const QLineF& l = line.line;
    const double width = std::max(1.0, line.width);
    QRectF rect;
    if (l.x1() == l.x2()) {
        rect = QRectF(l.x1(), std::min(l.y1(), l.y2()), width, std::abs(l.y2() - l.y1()));
    } else if (l.y1() == l.y2()) {
        rect = QRectF(std::min(l.x1(), l.x2()), l.y1(), std::abs(l.x2() - l.x1()), width);
    } else {
        rect = QRectF(l.p1(), l.p2()).normalized().adjusted(0.0, 0.0, width, width);
    }
    addSolid(rect, line.color);
}

bool RhiGlyphPainter::addText(const DisplayText& run, bool dropWhenFull) {
    const Shaped& entry = shaped(run.text, run.italic);
    const std::uint32_t color = packColor(run.color);
    // QTextLayout puts the first line's top at y = 0, so its baseline is at y = ascent.
    const QPointF topLeft(run.origin.x(), run.origin.y() - entry.ascent);
    const double cullLeft = -kCullMargin;
    const double cullRight = static_cast<double>(m_pixelSize.width()) + kCullMargin;
    for (const ShapedRun& glyphs : entry.runs) {
        const int slot = m_atlas.fontSlot(glyphs.font);
        const qsizetype count = std::min(glyphs.glyphs.size(), glyphs.positions.size());
        for (qsizetype i = 0; i < count; ++i) {
            const QPointF at = topLeft + glyphs.positions[i];
            const double x = at.x() * m_dpr;
            if (x < cullLeft || x > cullRight) {
                continue;
            }
            // Whole pixels vertically, quarter pixels horizontally (the atlas's subpixel steps).
            const double baseline = std::round(at.y() * m_dpr);
            double whole = std::floor(x);
            auto step = static_cast<int>(std::lround((x - whole) * EditorGlyphAtlas::kSubpixelSteps));
            if (step >= EditorGlyphAtlas::kSubpixelSteps) {
                whole += 1.0;
                step = 0;
            }
            const AtlasEntry* cell = m_atlas.glyph(slot, glyphs.glyphs[i], step);
            if (cell == nullptr) {
                if (!dropWhenFull) {
                    return false;
                }
                ++m_stats.droppedGlyphs;
                continue;
            }
            if (cell->rect.isEmpty()) {
                continue; // A space or another glyph without ink.
            }
            const QRect& texels = cell->rect;
            m_quads.push_back(GlyphQuad{static_cast<float>(whole + cell->offset.x()),
                                        static_cast<float>(baseline + cell->offset.y()),
                                        static_cast<float>(texels.width()), static_cast<float>(texels.height()),
                                        static_cast<float>(texels.left()), static_cast<float>(texels.top()),
                                        static_cast<float>(texels.left() + texels.width()),
                                        static_cast<float>(texels.top() + texels.height()), color});
            ++m_stats.glyphQuads;
        }
    }
    return true;
}

bool RhiGlyphPainter::addMarker(const DisplayMarker& marker, bool dropWhenFull) {
    const int diameter = std::max(1, static_cast<int>(std::lround(marker.rect.width() * m_dpr)));
    const AtlasEntry* cell = m_atlas.disc(diameter);
    if (cell == nullptr) {
        if (!dropWhenFull) {
            return false;
        }
        ++m_stats.droppedGlyphs;
        return true;
    }
    const QRect& texels = cell->rect;
    m_quads.push_back(GlyphQuad{static_cast<float>(std::lround(marker.rect.x() * m_dpr)),
                                static_cast<float>(std::lround(marker.rect.y() * m_dpr)),
                                static_cast<float>(texels.width()), static_cast<float>(texels.height()),
                                static_cast<float>(texels.left()), static_cast<float>(texels.top()),
                                static_cast<float>(texels.left() + texels.width()),
                                static_cast<float>(texels.top() + texels.height()), packColor(marker.color)});
    return true;
}

// --- GPU ---

void RhiGlyphPainter::releaseResources() {
    m_pipeline.reset();
    m_bindings.reset();
    m_sampler.reset();
    m_texture.reset();
    m_uniforms.reset();
    m_instances.reset();
    m_corners.reset();
    m_rhi = nullptr;
    m_passFormat.clear();
    m_instanceCapacity = 0;
    m_uploadedGeneration = 0;
    m_cornersUploaded = false;
}

Result<void> RhiGlyphPainter::initialize(QRhi& rhi, QRhiRenderPassDescriptor& pass) {
    if (m_rhi == &rhi && m_pipeline && pass.serializedFormat() == m_passFormat) {
        return {}; // A resize recreated the target only; the resources still fit.
    }
    releaseResources();
    m_rhi = &rhi;
    const auto failed = [this](const char* what) {
        releaseResources();
        return fail(ErrorCode::Unsupported, std::string("QRhi editor backend: ") + what);
    };

    m_corners.reset(rhi.newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kCorners)));
    m_instanceCapacity = kInitialInstances;
    m_instances.reset(rhi.newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                                    static_cast<quint32>(m_instanceCapacity * sizeof(GlyphQuad))));
    m_uniforms.reset(rhi.newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 4 * sizeof(float)));
    m_texture.reset(rhi.newTexture(QRhiTexture::RGBA8, m_atlas.image().size()));
    m_sampler.reset(rhi.newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                   QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    if (!m_corners->create() || !m_instances->create() || !m_uniforms->create()) {
        return failed("vertex or uniform buffer creation failed");
    }
    if (!m_texture->create() || !m_sampler->create()) {
        return failed("atlas texture or sampler creation failed");
    }
    m_bindings.reset(rhi.newShaderResourceBindings());
    m_bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, m_uniforms.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_texture.get(),
                                                  m_sampler.get()),
    });
    if (!m_bindings->create()) {
        return failed("shader resource bindings creation failed");
    }

    m_pipeline.reset(rhi.newGraphicsPipeline());
    m_pipeline->setFlags(QRhiGraphicsPipeline::UsesScissor);
    m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true; // Premultiplied alpha.
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    m_pipeline->setTargetBlends({blend});
    m_pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, hlslShader(QShader::VertexStage, kVertexShader, {{0, {0, -1}}})},
        {QRhiShaderStage::Fragment, hlslShader(QShader::FragmentStage, kFragmentShader, {{1, {1, 1}}})},
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({QRhiVertexInputBinding(2 * sizeof(float)),
                        QRhiVertexInputBinding(sizeof(GlyphQuad), QRhiVertexInputBinding::PerInstance)});
    layout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
        QRhiVertexInputAttribute(1, 1, QRhiVertexInputAttribute::Float4, offsetof(GlyphQuad, x)),
        QRhiVertexInputAttribute(1, 2, QRhiVertexInputAttribute::Float4, offsetof(GlyphQuad, u0)),
        QRhiVertexInputAttribute(1, 3, QRhiVertexInputAttribute::UNormByte4, offsetof(GlyphQuad, rgba)),
    });
    m_pipeline->setVertexInputLayout(layout);
    m_pipeline->setShaderResourceBindings(m_bindings.get());
    m_pipeline->setRenderPassDescriptor(&pass);
    if (!m_pipeline->create()) {
        return failed("graphics pipeline creation failed (HLSL compilation or input layout)");
    }
    m_passFormat = pass.serializedFormat();
    return {};
}

bool RhiGlyphPainter::ensureInstanceCapacity(std::size_t count) {
    if (count <= m_instanceCapacity) {
        return true;
    }
    std::size_t capacity = m_instanceCapacity;
    while (capacity < count) {
        capacity *= 2;
    }
    m_instances->setSize(static_cast<quint32>(capacity * sizeof(GlyphQuad)));
    if (!m_instances->create()) {
        return false;
    }
    m_instanceCapacity = capacity;
    return true;
}

void RhiGlyphPainter::record(QRhiCommandBuffer& cb, QRhiRenderTarget& target) {
    // Budget: B5, B6.
    ++m_stats.frames;
    m_stats.draws = 0;
    QRhiResourceUpdateBatch* updates = m_rhi->nextResourceUpdateBatch();
    if (!m_cornersUploaded) {
        updates->uploadStaticBuffer(m_corners.get(), kCorners.data());
        m_cornersUploaded = true;
    }

    // The atlas: the whole image after a reset or growth, else the part that changed.
    const QImage& image = m_atlas.image();
    if (m_texture->pixelSize() != image.size()) {
        m_texture->setPixelSize(image.size());
        (void)m_texture->create(); // The bindings pick the rebuilt texture up on setShaderResources.
        m_uploadedGeneration = 0;
    }
    if (m_uploadedGeneration != m_atlas.generation()) {
        updates->uploadTexture(m_texture.get(), image);
        m_uploadedGeneration = m_atlas.generation();
        ++m_stats.textureUploads;
    } else if (const QRect dirty = m_atlas.dirtyRect(); !dirty.isEmpty()) {
        QRhiTextureSubresourceUploadDescription part(image);
        part.setSourceTopLeft(dirty.topLeft());
        part.setSourceSize(dirty.size());
        part.setDestinationTopLeft(dirty.topLeft());
        updates->uploadTexture(m_texture.get(), QRhiTextureUploadDescription({0, 0, part}));
        ++m_stats.textureUploads;
    }
    m_atlas.markUploaded();

    const QSize pixels = target.pixelSize();
    std::size_t count = m_quads.size();
    if (count > 0 && !ensureInstanceCapacity(count)) {
        count = 0; // Nothing fits this frame; the next one tries again.
    }
    if (count > 0) {
        updates->updateDynamicBuffer(m_instances.get(), 0, static_cast<quint32>(count * sizeof(GlyphQuad)),
                                     m_quads.data());
    }
    const std::array<float, 4> sizes{static_cast<float>(pixels.width()), static_cast<float>(pixels.height()),
                                     static_cast<float>(image.width()), static_cast<float>(image.height())};
    updates->updateDynamicBuffer(m_uniforms.get(), 0, sizeof(sizes), sizes.data());

    cb.beginPass(&target, m_background, {1.0F, 0}, updates);
    if (count > 0) {
        cb.setGraphicsPipeline(m_pipeline.get());
        cb.setViewport(QRhiViewport(0, 0, static_cast<float>(pixels.width()), static_cast<float>(pixels.height())));
        cb.setShaderResources();
        const QRect bounds(QPoint(0, 0), pixels);
        for (const Range& range : m_ranges) {
            const QRect scissor = range.scissor.intersected(bounds);
            if (range.count == 0 || scissor.isEmpty() || range.first + range.count > count) {
                continue;
            }
            // QRhiScissor has a bottom-left origin.
            cb.setScissor(QRhiScissor(scissor.x(), pixels.height() - (scissor.y() + scissor.height()), scissor.width(),
                                      scissor.height()));
            const std::array<QRhiCommandBuffer::VertexInput, 2> inputs{
                QRhiCommandBuffer::VertexInput{m_corners.get(), 0},
                QRhiCommandBuffer::VertexInput{m_instances.get(),
                                               static_cast<quint32>(range.first * sizeof(GlyphQuad))},
            };
            cb.setVertexInput(0, static_cast<int>(inputs.size()), inputs.data());
            cb.draw(4, range.count);
            ++m_stats.draws;
        }
    }
    cb.endPass();
}

} // namespace frost::ui
