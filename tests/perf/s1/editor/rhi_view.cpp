// SPDX-License-Identifier: Apache-2.0
//
// The QRhiWidget view of S1: one instanced draw of textured quads (glyphs from a
// GlyphAtlas plus the frame marker) per frame on Direct3D 11. Shaders are HLSL
// source compiled at pipeline creation by Qt's D3D11 backend (D3DCompile), so
// the spike needs neither qsb nor Qt Shader Tools.

#include "frost/perf/win/timing.hpp"

#include "views.hpp"
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace frost::s1 {

namespace {

// Vertex inputs use TEXCOORD<location>, the semantics Qt's D3D11 backend binds
// QRhiVertexInputAttribute locations to.
constexpr char kVertexShader[] = R"(
cbuffer Frame : register(b0) { float2 viewSize; float2 unused; };
struct VSIn { float2 corner : TEXCOORD0; float4 rect : TEXCOORD1; float4 uv : TEXCOORD2; float4 colour : TEXCOORD3; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 colour : TEXCOORD1; };
VSOut main(VSIn i) {
    VSOut o;
    float2 p = i.rect.xy + i.corner * i.rect.zw;
    o.pos = float4(p.x / viewSize.x * 2.0 - 1.0, 1.0 - p.y / viewSize.y * 2.0, 0.0, 1.0);
    o.uv = lerp(i.uv.xy, i.uv.zw, i.corner);
    o.colour = i.colour;
    return o;
}
)";

constexpr char kFragmentShader[] = R"(
Texture2D atlas : register(t1);
SamplerState atlasSampler : register(s1);
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 colour : TEXCOORD1; };
float4 main(PSIn i) : SV_TARGET {
    float coverage = atlas.Sample(atlasSampler, i.uv).a;
    return float4(i.colour.rgb * coverage, coverage);
}
)";

constexpr std::size_t kInstanceCapacity = 65536; // 640 columns x 74 rows fit with room to spare.

QShader hlslShader(QShader::Stage stage, const char* source, const QShader::NativeResourceBindingMap& bindings) {
    const QShaderKey key(QShader::HlslShader, QShaderVersion(50));
    QShader shader;
    shader.setStage(stage);
    shader.setShader(key, QShaderCode(QByteArray(source), QByteArrayLiteral("main")));
    // QRhi binding -> HLSL register (b/t, s); the backend maps 1:1 without a map,
    // but stating it keeps the shaders' register() clauses and the bindings in step.
    shader.setResourceBindingMap(key, bindings);
    return shader;
}

} // namespace

RhiView::RhiView(const TextModel& model, const TextFont& font, const Theme& theme)
    : m_model(model), m_font(font), m_theme(theme), m_atlas(GlyphAtlas::build(font)) {
    setApi(QRhiWidget::Api::Direct3D11);
}

RhiView::~RhiView() = default;

void RhiView::repaintNow(const QRegion& /*dirty*/) { repaint(); }

void RhiView::initialize(QRhiCommandBuffer* /*cb*/) {
    // Called again when the backing texture is recreated (resize); D3D11 render
    // pass descriptors stay compatible, so the pipeline is kept.
    if (!m_pipeline) {
        // B1 context: runtime HLSL compilation is part of the first frame here.
        const std::int64_t start = perf::win::qpcNow();
        if (!createPipeline()) {
            m_pipeline.reset();
        }
        m_pipelineMs = perf::win::qpcRate().toMs(perf::win::qpcNow() - start);
    }
}

