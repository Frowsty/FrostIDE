// SPDX-License-Identifier: Apache-2.0
//
// The two minimal S1 editor views (ADR 0007), each drawing TextModel lines in a
// monospace font with per-class colours plus the frame marker at the view's
// top-left corner:
//   RasterView  QWidget painted with QPainter from a per-line QGlyphRun cache,
//               flushed by Qt's raster backing store (GDI into DWM);
//   RhiView     QRhiWidget (Direct3D 11) drawing instanced quads from a glyph
//               atlas, composited by Qt's RHI backing store into a swap chain.
// They are spike code: ASCII only, no wrapping, no horizontal scroll, one font.
// Everything here runs on the GUI thread.
#pragma once

#include "corpus.hpp"
#include <QColor>
#include <QFont>
#include <QGlyphRun>
#include <QImage>
#include <QRawFont>
#include <QRect>
#include <QRegion>
#include <QRhiWidget>
#include <QWidget>

#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

class QPainter;
class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;

namespace frost::s1 {

// The font both views use and its monospace grid, in device pixels (S1 runs at
// devicePixelRatio 1; the harness refuses other ratios).
struct TextFont {
    QFont font;
    QRawFont raw;
    double advance = 0;                // Horizontal advance of every printable ASCII glyph.
    int lineHeight = 0;                // Baseline-to-baseline distance, rounded up.
    int ascent = 0;                    // Baseline offset from the line top, rounded.
    std::array<quint32, 128> glyphs{}; // Glyph index per ASCII code (0 for controls).

    // A fixed-pitch system font at `pixelSize`. Precondition: a QGuiApplication exists.
    [[nodiscard]] static TextFont make(int pixelSize);
};

// Colours per TokenClass and the background.
struct Theme {
    QColor background{0x1e, 0x1f, 0x22};
    std::array<QColor, kTokenClasses> classes{
        QColor(0xbc, 0xbe, 0xc4), QColor(0xcf, 0x8e, 0x6d), QColor(0x6f, 0xaf, 0xbd), QColor(0x2a, 0xac, 0xb8),
        QColor(0x6a, 0xab, 0x73), QColor(0x7a, 0x7e, 0x85), QColor(0xa9, 0xb7, 0xc6), QColor(0xb3, 0xae, 0x60)};
};

// What a frame shows. The marker is painted in white and black 4 x 4 px cells
// (frost/perf/frame_marker.hpp) at view pixel (0, 0).
struct ViewState {
    double scrollPx = 0; // Top of the view in document pixels.
    std::uint16_t marker = 0;
    bool fillOnly = false; // Baseline mode: background and marker only, no text.
    // Probe validation: after drawing, the paint (raster paintEvent, RHI render)
    // spins this long on the GUI thread, a known delay the probes must measure.
    double injectDelayMs = 0;
};

// The first visible line and the pixel offset of its top for a scroll position.
struct VisibleLines {
    std::size_t first = 0;
    std::size_t count = 0;
    int topPx = 0; // Where line `first` starts, in view pixels (<= 0).
};
[[nodiscard]] VisibleLines visibleLines(const ViewState& state, int viewHeight, int lineHeight, std::size_t lineCount);

// Glyph runs of recently drawn lines, one QGlyphRun per highlight class, so a
// frame does no shaping: the monospace ASCII grid maps characters to glyph
// indexes and positions directly. LRU with a fixed capacity; entries are rebuilt
// when the line's revision changes.
class GlyphRunCache {
public:
    struct Run {
        TokenClass cls = TokenClass::Text;
        QGlyphRun run;
    };

    GlyphRunCache(const TextFont& font, std::size_t capacity, int maxColumns);

    // The runs of `line`, built on a miss. The reference is valid until the next call.
    const std::vector<Run>& runs(const TextModel& model, std::size_t line);

    [[nodiscard]] std::size_t hits() const noexcept { return m_hits; }
    [[nodiscard]] std::size_t misses() const noexcept { return m_misses; }

private:
    struct Entry {
        std::size_t line = 0;
        std::uint32_t revision = 0;
        std::vector<Run> runs;
    };

    const TextFont& m_font;
    std::size_t m_capacity;
    int m_maxColumns;
    std::list<Entry> m_lru; // Most recent first.
    std::unordered_map<std::size_t, std::list<Entry>::iterator> m_index;
    std::size_t m_hits = 0;
    std::size_t m_misses = 0;
};

// Paints one raster frame: background, visible lines from `cache`, the marker.
// `clip` limits what is drawn (a typing repaint passes the dirty line and the
// marker); `viewSize` is the full view, which fixes the visible lines.
void paintRaster(QPainter& painter, const TextModel& model, GlyphRunCache& cache, const TextFont& font,
                 const Theme& theme, const ViewState& state, QSize viewSize, const QRegion& clip);

// A glyph atlas for printable ASCII: cell 0 is solid white (marker cells), cell
// c - 31 holds character c in white with alpha coverage, premultiplied.
struct GlyphAtlas {
    QImage image;       // Format_RGBA8888_Premultiplied.
    int cellWidth = 0;  // ceil(advance) + 2 px of padding.
    int cellHeight = 0; // lineHeight.
    int columns = 16;

