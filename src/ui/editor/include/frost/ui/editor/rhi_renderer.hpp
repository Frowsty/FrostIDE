// SPDX-License-Identifier: Apache-2.0
//
// The QRhi editor backend's widget side (ADR 0011 A1, ADR 0043): RhiEditorViewport, the
// QRhiWidget an EditorView uses as its viewport while the QRhi backend draws, and
// RhiEditorRenderer, the IEditorRenderer that hands it each frame's display list. The drawing
// itself is RhiGlyphPainter's.
#pragma once

#include "frost/ui/editor/editor_renderer.hpp"
#include "frost/ui/editor/rhi_glyph_painter.hpp"

#include <QFont>
#include <QRhiWidget>
#include <QString>

#include <string>
#include <string_view>

namespace frost::ui {

/*
 * The QRhiWidget (Direct3D 11) an EditorView puts in place of its raster viewport. Each paint, the
 * view hands it the frame's display list (setFrame) and Qt then calls render(), which prepares and
 * records it with the painter at the widget's device pixel ratio; resizes and DPI changes recreate
 * the colour buffer, and a new ratio resets the atlas.
 *
 * Failures are reported once through `failed` (the reason as text): Qt's renderFailed (no QRhi, a
 * frame that could not begin), a painter initialisation error, or a lost device. `adapterReady`
 * names the adapter after the first initialisation and says whether it is a software one (WARP),
 * which the view's auto rule turns down. The owner must handle both through queued connections:
 * they are emitted inside Qt's paint, where the widget cannot be deleted. GUI thread only.
 */
class RhiEditorViewport final : public QRhiWidget {
    Q_OBJECT

public:
    explicit RhiEditorViewport(const QFont& font, QWidget* parent = nullptr);
    ~RhiEditorViewport() override;

    // The next frame to draw (a copy; the list's strings are implicitly shared).
    void setFrame(const EditorDisplayList& list);
    void setEditorFont(const QFont& font);
    [[nodiscard]] const RhiGlyphPainter& painter() const noexcept { return m_painter; }
    [[nodiscard]] bool hasFailed() const noexcept { return m_failed; }
    [[nodiscard]] const QString& adapterName() const noexcept { return m_adapter; }

    // Reports `reason` through `failed` (once), as a real failure would; tests and fault hooks.
    void reportFailure(const QString& reason);

signals:
    void failed(const QString& reason);
    void adapterReady(const QString& name, bool software);

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

private:
    RhiGlyphPainter m_painter;
    EditorDisplayList m_frame;
    QString m_adapter;
    bool m_failed = false;
    bool m_adapterReported = false;
};

/*
 * The IEditorRenderer of the QRhi backend: render() gives the list to the viewport (which draws it
 * when Qt calls its render()), setFont() resets the viewport's atlas. The viewport is owned by the
 * EditorView (as its QAbstractScrollArea viewport) and outlives this renderer. GUI thread only.
 */
class RhiEditorRenderer final : public IEditorRenderer {
public:
    explicit RhiEditorRenderer(RhiEditorViewport& viewport);

    [[nodiscard]] std::string_view name() const noexcept override { return "rhi"; }
    void render(const EditorDisplayList& list, QPaintDevice& device) override;
    void setFont(const QFont& font) override;

    [[nodiscard]] RhiEditorViewport& viewport() noexcept { return *m_viewport; }

private:
    RhiEditorViewport* m_viewport;
};

} // namespace frost::ui
