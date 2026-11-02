// SPDX-License-Identifier: Apache-2.0
//
// The QRhi editor backend's widget side (frost/ui/editor/rhi_renderer.hpp): the QRhiWidget viewport
// that records each frame with RhiGlyphPainter, and the IEditorRenderer that feeds it.

#include "frost/ui/editor/rhi_renderer.hpp"

#include "frost/core/log.hpp"
#include "frost/ui/editor/editor_backend.hpp"

#include <rhi/qrhi.h>

namespace frost::ui {

RhiEditorViewport::RhiEditorViewport(const QFont& font, QWidget* parent) : QRhiWidget(parent), m_painter(font) {
    setApi(QRhiWidget::Api::Direct3D11);
    // Qt reports a QRhi it could not create, or a frame it could not begin, through renderFailed.
    connect(this, &QRhiWidget::renderFailed, this,
            [this] { reportFailure(QStringLiteral("QRhiWidget could not render (no QRhi, or a frame failed)")); });
}

RhiEditorViewport::~RhiEditorViewport() = default;

void RhiEditorViewport::setFrame(const EditorDisplayList& list) { m_frame = list; }

void RhiEditorViewport::setEditorFont(const QFont& font) {
    m_painter.setFont(font);
    update();
}

void RhiEditorViewport::reportFailure(const QString& reason) {
    if (m_failed) {
        return;
    }
    m_failed = true;
    emit failed(reason);
}

void RhiEditorViewport::initialize(QRhiCommandBuffer*) {
    // Called on the first frame and whenever the colour buffer is recreated (resize, DPI change);
    // the painter keeps its resources while the QRhi and the render pass stay the same.
    QRhi* const rhi = this->rhi();
    if (rhi == nullptr) {
        reportFailure(QStringLiteral("no QRhi"));
        return;
    }
    if (const Result<void> ready = m_painter.initialize(*rhi, *renderTarget()->renderPassDescriptor()); !ready) {
        reportFailure(QString::fromStdString(ready.error().message()));
        return;
    }
    if (!m_adapterReported) {
        m_adapterReported = true;
        const QRhiDriverInfo info = rhi->driverInfo();
        m_adapter = QString::fromUtf8(info.deviceName);
        const bool software =
            isSoftwareAdapter(info.vendorId, info.deviceId, info.deviceType == QRhiDriverInfo::CpuDevice);
        log::info("editor", "QRhi editor backend on {} ({}, {})", m_adapter.toStdString(), rhi->backendName(),
                  software ? "software" : "hardware");
        emit adapterReady(m_adapter, software);
    }
}

void RhiEditorViewport::render(QRhiCommandBuffer* cb) {
    QRhi* const rhi = this->rhi();
    if (rhi == nullptr || m_failed || !m_painter.initialized()) {
        return;
    }
    if (rhi->isDeviceLost()) {
        reportFailure(QStringLiteral("the graphics device was lost"));
        return;
    }
    // The colour buffer is the widget's size times its device pixel ratio; a texture render target
    // reports a ratio of 1, so the widget's own ratio scales the logical display list.
    m_painter.prepare(m_frame, devicePixelRatioF());
    m_painter.record(*cb, *renderTarget());
}

void RhiEditorViewport::releaseResources() { m_painter.releaseResources(); }

RhiEditorRenderer::RhiEditorRenderer(RhiEditorViewport& viewport) : m_viewport(&viewport) {}

void RhiEditorRenderer::render(const EditorDisplayList& list, QPaintDevice&) {
    // The viewport records the list when Qt calls its render(), right after this paint event.
    m_viewport->setFrame(list);
}

void RhiEditorRenderer::setFont(const QFont& font) { m_viewport->setEditorFont(font); }

} // namespace frost::ui