    [[nodiscard]] static GlyphAtlas build(const TextFont& font);
    // The cell rectangle of ASCII `c` (32..126), or of the solid cell for other values.
    [[nodiscard]] QRect cell(char c) const noexcept;
};

// One textured quad of the RHI view: position and size in view pixels, atlas
// coordinates normalised, RGBA8 colour. Layout matches the vertex input layout.
struct QuadInstance {
    float x, y, w, h;
    float u0, v0, u1, v1;
    std::uint32_t rgba;
};

// Fills `out` with the quads of one frame: glyphs of the visible lines, then the
// kMarkerCells marker quads last.
void buildQuads(const TextModel& model, const TextFont& font, const GlyphAtlas& atlas, const Theme& theme,
                const ViewState& state, QSize view, std::vector<QuadInstance>& out);

// Shrinks `quads` (as buildQuads left them) to at most `capacity` instances while
// keeping the trailing marker quads, so a frame with too many glyphs loses glyphs,
// never its marker. Returns true when glyph quads were dropped. Precondition:
// capacity >= kMarkerCells and quads.size() >= kMarkerCells. O(kMarkerCells).
bool fitQuads(std::vector<QuadInstance>& quads, std::size_t capacity);

// One completed paint (raster paintEvent or RHI render), in QPC ticks.
struct PaintRecord {
    std::uint16_t marker = 0;
    std::int64_t beginTicks = 0;
    std::int64_t endTicks = 0;
};

// The interface the harness drives. Implementations log every paint with the
// marker it drew, so input-to-paint-end (B5) needs no Qt event tracing and works
// when Qt defers the paint (QRhiWidget renders on the window's next update).
class FrameView {
public:
    virtual ~FrameView() = default;
    [[nodiscard]] virtual QWidget* widget() = 0;
    virtual void setState(const ViewState& state) = 0;
    [[nodiscard]] virtual const ViewState& state() const = 0;
    // Repaints now. Raster paints and flushes before returning; QRhiWidget does not
    // render inside repaint(), so the RHI view renders and presents on the window's
    // next update (the paint log records when). `dirty` limits the raster repaint
    // (empty = everything); the RHI view always renders the full frame.
    virtual void repaintNow(const QRegion& dirty) = 0;
    [[nodiscard]] const std::vector<PaintRecord>& paints() const noexcept { return m_paints; }

protected:
    void logPaint(std::uint16_t marker, std::int64_t beginTicks, std::int64_t endTicks) {
        m_paints.push_back(PaintRecord{marker, beginTicks, endTicks});
    }

private:
    std::vector<PaintRecord> m_paints;
};

class RasterView final : public QWidget, public FrameView {
public:
    RasterView(const TextModel& model, const TextFont& font, const Theme& theme);

    QWidget* widget() override { return this; }
    void setState(const ViewState& state) override { m_state = state; }
    [[nodiscard]] const ViewState& state() const override { return m_state; }
    void repaintNow(const QRegion& dirty) override;

    [[nodiscard]] const GlyphRunCache& cache() const noexcept { return m_cache; }

    // Scroll-blit mode: a full repaint after a scroll of less than half the view
    // height moves the backing store (QWidget::scroll) and paints only the exposed
    // strip and the marker. The flush still covers the whole view.
    void setScrollBlit(bool enabled) noexcept { m_scrollBlit = enabled; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    const TextModel& m_model;
    const TextFont& m_font;
    const Theme& m_theme;
    GlyphRunCache m_cache;
    ViewState m_state;
    long long m_paintedScroll = -1; // Integer scroll of the backing store contents; -1 = none yet.
    bool m_scrollBlit = false;
};

class RhiView final : public QRhiWidget, public FrameView {
public:
    RhiView(const TextModel& model, const TextFont& font, const Theme& theme);
    ~RhiView() override;

    QWidget* widget() override { return this; }
    void setState(const ViewState& state) override { m_state = state; }
    [[nodiscard]] const ViewState& state() const override { return m_state; }
    void repaintNow(const QRegion& dirty) override;

    // The most recent pipeline or resource error (empty when rendering works).
    [[nodiscard]] const QString& error() const noexcept { return m_error; }
    [[nodiscard]] std::size_t lastQuadCount() const noexcept { return m_quads.size(); }
    // Frames whose glyph quads did not fit the instance buffer (the marker is
    // always kept; glyphs beyond the capacity are dropped and counted here).
    [[nodiscard]] std::size_t truncatedFrames() const noexcept { return m_truncatedFrames; }
    // Milliseconds initialize() spent creating the pipeline (HLSL compiled at run
    // time; a shipping build would load precompiled .qsb shaders). 0 before then.
    [[nodiscard]] double pipelineMs() const noexcept { return m_pipelineMs; }

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

private:
    bool createPipeline();

    const TextModel& m_model;
    const TextFont& m_font;
    const Theme& m_theme;
    GlyphAtlas m_atlas;
    ViewState m_state;
    std::vector<QuadInstance> m_quads;
    std::unique_ptr<QRhiBuffer> m_corners;
    std::unique_ptr<QRhiBuffer> m_instances;
    std::unique_ptr<QRhiBuffer> m_uniforms;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiShaderResourceBindings> m_bindings;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
    std::size_t m_instanceCapacity = 0;
    std::size_t m_truncatedFrames = 0;
    double m_pipelineMs = 0;
    bool m_staticUploaded = false;
    QString m_error;
};

} // namespace frost::s1
