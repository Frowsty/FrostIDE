// SPDX-License-Identifier: Apache-2.0
//
// The QRhi editor frame painter (frost/ui/editor/rhi_glyph_painter.hpp): display list to quads (every
// element kind, through one exhaustive switch), and quads to one render pass. The pipeline follows the S1 spike's
// (tests/perf/s1/editor/rhi_view.cpp, ADR 0007): HLSL compiled by Qt's D3D11 backend at pipeline creation, so no qsb is
// needed, one static strip of four corners and one instance per quad.

#include "frost/ui/editor/rhi_glyph_painter.hpp"

#include "display_strokes.hpp"
#include "editor_font.hpp"
#include <QPainter>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <span>
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
// Squiggles and dotted underlines are cut into coverage cells about this many device pixels wide.
constexpr double kStripChunk = 256.0;
// Maximum side of a chrome shape coverage cell, in device pixels.
constexpr int kMaxCellSide = 512;

// Coverage cells' geometry is quantised to Qt's own raster precision (26.6 fixed point): drawing
// it changes nothing, and a shape moved by whole pixels gets the same key.
constexpr double kCellSteps = 64.0;

// `value` rounded to the nearest 1 / `steps`.
[[nodiscard]] double quantised(double value, double steps = kCellSteps) noexcept {
    return std::round(value * steps) / steps;
}

// A coverage cell's key: a tag and the drawing's numbers in kCellSteps of a pixel.
[[nodiscard]] QByteArray cellKey(char tag, std::span<const double> numbers) {
    QByteArray key(1, tag);
    key.reserve(1 + static_cast<qsizetype>(numbers.size() * sizeof(std::int32_t)));
    for (const double number : numbers) {
        const auto fixed = static_cast<std::int32_t>(std::lround(number * kCellSteps));
        key.append(reinterpret_cast<const char*>(&fixed), sizeof(fixed));
    }
    return key;
}

[[nodiscard]] QByteArray cellKey(char tag, std::initializer_list<double> numbers) {
    return cellKey(tag, std::span<const double>(numbers.begin(), numbers.size()));
}

// One axis of an antialiased rectangle: a pixel range and the share of each pixel it covers.
struct CoverageSpan {
    double begin = 0.0;
    double end = 0.0;
    double share = 0.0;
};

// [from, to) as at most three spans: a partial first pixel, the whole pixels, a partial last one.
[[nodiscard]] std::pair<std::array<CoverageSpan, 3>, std::size_t> coverageSpans(double from, double to) noexcept {
    std::array<CoverageSpan, 3> out{};
    std::size_t count = 0;
    const double first = std::floor(from);
    const double last = std::floor(to);
    if (first == last) {
        out[count++] = CoverageSpan{first, first + 1.0, to - from};
        return {out, count};
    }
    const double inner = std::ceil(from);
    if (inner > from) {
        out[count++] = CoverageSpan{first, inner, inner - from};
    }
    if (last > inner) {
        out[count++] = CoverageSpan{inner, last, 1.0};
    }
    if (to > last) {
        out[count++] = CoverageSpan{last, last + 1.0, to - last};
    }
    return {out, count};
}

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
    m_font = detail::editorFont(font);
    m_italic = m_font;
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

bool RhiGlyphPainter::buildQuads(const EditorDisplayList& list, bool dropWhenFull) {
    m_quads.clear();
    m_ranges.clear();
    m_stats.glyphQuads = 0;
    m_stats.kindQuads.fill(0);
    // Paint order and clipping as the raster backend: one range per run of kinds with one scissor
    // (fills unclipped, the text area's kinds clipped to it, then the gutter, stripe and chrome).
    for (const DisplayElementKind kind : kDisplayPaintOrder) {
        const QRect scissor = snap(clippedToTextArea(kind) ? list.textArea : list.viewport, m_dpr);
        if (m_ranges.empty() || m_ranges.back().scissor != scissor) {
            m_ranges.push_back(Range{static_cast<std::uint32_t>(m_quads.size()), 0, scissor});
        }
        const std::size_t before = m_quads.size();
        if (!addKind(list, kind, dropWhenFull)) {
            return false;
        }
        m_stats.kindQuads[static_cast<std::size_t>(kind)] = m_quads.size() - before;
        m_ranges.back().count = static_cast<std::uint32_t>(m_quads.size()) - m_ranges.back().first;
    }
    return true;
}

