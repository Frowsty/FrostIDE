// SPDX-License-Identifier: Apache-2.0
//
// EditorView (frost/ui/editor/editor_view.hpp), part 4: the renderer choice (ADR 0043). The view
// starts on raster, attaches the QRhi viewport after its first frame when editor.renderer's rule
// says so, and swaps back to a raster viewport on a setting change or a QRhi failure, keeping the
// editor model untouched.

#include "frost/core/log.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/editor/rhi_renderer.hpp"

#include <QMetaObject>
#include <QScrollBar>

#include <utility>

namespace frost::ui {

void EditorView::configureViewport() {
    QWidget* surface = viewport();
    surface->setAttribute(Qt::WA_InputMethodEnabled, true);
    surface->setAttribute(Qt::WA_OpaquePaintEvent, true);
    surface->setCursor(Qt::IBeamCursor);
    surface->setMouseTracking(false);
}

void EditorView::installViewport(QWidget* surface) {
    // QAbstractScrollArea deletes the previous viewport; nothing may still point into it.
    setViewport(surface);
    configureViewport();
    updateOverlayGeometry();
    updateScrollBars();
    viewport()->update();
}

void EditorView::selectRenderer() {
    const EditorRendererMode mode = parseEditorRendererMode(m_settings.renderer).value_or(EditorRendererMode::Auto);
    const RendererEnvironment environment =
        m_services.renderer.environment.has_value() ? *m_services.renderer.environment : detectRendererEnvironment();
    BackendChoice choice = chooseEditorBackend(mode, environment);
    if (choice.backend == EditorBackend::Rhi && m_rhiFailure) {
        choice = BackendChoice{EditorBackend::Raster, "QRhi failed earlier (" + *m_rhiFailure + "), raster"};
    }
    m_rendererState.mode = mode;
    if (choice.backend == EditorBackend::Raster) {
        m_rendererState.pending = false;
        useRaster(std::move(choice.reason));
        return;
    }
    m_rendererState.reason = std::move(choice.reason);
    if (m_rendererState.active == EditorBackend::Rhi) {
        return;
    }
    if (m_paintCount == 0) {
        // ADR 0007 decision 3: QRhi's start-up (about 210 ms of the first show()) stays off the first
        // frame, which raster paints; paintEvent attaches the QRhi viewport after it.
        m_rendererState.pending = true;
        return;
    }
    attachRhi();
}

void EditorView::attachRhi() {
    m_rendererState.pending = false;
    if (m_rhiViewport) {
        return;
    }
    auto* surface = new RhiEditorViewport(m_shaper->font());
    const QPointer<RhiEditorViewport> guard(surface);
    // Both signals come from inside Qt's paint of the viewport, so they are handled queued.
    connect(
        surface, &RhiEditorViewport::failed, this,
        [this, guard](const QString& reason) {
            if (guard && guard == m_rhiViewport) {
                fallBackToRaster(reason.toStdString());
            }
        },
        Qt::QueuedConnection);
    connect(
        surface, &RhiEditorViewport::adapterReady, this,
        [this, guard](const QString& name, bool software) {
            if (!guard || guard != m_rhiViewport || !software) {
                return;
            }
            RendererEnvironment environment = m_services.renderer.environment.has_value()
                                                  ? *m_services.renderer.environment
                                                  : detectRendererEnvironment();
            environment.softwareAdapter = true;
            const BackendChoice choice = chooseEditorBackend(m_rendererState.mode, environment);
            if (choice.backend == EditorBackend::Raster) {
                fallBackToRaster(choice.reason + " (" + name.toStdString() + ")");
            }
        },
        Qt::QueuedConnection);
    // The old renderer drew on the old viewport; switch the renderer first, then the widget. The
    // QPointer is set before the swap, so viewportEvent already passes the new viewport's first
    // resize on to the QRhiWidget.
    m_renderer = std::make_unique<RhiEditorRenderer>(*surface);
    m_rhiViewport = surface;
    // Hooked reports go first: a QRhiWidget that cannot render reports it during installViewport.
    if (m_services.renderer.failRhiCreation) {
        surface->reportFailure(QStringLiteral("forced QRhi creation failure (EditorRendererHooks)"));
    } else if (m_services.renderer.softwareAdapter.has_value()) {
        emit surface->adapterReady(QString::fromStdString(*m_services.renderer.softwareAdapter), true);
    }
    installViewport(surface);
    m_rendererState.active = EditorBackend::Rhi;
    log::info("editor", "editor renderer: rhi ({})", m_rendererState.reason);
    emit rendererChanged();
}

void EditorView::useRaster(std::string reason) {
    m_rendererState.reason = std::move(reason);
    if (m_rendererState.active == EditorBackend::Raster && !m_rhiViewport) {
        return;
    }
    m_renderer = std::make_unique<RasterEditorRenderer>(m_shaper->font());
    m_rhiViewport = nullptr;
    installViewport(new QWidget());
    m_rendererState.active = EditorBackend::Raster;
    log::info("editor", "editor renderer: raster ({})", m_rendererState.reason);
    emit rendererChanged();
}

void EditorView::fallBackToRaster(std::string reason) {
    log::warn("editor", "the QRhi editor renderer falls back to raster: {}", reason);
    m_rhiFailure = reason;
    ++m_rendererState.fallbacks;
    m_rendererState.pending = false;
    useRaster("fallback: " + reason);
}

} // namespace frost::ui