bool RhiView::createPipeline() {
    QRhi* const r = rhi();
    if (r == nullptr) {
        m_error = QStringLiteral("no QRhi");
        return false;
    }
    m_corners.reset(r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, 8 * sizeof(float)));
    m_instanceCapacity = kInstanceCapacity;
    m_instances.reset(r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                                   static_cast<quint32>(kInstanceCapacity * sizeof(QuadInstance))));
    m_uniforms.reset(r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 4 * sizeof(float)));
    m_texture.reset(r->newTexture(QRhiTexture::RGBA8, m_atlas.image.size()));
    m_sampler.reset(r->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                  QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    if (!m_corners->create() || !m_instances->create() || !m_uniforms->create() || !m_texture->create() ||
        !m_sampler->create()) {
        m_error = QStringLiteral("buffer, texture or sampler creation failed");
        return false;
    }
    m_bindings.reset(r->newShaderResourceBindings());
    m_bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, m_uniforms.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_texture.get(),
                                                  m_sampler.get()),
    });
    if (!m_bindings->create()) {
        m_error = QStringLiteral("shader resource bindings creation failed");
        return false;
    }

    m_pipeline.reset(r->newGraphicsPipeline());
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
                        QRhiVertexInputBinding(sizeof(QuadInstance), QRhiVertexInputBinding::PerInstance)});
    layout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
        QRhiVertexInputAttribute(1, 1, QRhiVertexInputAttribute::Float4, offsetof(QuadInstance, x)),
        QRhiVertexInputAttribute(1, 2, QRhiVertexInputAttribute::Float4, offsetof(QuadInstance, u0)),
        QRhiVertexInputAttribute(1, 3, QRhiVertexInputAttribute::UNormByte4, offsetof(QuadInstance, rgba)),
    });
    m_pipeline->setVertexInputLayout(layout);
    m_pipeline->setShaderResourceBindings(m_bindings.get());
    m_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!m_pipeline->create()) {
        m_error = QStringLiteral("graphics pipeline creation failed (HLSL compile or input layout)");
        return false;
    }
    m_staticUploaded = false;
    return true;
}

void RhiView::render(QRhiCommandBuffer* cb) {
    // Budget: B5, B6.
    const std::int64_t begin = perf::win::qpcNow();
    QRhi* const r = rhi();
    QRhiResourceUpdateBatch* updates = r->nextResourceUpdateBatch();
    const QSize size = renderTarget()->pixelSize();
    std::size_t count = 0;
    if (m_pipeline) {
        if (!m_staticUploaded) {
            static constexpr std::array<float, 8> kCorners{0, 0, 1, 0, 0, 1, 1, 1};
            updates->uploadStaticBuffer(m_corners.get(), kCorners.data());
            updates->uploadTexture(m_texture.get(), m_atlas.image);
            m_staticUploaded = true;
        }
        const std::array<float, 4> frame{static_cast<float>(size.width()), static_cast<float>(size.height()), 0, 0};
        updates->updateDynamicBuffer(m_uniforms.get(), 0, sizeof(frame), frame.data());
        buildQuads(m_model, m_font, m_atlas, m_theme, m_state, size, m_quads);
        if (fitQuads(m_quads, m_instanceCapacity)) {
            ++m_truncatedFrames;
        }
        count = m_quads.size();
        if (count > 0) {
            updates->updateDynamicBuffer(m_instances.get(), 0, static_cast<quint32>(count * sizeof(QuadInstance)),
                                         m_quads.data());
        }
    }
    cb->beginPass(renderTarget(), m_theme.background, {1.0F, 0}, updates);
    if (m_pipeline && count > 0) {
        cb->setGraphicsPipeline(m_pipeline.get());
        cb->setViewport(QRhiViewport(0, 0, static_cast<float>(size.width()), static_cast<float>(size.height())));
        cb->setShaderResources();
        const std::array<QRhiCommandBuffer::VertexInput, 2> inputs{
            QRhiCommandBuffer::VertexInput{m_corners.get(), 0},
            QRhiCommandBuffer::VertexInput{m_instances.get(), 0},
        };
        cb->setVertexInput(0, static_cast<int>(inputs.size()), inputs.data());
        cb->draw(4, static_cast<quint32>(count));
    }
    cb->endPass();
    if (m_state.injectDelayMs > 0) {
        perf::win::spinUntil(perf::win::qpcNow() + perf::win::qpcRate().fromMs(m_state.injectDelayMs));
    }
    logPaint(m_state.marker, begin, perf::win::qpcNow());
}

void RhiView::releaseResources() {
    m_pipeline.reset();
    m_bindings.reset();
    m_sampler.reset();
    m_texture.reset();
    m_uniforms.reset();
    m_instances.reset();
    m_corners.reset();
}

} // namespace frost::s1