bool RhiGlyphPainter::addKind(const EditorDisplayList& list, DisplayElementKind kind, bool dropWhenFull) {
    /*
     * The parity guard (ADR 0043 "Parity"). This binding names every member of the display list, so
     * a member added to EditorDisplayList fails to compile here (the list "decomposes into" more
     * elements than names) until it is drawn below or marked [[maybe_unused]] as data only, and
     * the switch has no default, so a DisplayElementKind without a QRhi path fails -Wswitch. The
     * element structs are named the same way where they are drawn.
     */
    const auto& [viewport [[maybe_unused]], background [[maybe_unused]], textArea [[maybe_unused]], gutter,
                 gutterBackground, font [[maybe_unused]], fills, underlines, runs, decorations, overlays, carets,
                 caretColor, gutterRuns, markers, breakpoints, executionPoint, changeStripes, stripe, stripeBackground,
                 stripeMarks, stripeSummary, stripeSummaryColor, inspection [[maybe_unused]], balloon [[maybe_unused]],
                 chrome, chromeRuns, firstRow [[maybe_unused]], rowCount [[maybe_unused]]] = list;
    // viewport, background and textArea are the target, the clear colour and the scissors (prepare,
    // buildQuads); the font is the painter's own (setFont); the inspection and balloon records are
    // drawn through `chrome` and `chromeRuns`; the rows are bookkeeping.
    const auto texts = [&](const std::vector<DisplayText>& all) {
        for (const DisplayText& run : all) {
            if (!addText(run, dropWhenFull)) {
                return false;
            }
        }
        return true;
    };
    switch (kind) {
    case DisplayElementKind::Fills:
        for (const DisplayFill& fill : fills) {
            const auto& [rect, color] = fill;
            addSolid(rect, color);
        }
        return true;
    case DisplayElementKind::BackgroundDecorations:
        // Aliased, as the raster backend fills them before it turns antialiasing on.
        for (const DisplayDecoration& decoration : decorations) {
            if (decoration.style == DecorationStyle::Background) {
                addSolid(decoration.rect, decoration.color);
            }
        }
        return true;
    case DisplayElementKind::Underlines:
        for (const DisplayLine& line : underlines) {
            addLine(line);
        }
        return true;
    case DisplayElementKind::Text: return texts(runs);
    case DisplayElementKind::Decorations:
        for (const DisplayDecoration& decoration : decorations) {
            if (decoration.style != DecorationStyle::Background && !addDecoration(decoration, dropWhenFull)) {
                return false;
            }
        }
        return true;
    case DisplayElementKind::Overlays:
        for (const DisplayLine& line : overlays) {
            addLine(line);
        }
        return true;
    case DisplayElementKind::Carets:
        for (const QRectF& caret : carets) {
            addSolid(caret, caretColor);
        }
        return true;
    case DisplayElementKind::GutterBackground:
        if (!gutter.isEmpty()) {
            addSolid(gutter, gutterBackground);
        }
        return true;
    case DisplayElementKind::GutterText: return gutter.isEmpty() || texts(gutterRuns);
    case DisplayElementKind::ChangeStripes:
        if (!gutter.isEmpty()) {
            for (const DisplayChangeStripe& change : changeStripes) {
                const auto& [rect, color, changeKind [[maybe_unused]], line [[maybe_unused]], index [[maybe_unused]]] =
                    change;
                addSolid(rect, color);
            }
        }
        return true;
    case DisplayElementKind::Markers:
        if (!gutter.isEmpty()) {
            for (const DisplayMarker& marker : markers) {
                if (!addMarker(marker, dropWhenFull)) {
                    return false;
                }
            }
            for (const DisplayBreakpoint& breakpoint : breakpoints) {
                const int diameter = std::max(1, static_cast<int>(std::lround(breakpoint.rect.width() * m_dpr)));
                const AtlasEntry* cell =
                    breakpoint.kind == BreakpointKind::Verified ? m_atlas.disc(diameter) : m_atlas.ring(diameter);
                const QPoint origin(static_cast<int>(std::lround(breakpoint.rect.x() * m_dpr)),
                                    static_cast<int>(std::lround(breakpoint.rect.y() * m_dpr)));
                if (!addCell(cell, origin, breakpoint.color, dropWhenFull))
                    return false;
            }
            if (executionPoint) {
                const QRectF& rect = executionPoint->rect;
                const int width = std::max(1, static_cast<int>(std::lround(rect.width() * m_dpr)));
                const int height = std::max(1, static_cast<int>(std::lround(rect.height() * m_dpr)));
                const QPoint origin(static_cast<int>(std::lround(rect.x() * m_dpr)),
                                    static_cast<int>(std::lround(rect.y() * m_dpr)));
                if (!addCell(m_atlas.arrow(width, height), origin, executionPoint->color, dropWhenFull))
                    return false;
            }
        }
        return true;
    case DisplayElementKind::StripeBackground:
        if (!stripe.isEmpty()) {
            addSolid(stripe, stripeBackground);
        }
        return true;
    case DisplayElementKind::StripeMarks:
        if (!stripe.isEmpty()) {
            for (const DisplayStripeMark& mark : stripeMarks) {
                const auto& [rect, color, markKind [[maybe_unused]], source [[maybe_unused]], line [[maybe_unused]]] =
                    mark;
                addSolid(rect, color);
            }
        }
        return true;
    case DisplayElementKind::StripeSummary:
        if (!stripe.isEmpty() && !stripeSummary.isEmpty()) {
            addSolid(stripeSummary, stripeSummaryColor);
        }
        return true;
    case DisplayElementKind::Chrome:
        for (const DisplayShape& shape : chrome) {
            if (!addShape(shape, dropWhenFull)) {
                return false;
            }
        }
        return true;
    case DisplayElementKind::ChromeText: return texts(chromeRuns);
    case DisplayElementKind::kCount: return true;
    }
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

void RhiGlyphPainter::addCoverageRect(QRectF device, const QColor& color) {
    const QRectF r = device.normalized();
    if (color.alpha() == 0 || r.isEmpty()) {
        return;
    }
    // Per axis: a partial first pixel, the whole pixels, a partial last pixel, each with the share
    // of the pixel the rectangle covers; a quad per pair, its alpha scaled by both shares.
    const auto [columns, columnCount] = coverageSpans(r.left(), r.right());
    const auto [rows, rowCount] = coverageSpans(r.top(), r.bottom());
    const QPoint texel = EditorGlyphAtlas::solidTexel();
    const auto u = static_cast<float>(texel.x()) + 0.5F;
    const auto v = static_cast<float>(texel.y()) + 0.5F;
    for (std::size_t j = 0; j < rowCount; ++j) {
        for (std::size_t i = 0; i < columnCount; ++i) {
            const double alpha = color.alphaF() * columns[i].share * rows[j].share;
            if (alpha * 255.0 < 0.5) {
                continue;
            }
            QColor shaded = color;
            shaded.setAlphaF(static_cast<float>(alpha));
            m_quads.push_back(GlyphQuad{static_cast<float>(columns[i].begin), static_cast<float>(rows[j].begin),
                                        static_cast<float>(columns[i].end - columns[i].begin),
                                        static_cast<float>(rows[j].end - rows[j].begin), u, v, u, v,
                                        packColor(shaded)});
        }
    }
}

void RhiGlyphPainter::addLine(const DisplayLine& line) {
    /*
     * The raster backend's aliased pen lines (the right margin, the preedit underline; the editor's
     * lines are axis-aligned), as Qt's raster engine draws them: a pen at most one device pixel wide
     * (cosmetic) sets the pixels from floor(start) to floor(end) on row (column) floor(y); a wider
     * one fills the line's outline, widened by half the pen on every side (square caps), with each
     * edge rounded to the pixel grid.
     */
    const auto& [segment, color, penWidth] = line;
    if (color.alpha() == 0) {
        return;
    }
    const QPointF a = segment.p1() * m_dpr;
    const QPointF b = segment.p2() * m_dpr;
    const double width = std::max(0.0, penWidth) * m_dpr;
    QRect pixels;
    if (width <= 1.0) {
        const QPoint from(static_cast<int>(std::floor(std::min(a.x(), b.x()))),
                          static_cast<int>(std::floor(std::min(a.y(), b.y()))));
        const QPoint to(static_cast<int>(std::floor(std::max(a.x(), b.x()))),
                        static_cast<int>(std::floor(std::max(a.y(), b.y()))));
        pixels = QRect(from, to);
    } else {
        const double half = width / 2.0;
        const QRectF outline(QPointF(std::min(a.x(), b.x()) - half, std::min(a.y(), b.y()) - half),
                             QPointF(std::max(a.x(), b.x()) + half, std::max(a.y(), b.y()) + half));
        pixels = snap(outline, 1.0);
    }
    if (pixels.isEmpty()) {
        return;
    }
    const QPoint texel = EditorGlyphAtlas::solidTexel();
    const auto u = static_cast<float>(texel.x()) + 0.5F;
    const auto v = static_cast<float>(texel.y()) + 0.5F;
    m_quads.push_back(GlyphQuad{static_cast<float>(pixels.x()), static_cast<float>(pixels.y()),
                                static_cast<float>(pixels.width()), static_cast<float>(pixels.height()), u, v, u, v,
                                packColor(color)});
}

bool RhiGlyphPainter::addText(const DisplayText& run, bool dropWhenFull) {
    // The semantic, inactive and highlight styling is already in the colour and `italic` (dimming
    // is the inactive colour; strike-outs are decorations); the rest is for tests.
    const auto& [origin, string, runColor, italic, highlight [[maybe_unused]], source [[maybe_unused]],
                 semantic [[maybe_unused]], inactive [[maybe_unused]]] = run;
    const Shaped& entry = shaped(string, italic);
    const std::uint32_t color = packColor(runColor);
    // QTextLayout puts the first line's top at y = 0, so its baseline is at y = ascent.
    const QPointF topLeft(origin.x(), origin.y() - entry.ascent);
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
    // The raster backend's shapes: a run marker (tests, entry points) is a right-pointing play
    // triangle in the marker's rectangle, every other marker an antialiased ellipse.
    const auto& [rect, color, severity, line [[maybe_unused]]] = marker;
    DisplayShape shape;
    shape.fill = color;
    if (severity == MarkerSeverity::RunTest) {
        shape.kind = DisplayShape::Kind::Polygon;
        shape.points = QPolygonF({rect.topLeft(), QPointF(rect.right(), rect.center().y()), rect.bottomLeft()});
    } else {
        shape.kind = DisplayShape::Kind::Ellipse;
        shape.rect = rect;
    }
    return addShape(shape, dropWhenFull);
}

bool RhiGlyphPainter::addCell(const AtlasEntry* cell, QPoint origin, const QColor& color, bool dropWhenFull) {
    if (cell == nullptr) {
        if (!dropWhenFull) {
            return false;
        }
        ++m_stats.droppedGlyphs;
        return true;
    }
    const QRect& texels = cell->rect;
    if (texels.isEmpty() || color.alpha() == 0) {
        return true;
    }
    m_quads.push_back(GlyphQuad{static_cast<float>(origin.x() + cell->offset.x()),
                                static_cast<float>(origin.y() + cell->offset.y()), static_cast<float>(texels.width()),
                                static_cast<float>(texels.height()), static_cast<float>(texels.left()),
                                static_cast<float>(texels.top()), static_cast<float>(texels.left() + texels.width()),
                                static_cast<float>(texels.top() + texels.height()), packColor(color)});
    return true;
}

bool RhiGlyphPainter::addDecoration(const DisplayDecoration& decoration, bool dropWhenFull) {
    const auto& [rect, baseline [[maybe_unused]], style, color, width, source [[maybe_unused]],
                 severity [[maybe_unused]], tooltip [[maybe_unused]]] = decoration;
    switch (style) {
    case DecorationStyle::Background: addSolid(rect, color); return true;
    case DecorationStyle::WavyUnderline:
    case DecorationStyle::DottedUnderline: return addStrokeStrip(decoration, dropWhenFull);
    case DecorationStyle::Underline:
    case DecorationStyle::Strikeout: {
        // An antialiased pen line with square caps: half a width past both ends and both sides.
        const double y =
            style == DecorationStyle::Underline ? detail::underlineY(decoration) : detail::strikeoutY(decoration);
        const double half = width / 2.0;
        addCoverageRect(QRectF(QPointF((rect.left() - half) * m_dpr, (y - half) * m_dpr),
                               QPointF((rect.right() + half) * m_dpr, (y + half) * m_dpr)),
                        color);
        return true;
    }
    }
    return true;
}

bool RhiGlyphPainter::addStrokeStrip(const DisplayDecoration& decoration, bool dropWhenFull) {
    // In device pixels; each cell is drawn by the raster backend's own code through the same
    // device-pixel-ratio scale (so pens and joins come out the same), shifted by whole pixels.
    const bool wavy = decoration.style == DecorationStyle::WavyUnderline;
    const double dpr = m_dpr;
    const double width = decoration.width * dpr;
    const double left = quantised(decoration.rect.left() * dpr);
    const double right = quantised(decoration.rect.right() * dpr);
    if (right <= left || width <= 0.0) {
        return true;
    }
    const double y = (wavy ? detail::waveCentreY(decoration) : detail::underlineY(decoration)) * dpr;
    const double period = wavy ? detail::wavePeriod(width) : detail::dotPeriod(width);
    // The cell's rows: the stroke's reach around its centre line, plus antialiasing.
    const double reach = (wavy ? detail::waveAmplitude(width) : 0.0) + width / 2.0 + 2.0;
    const int top = static_cast<int>(std::floor(y - reach));
    const int height = static_cast<int>(std::ceil(y + reach)) - top;
    const double cellY = quantised(y - top);
    // Round caps reach half a width past the ends.
    const int margin = static_cast<int>(std::ceil(width)) + 2;
    const auto begin = static_cast<std::int64_t>(std::floor(left)) - margin;
    const auto end = static_cast<std::int64_t>(std::ceil(right)) + margin;
    // Chunks of about kStripChunk pixels, a whole number of periods each where the period allows,
    // so a long squiggle's middle chunks show the same phase and share one cell.
    const int chunk =
        std::max(1, static_cast<int>(std::lround(std::max(1.0, std::floor(kStripChunk / period)) * period)));
    const double far = -(period + margin);
    const std::int64_t skipDistance = std::max<std::int64_t>(0, -static_cast<std::int64_t>(margin) - chunk - begin);
    const std::int64_t first = begin + ((skipDistance + chunk - 1) / chunk) * chunk;
    for (std::int64_t at = first; at < end; at += chunk) {
        if (at > m_pixelSize.width() + margin) {
            break; // The rest lies right of the viewport.
        }
        const int cellWidth = static_cast<int>(std::min<std::int64_t>(chunk, end - at));
        if (at + cellWidth < -margin) {
            continue; // Scrolled out to the left.
        }
        double start = left - static_cast<double>(at);
        double stop = right - static_cast<double>(at);
        // A start more than a period (and the caps) left of the chunk shows only its phase: move it
        // whole periods right, into [far - period, far). An end that far right of the chunk does
        // not show at all: one canonical end.
        if (start < far) {
            start += (std::ceil((far - start) / period) - 1.0) * period;
        }
        start = quantised(start);
        stop = quantised(std::min(stop, cellWidth + margin + period));
        const QByteArray key = cellKey(wavy ? 'W' : 'D', {dpr, decoration.width, cellY, start, stop,
                                                          static_cast<double>(height), static_cast<double>(cellWidth)});
        const AtlasEntry* cell = m_atlas.coverage(key, QSize(cellWidth, height), [&](QPainter& painter) {
            painter.scale(dpr, dpr);
            if (wavy) {
                detail::strokeWave(painter, start / dpr, stop / dpr, cellY / dpr, decoration.width, Qt::white);
            } else {
                detail::strokeDots(painter, start / dpr, stop / dpr, cellY / dpr, decoration.width, Qt::white);
            }
        });
        if (!addCell(cell, QPoint(static_cast<int>(at), top), decoration.color, dropWhenFull)) {
            return false;
        }
    }
    return true;
}

bool RhiGlyphPainter::addShape(const DisplayShape& shape, bool dropWhenFull) {
    const auto& [kind, rect, points, radius, fill, stroke, strokeWidth] = shape;
    const bool filled = fill.isValid() && fill.alpha() > 0 && kind != DisplayShape::Kind::Polyline;
    const bool stroked = stroke.isValid() && stroke.alpha() > 0 && strokeWidth > 0.0;
    if (!filled && !stroked) {
        return true;
    }
    const double dpr = m_dpr;
    const auto device = [dpr](QRectF logical) { return QRectF(logical.topLeft() * dpr, logical.size() * dpr); };
    // A plain rectangle's fill is a set of exact-coverage quads; no cell needed.
    if (kind == DisplayShape::Kind::Rect && radius <= 0.0 && !stroked) {
        addCoverageRect(device(rect), fill);
        return true;
    }
    const bool box = kind == DisplayShape::Kind::Rect || kind == DisplayShape::Kind::Ellipse;
    const QRectF bounds = box ? rect.normalized() : points.boundingRect();
    const double pen = stroked ? strokeWidth / 2.0 : 0.0;
    const QRectF reach = device(bounds.adjusted(-pen, -pen, pen, pen)).adjusted(-2.0, -2.0, 2.0, 2.0);
    // Cover only visible pixels, in bounded cells. Drawing the same full path into each cell
    // preserves rounded corners, strokes and antialiasing even for a large or zoomed shape.
    const QRectF visible = reach.intersected(QRectF(QPointF(0, 0), QSizeF(m_pixelSize)));
    if (visible.isEmpty()) {
        return true;
    }
    const int firstX = static_cast<int>(std::floor(visible.left()));
    const int firstY = static_cast<int>(std::floor(visible.top()));
    const int lastX = static_cast<int>(std::ceil(visible.right())) + 1;
    const int lastY = static_cast<int>(std::ceil(visible.bottom())) + 1;
    for (int y = firstY; y < lastY; y += kMaxCellSide) {
        for (int x = firstX; x < lastX; x += kMaxCellSide) {
            const QPoint origin(x, y);
            const QSize size(std::min(kMaxCellSide, lastX - x), std::min(kMaxCellSide, lastY - y));
            // The shape relative to the cell, in logical pixels: drawn through the same device-pixel-ratio
            // scale as the raster backend's, with its device coordinates quantised so a shape moved by
            // whole pixels (the inspection widget after a resize) finds its cell again.
            const auto local = [dpr](double logical, int offset) { return quantised(logical * dpr - offset) / dpr; };
            DisplayShape cellShape;
            cellShape.kind = kind;
            cellShape.radius = radius;
            cellShape.strokeWidth = strokeWidth;
            std::vector<double> numbers{static_cast<double>(static_cast<int>(kind)),
                                        dpr,
                                        radius,
                                        strokeWidth,
                                        static_cast<double>(size.width()),
                                        static_cast<double>(size.height())};
            if (box) {
                cellShape.rect = QRectF(QPointF(local(rect.left(), origin.x()), local(rect.top(), origin.y())),
                                        QPointF(local(rect.right(), origin.x()), local(rect.bottom(), origin.y())));
                numbers.insert(numbers.end(), {cellShape.rect.left() * dpr, cellShape.rect.top() * dpr,
                                               cellShape.rect.right() * dpr, cellShape.rect.bottom() * dpr});
            } else {
                cellShape.points.reserve(points.size());
                for (const QPointF& point : points) {
                    const QPointF at(local(point.x(), origin.x()), local(point.y(), origin.y()));
                    cellShape.points.append(at);
                    numbers.insert(numbers.end(), {at.x() * dpr, at.y() * dpr});
                }
            }
            if (filled) {
                const AtlasEntry* cell = m_atlas.coverage(cellKey('F', numbers), size, [&](QPainter& painter) {
                    painter.scale(dpr, dpr);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(Qt::white);
                    detail::drawShapeGeometry(painter, cellShape);
                });
                if (!addCell(cell, origin, fill, dropWhenFull)) {
                    return false;
                }
            }
            if (stroked) {
                const AtlasEntry* cell = m_atlas.coverage(cellKey('S', numbers), size, [&](QPainter& painter) {
                    painter.scale(dpr, dpr);
                    painter.setPen(detail::shapePen(Qt::white, strokeWidth));
                    painter.setBrush(Qt::NoBrush);
                    detail::drawShapeGeometry(painter, cellShape);
                });
                if (!addCell(cell, origin, stroke, dropWhenFull)) {
                    return false;
                }
            }
        }
    }
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
