// SPDX-License-Identifier: Apache-2.0
//
// QRhi/raster parity of the editor backends (ADR 0043 "Parity"), offscreen: one frame built by a
// real EditorView with every DisplayElementKind in it (insight squiggles of every style, usages,
// find matches, semantic italic and inactive text, the gutter, the error stripe, the inspection
// widget with its spinner and the F2 balloon) is drawn by the QRhi painter on a Direct3D 11 WARP
// device and compared per row with the raster backend, as a whole, in crops and one kind at a time;
// the clean badge, long squiggles and the coverage cells' reuse are checked too. Where Direct3D is
// missing (Linux), QRhi's Null backend still checks that every kind gave quads. One test opens the
// view itself with the QRhi viewport on the windows platform: it runs only on a private desktop
// (the manual real-GPU check through the hidden-desktop runner) and skips elsewhere.

#include "frost/ui/editor/display_list.hpp"
#include "frost/ui/editor/editor_backend.hpp"
#include "frost/ui/editor/editor_renderer.hpp"
#include "frost/ui/editor/find_bar.hpp"
#include "frost/ui/editor/rhi_glyph_painter.hpp"
#include "frost/ui/editor/rhi_renderer.hpp"

#include "editor/editor_harness.hpp"
#include "editor/rhi_compare.hpp"
#include "support/appearance_fixture.hpp"
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QImage>
#include <QLineEdit>
#include <QPainter>

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::ui {
namespace {

using test::EditorHarness;
using test::expectSameInk;
using test::HarnessOptions;
using test::renderWithRaster;
using test::renderWithRhi;
using test::RhiFrame;
using test::TestRhi;

constexpr std::string_view kSource = "#define LIMIT 4\n"
                                     "static int counter = 0;\n"
                                     "int scaled(int factor) {\n"
                                     "    int local = factor * LIMIT;\n"
                                     "    counter = old(local);\n"
                                     "    return local + factor;\n"
                                     "}\n"
                                     "#if 0\n"
                                     "int unused = 1;\n"
                                     "#endif\n"
                                     "// local: the end of the sample\n";

// The byte range of the `nth` (0-based) occurrence of `word` in `text`.
text::ByteRange find(std::string_view text, std::string_view word, std::size_t nth = 0) {
    std::size_t at = text.find(word);
    for (std::size_t i = 0; i < nth && at != std::string_view::npos; ++i) {
        at = text.find(word, at + 1);
    }
    EXPECT_NE(at, std::string_view::npos) << word;
    return text::ByteRange{ByteOffset{at}, ByteOffset{at + word.size()}};
}

EditorDiagnostic problem(InsightSeverity severity, text::ByteRange range, std::string message) {
    EditorDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.range = range;
    diagnostic.message = std::move(message);
    return diagnostic;
}

// Delivers events until `done` holds or `ms` pass; returns `done()`.
bool pumpUntil(const std::function<bool()>& done, int ms = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

/*
 * The all-kinds frame: an EditorView over kSource in the glacier-dark theme with every insight
 * feature on. build() returns its display list, topped up with the two kinds an offscreen view
 * cannot make by itself: a caret (the view needs focus, which the find bar holds) and the preedit
 * underline (it needs an IME composition).
 */
struct AllKinds {
    explicit AllKinds(HarnessOptions options = {}) {
        AppearanceSettings settings;
        settings.theme = "glacier-dark";
        appearance = test::install(settings);
        EXPECT_NE(appearance, nullptr);
        options.path = "sample.cpp";
        options.syntax = true;
        options.appearance = appearance ? appearance->manager.get() : nullptr;
        options.settings.rightMargin = 60; // Inside the 800 px view.
        options.settings.showRightMargin = true;
        h = std::make_unique<EditorHarness>(kSource, options);
        EditorView& view = *h->view;
        const std::uint64_t version = view.editor().text().version();
        using namespace semantic_modifiers;
        EXPECT_TRUE(view.setSemanticTokens(
            version, {SemanticSpan{find(kSource, "counter"), SemanticKind::StaticField, kStatic | kDeclaration},
                      SemanticSpan{find(kSource, "factor"), SemanticKind::Parameter, kDeclaration},
                      SemanticSpan{find(kSource, "counter", 1), SemanticKind::StaticField, kStatic},
                      SemanticSpan{find(kSource, "old"), SemanticKind::Function, kDeprecated}}));
        EXPECT_TRUE(view.setInactiveRanges(version, {find(kSource, "int unused = 1;")}));
        view.setDiagnostics(EditorDiagnosticSource::Live,
                            {problem(InsightSeverity::Error, find(kSource, "old"), "use of undeclared identifier"),
                             problem(InsightSeverity::Warning, find(kSource, "LIMIT", 1), "macro expands to 4"),
                             problem(InsightSeverity::WeakWarning, find(kSource, "factor", 2), "could be const"),
                             problem(InsightSeverity::Info, find(kSource, "scaled"), "defined here")});
        view.setDecorations("parity", {EditorDecoration{find(kSource, "return"), DecorationStyle::Underline,
                                                        std::nullopt, QColor(0x56, 0xA8, 0xF5), "link"},
                                       EditorDecoration{find(kSource, "the end"), DecorationStyle::Background,
                                                        std::nullopt, QColor(0x3D, 0x5A, 0x3D), "box"}});
        view.setUsageHighlights(
            {UsageHighlight{find(kSource, "local"), true}, UsageHighlight{find(kSource, "local", 1), false}});
        view.setLineMarkers({LineMarker{LineIndex{4}, MarkerSeverity::Error, "boom"},
                             LineMarker{LineIndex{1}, MarkerSeverity::Warning, "hmm"}});
        view.setLineChanges({LineChangeMarker{LineIndex{1}, 2, ChangeStripeKind::Added},
                             LineChangeMarker{LineIndex{4}, 1, ChangeStripeKind::Modified},
                             LineChangeMarker{LineIndex{6}, 0, ChangeStripeKind::Deleted}});
        view.setAnalyzing(true); // The spinner, beside the counts and the arrows.
        view.openFind(false);
        view.findBar()->findField()->setText(QStringLiteral("factor")); // Fills and stripe marks.
        EXPECT_TRUE(view.gotoNextProblem(true));                        // The balloon.
        h->settle();
    }

    [[nodiscard]] EditorDisplayList build() const {
        QFocusEvent focus(QEvent::FocusIn, Qt::OtherFocusReason);
        QCoreApplication::sendEvent(h->view.get(), &focus); // The caret shows with focus.
        EditorDisplayList list = h->view->buildDisplayList();
        const DisplayText* end = list.findText(QStringLiteral("end"));
        if (end == nullptr) {
            end = &list.text.back();
        }
        if (list.carets.empty()) {
            list.carets.push_back(QRectF(end->origin.x(), end->origin.y() - 12.0, 2.0, 16.0));
        }
        if (list.overlays.empty()) {
            const double y = end->origin.y() + 2.0;
            list.overlays.push_back(
                DisplayLine{QLineF(end->origin.x(), y, end->origin.x() + 24.0, y), h->view->colors().foreground, 1.0});
        }
        return list;
    }

    std::unique_ptr<test::Installed> appearance; // Outlives the view.
    std::unique_ptr<EditorHarness> h;
};

// A copy of `list` that draws only `kind`: the other kinds' elements are dropped, and the gutter's
// and the stripe's backgrounds made transparent (their rectangles stay, for the kinds inside them).
EditorDisplayList isolate(const EditorDisplayList& list, DisplayElementKind kind) {
    using K = DisplayElementKind;
    EditorDisplayList out = list;
    const auto drop = [kind](K other, auto& elements) {
        if (other != kind) {
            elements.clear();
        }
    };
    drop(K::Fills, out.fills);
    std::erase_if(out.decorations, [kind](const DisplayDecoration& decoration) {
        return decoration.style == DecorationStyle::Background ? kind != K::BackgroundDecorations
                                                               : kind != K::Decorations;
    });
    drop(K::Underlines, out.underlines);
    drop(K::Text, out.text);
    drop(K::Overlays, out.overlays);
    drop(K::Carets, out.carets);
    if (kind != K::GutterBackground) {
        out.gutterBackground = Qt::transparent;
    }
    drop(K::GutterText, out.gutterText);
    drop(K::ChangeStripes, out.changeStripes);
    drop(K::Markers, out.markers);
    if (kind != K::StripeBackground) {
        out.stripeBackground = Qt::transparent;
    }
    drop(K::StripeMarks, out.stripeMarks);
    if (kind != K::StripeSummary) {
        out.stripeSummary = QRectF();
    }
    drop(K::Chrome, out.chrome);
    drop(K::ChromeText, out.chromeText);
    return out;
}

// `logical` in device pixels at `dpr`, rounded inwards by a pixel (a crop or a sample point that
// stays inside the element in both images).
QRect deviceRect(QRectF logical, double dpr) {
    const QRectF r(logical.topLeft() * dpr, logical.size() * dpr);
    return r.toAlignedRect().adjusted(1, 1, -1, -1);
}

QPoint devicePoint(QPointF logical, double dpr) { return (logical * dpr).toPoint(); }

// A pixel just inside the top-left corner of the first fill of `color` (rows' tops hold no glyph ink).
std::optional<QPointF> fillCorner(const EditorDisplayList& list, const QColor& color) {
    for (const DisplayFill& fill : list.fills) {
        if (fill.color == color && fill.rect.width() > 3.0) {
            return fill.rect.topLeft() + QPointF(1.5, 1.0);
        }
    }
    return std::nullopt;
}

TEST(RhiParity, TheAllKindsFrameHoldsEveryKindAndEveryKindGivesQuads) {
    FROST_REQUIRE_OFFSCREEN();
    const AllKinds frame;
    const EditorDisplayList list = frame.build();
    for (const DisplayElementKind kind : kDisplayPaintOrder) {
        EXPECT_GT(displayElementCount(list, kind), 0U) << toString(kind);
    }
    for (const DecorationStyle style :
         {DecorationStyle::WavyUnderline, DecorationStyle::DottedUnderline, DecorationStyle::Underline,
          DecorationStyle::Strikeout, DecorationStyle::Background}) {
        EXPECT_FALSE(list.decorationsWithStyle(style).empty()) << static_cast<int>(style);
    }
    std::set<DisplayShape::Kind> shapes;
    bool roundedAndStroked = false;
    for (const DisplayShape& shape : list.chrome) {
        shapes.insert(shape.kind);
        roundedAndStroked = roundedAndStroked || (shape.radius > 0.0 && shape.stroke.isValid());
    }
    EXPECT_EQ(shapes.size(), 4U);   // Rect, Ellipse, Polygon, Polyline.
    EXPECT_TRUE(roundedAndStroked); // The balloon's frame.
    EXPECT_EQ(list.inspection.state, InspectionState::Problems);
    EXPECT_TRUE(list.inspection.summary.analyzing);
    ASSERT_TRUE(list.balloon.has_value());
    EXPECT_TRUE(std::ranges::any_of(list.text, &DisplayText::italic));
    EXPECT_TRUE(std::ranges::any_of(list.text, &DisplayText::inactive));
    EXPECT_TRUE(std::ranges::any_of(list.text, [](const DisplayText& run) { return run.semantic.has_value(); }));
    EXPECT_TRUE(fillCorner(list, frame.h->view->colors().searchMatch).has_value());

    // The QRhi painter draws each kind (CPU half; no device needed, so Linux checks it too).
    for (const double dpr : {1.0, 1.5}) {
        RhiGlyphPainter painter(list.font);
        painter.prepare(list, dpr);
        for (const DisplayElementKind kind : kDisplayPaintOrder) {
            EXPECT_GT(painter.stats().quadsOf(kind), 0U) << toString(kind) << " at " << dpr;
        }
        EXPECT_EQ(painter.stats().droppedGlyphs, 0U);
        // Fills; the text area's kinds; the gutter, stripe and chrome.
        std::uint64_t total = 0;
        for (const std::uint64_t quads : painter.stats().kindQuads) {
            total += quads;
        }
        EXPECT_EQ(total, painter.quads().size());
    }
}

TEST(RhiParity, TheAllKindsFrameMatchesRasterPerRowWholeAndInCrops) {
    FROST_REQUIRE_OFFSCREEN();
    const AllKinds frame;
    const EditorDisplayList list = frame.build();
    const EditorColors& colors = frame.h->view->colors();
    TestRhi rhi;
    for (const double dpr : {1.0, 1.5}) {
        SCOPED_TRACE(dpr);
        const RhiFrame gpu = renderWithRhi(rhi, list, dpr);
        if (dpr == 1.0) {
            std::cout << "QRhi " << gpu.backend << " on " << gpu.adapter << std::endl;
        }
        EXPECT_EQ(gpu.stats.droppedGlyphs, 0U);
        EXPECT_EQ(gpu.stats.draws, 3U); // Fills; the text area; the gutter, stripe and chrome.
        if (gpu.image.isNull()) {
            GTEST_SKIP() << "no Direct3D 11 device (" << gpu.backend << "): quads only";
        }
        const QImage raster = renderWithRaster(list, dpr);
        std::vector<QPoint> solid{
            devicePoint(QPointF(list.textArea.right() - 30.0, list.viewport.bottom() - 10.0), dpr),
            devicePoint(QPointF(list.gutter.left() + 2.0, list.viewport.bottom() - 10.0), dpr),
            devicePoint(QPointF(list.stripe.center().x(), list.viewport.bottom() - 4.0), dpr),
            devicePoint(list.stripeSummary.center(), dpr), devicePoint(list.stripeMarks.front().rect.center(), dpr)};
        for (const QColor& color : {colors.searchMatch, colors.currentLine}) {
            if (const std::optional<QPointF> corner = fillCorner(list, color)) {
                solid.push_back(devicePoint(*corner, dpr));
            }
        }
        for (const DisplayDecoration* box : list.decorationsWithStyle(DecorationStyle::Background)) {
            solid.push_back(devicePoint(box->rect.topLeft() + QPointF(1.5, 1.0), dpr));
        }
        expectSameInk(raster, gpu.image, solid);

        // The crops: the text area (squiggles, text, the widget, the balloon), the gutter, the
        // stripe, the inspection widget and the balloon on their own.
        const struct {
            const char* name;
            QRectF rect;
            int minimumInk;
        } crops[] = {{"text area", list.textArea, 200},
                     {"gutter", list.gutter, 50},
                     {"stripe", list.stripe, 10},
                     {"inspection widget", list.inspection.rect, 30},
                     {"balloon", list.balloon->rect, 30}};
        for (const auto& crop : crops) {
            SCOPED_TRACE(crop.name);
            const QRect r = deviceRect(crop.rect, dpr);
            expectSameInk(raster.copy(r), gpu.image.copy(r), {}, crop.minimumInk);
        }
    }
}

// Each kind drawn alone at `dpr` matches the raster backend per row (one QRhi for all of them).
void expectEachKindAloneMatchesRaster(double dpr) {
    const AllKinds frame;
    const EditorDisplayList list = frame.build();
    TestRhi rhi;
    std::string pixelless;
    for (const DisplayElementKind kind : kDisplayPaintOrder) {
        SCOPED_TRACE(testing::Message() << toString(kind) << " at " << dpr);
        const EditorDisplayList alone = isolate(list, kind);
        const RhiFrame gpu = renderWithRhi(rhi, alone, dpr);
        EXPECT_GT(gpu.stats.quadsOf(kind), 0U);
        EXPECT_EQ(gpu.stats.quads, gpu.stats.quadsOf(kind));
        if (gpu.image.isNull()) {
            pixelless = gpu.backend; // No Direct3D 11 device: the quads only.
            continue;
        }
        // Kinds in the theme's quiet colours (the gutter and stripe backgrounds, the current
        // line, the right margin) leave no ink; the rows must still agree.
        expectSameInk(renderWithRaster(alone, dpr), gpu.image, {}, 0);
    }
    if (!pixelless.empty()) {
        GTEST_SKIP() << "no Direct3D 11 device (" << pixelless << "): quads only";
    }
}

TEST(RhiParity, EachKindAloneMatchesRasterPerRowAt100) {
    FROST_REQUIRE_OFFSCREEN();
    expectEachKindAloneMatchesRaster(1.0);
}

TEST(RhiParity, EachKindAloneMatchesRasterPerRowAt150) {
    FROST_REQUIRE_OFFSCREEN();
    expectEachKindAloneMatchesRaster(1.5);
}

TEST(RhiParity, TheCleanBadgeAndLongSquigglesMatchRaster) {
    FROST_REQUIRE_OFFSCREEN();
    AppearanceSettings settings;
    settings.theme = "glacier-dark";
    const auto appearance = test::install(settings);
    ASSERT_NE(appearance, nullptr);
    HarnessOptions options;
    options.appearance = appearance->manager.get();
    // A whole-line build error (column 0) across a line wider than the view: a squiggle of many
    // chunks; and the clean check mark in a second frame.
    const std::string line = "int value = compute(" + std::string(150, 'x') + ");\n";
    EditorHarness h(line + "next\n", options);
    EditorDiagnostic build;
    build.severity = InsightSeverity::Error;
    build.message = "C2065: undeclared identifier";
    build.line = 1;
    h.view->setDiagnostics(EditorDiagnosticSource::Build, {build});
    const EditorDisplayList squiggly = h.view->buildDisplayList();
    const std::vector<const DisplayDecoration*> waves = squiggly.decorationsWithStyle(DecorationStyle::WavyUnderline);
    ASSERT_EQ(waves.size(), 1U);
    EXPECT_GT(waves.front()->rect.right(), squiggly.textArea.right()); // Past the view's right edge.

    h.view->setDiagnostics(EditorDiagnosticSource::Build, {});
    h.view->setInsightEnabled(true);
    const EditorDisplayList clean = h.view->buildDisplayList();
    ASSERT_EQ(clean.inspection.state, InspectionState::Clean);
    ASSERT_TRUE(std::ranges::any_of(clean.chrome, [](const DisplayShape& shape) {
        return shape.kind == DisplayShape::Kind::Polyline;
    })); // The check mark.

    TestRhi rhi;
    for (const double dpr : {1.0, 1.5}) {
        SCOPED_TRACE(dpr);
        for (const EditorDisplayList* list : {&squiggly, &clean}) {
            const RhiFrame gpu = renderWithRhi(rhi, *list, dpr);
            EXPECT_GT(gpu.stats.quadsOf(DisplayElementKind::Chrome), 0U);
            EXPECT_EQ(gpu.stats.droppedGlyphs, 0U);
            if (gpu.image.isNull()) {
                GTEST_SKIP() << "no Direct3D 11 device (" << gpu.backend << "): quads only";
            }
            const QImage raster = renderWithRaster(*list, dpr);
            expectSameInk(raster, gpu.image, {});
            const QRect widget = deviceRect(list->inspection.rect, dpr);
            expectSameInk(raster.copy(widget), gpu.image.copy(widget), {}, 10);
        }
    }
}

TEST(RhiParity, OversizedChromeIsClippedAndDrawnWithoutDroppingShapes) {
    FROST_REQUIRE_OFFSCREEN();
    EditorDisplayList list;
    list.viewport = QRectF(0, 0, 800, 180);
    list.textArea = list.viewport;
    list.background = QColor(30, 31, 34);
    list.font = QFont(QStringLiteral("JetBrains Mono"), 11);
    DisplayShape rounded;
    rounded.kind = DisplayShape::Kind::Rect;
    rounded.rect = QRectF(5, 5, 2500, 70);
    rounded.radius = 16;
    rounded.fill = QColor(120, 180, 230);
    rounded.stroke = QColor(245, 180, 100);
    rounded.strokeWidth = 2;
    list.chrome.push_back(rounded);
    DisplayShape ellipse;
    ellipse.kind = DisplayShape::Kind::Ellipse;
    ellipse.rect = QRectF(-2100, 95, 2400, 70);
    ellipse.fill = QColor(180, 100, 220);
    list.chrome.push_back(ellipse);
    TestRhi rhi;
    const RhiFrame gpu = renderWithRhi(rhi, list, 1.0);
    EXPECT_EQ(gpu.stats.droppedGlyphs, 0U);
    EXPECT_GE(gpu.stats.quadsOf(DisplayElementKind::Chrome), 3U);
    if (gpu.image.isNull()) {
        GTEST_SKIP() << "no Direct3D 11 device (" << gpu.backend << "): quads only";
    }
    expectSameInk(renderWithRaster(list, 1.0), gpu.image, {}, 20);
}

TEST(RhiParity, FarOffscreenSquiggleKeepsPhaseAndOnlyBuildsVisibleChunks) {
    FROST_REQUIRE_OFFSCREEN();
    EditorDisplayList list;
    list.viewport = QRectF(0, 0, 300, 40);
    list.textArea = list.viewport;
    list.background = QColor(30, 31, 34);
    list.font = QFont(QStringLiteral("JetBrains Mono"), 11);
    list.decorations.push_back(DisplayDecoration{QRectF(-1.0e12, 0, 1.0e12 + 200, 20),
                                                 14.0,
                                                 DecorationStyle::WavyUnderline,
                                                 QColor(240, 110, 110),
                                                 1.0,
                                                 text::ByteRange{},
                                                 std::nullopt,
                                                 {}});
    RhiGlyphPainter painter(list.font);
    painter.prepare(list, 1.0);
    EXPECT_LE(painter.stats().quadsOf(DisplayElementKind::Decorations), 3U);
    EXPECT_EQ(painter.stats().droppedGlyphs, 0U);
    TestRhi rhi;
    const RhiFrame distantFrame = renderWithRhi(rhi, list, 1.0);
    list.decorations.front().rect = QRectF(-256, 0, 456, 20);
    const RhiFrame nearbyFrame = renderWithRhi(rhi, list, 1.0);
    if (distantFrame.image.isNull() || nearbyFrame.image.isNull()) {
        GTEST_SKIP() << "no Direct3D 11 device: quads only";
    }
    EXPECT_EQ(distantFrame.image, nearbyFrame.image);
}

TEST(RhiParity, TranslucentChromeHasNoTileSeamsAtFractionalDpr) {
    FROST_REQUIRE_OFFSCREEN();
    EditorDisplayList list;
    list.viewport = QRectF(0, 0, 800, 120);
    list.textArea = list.viewport;
    list.background = QColor(30, 31, 34);
    list.font = QFont(QStringLiteral("JetBrains Mono"), 11);
    DisplayShape shape;
    shape.kind = DisplayShape::Kind::Rect;
    shape.rect = QRectF(5.25, 10.25, 730, 70);
    shape.radius = 8;
    shape.fill = QColor(120, 180, 230, 144);
    shape.stroke = QColor(240, 150, 100, 170);
    shape.strokeWidth = 2;
    list.chrome.push_back(shape);
    constexpr double dpr = 1.5;
    TestRhi rhi;
    const RhiFrame gpu = renderWithRhi(rhi, list, dpr);
    EXPECT_EQ(gpu.stats.droppedGlyphs, 0U);
    if (gpu.image.isNull()) {
        GTEST_SKIP() << "no Direct3D 11 device: quads only";
    }
    const QImage raster = renderWithRaster(list, dpr);
    // Tiles start at floor((left - half stroke) * dpr - 2); compare both sides of each join,
    // including the antialiased top and bottom strokes and the translucent interior.
    const int origin = static_cast<int>(std::floor((shape.rect.left() - shape.strokeWidth / 2) * dpr - 2));
    for (const int seam : {origin + 512, origin + 1024}) {
        for (int y = 12; y < 125; ++y) {
            for (int x = seam - 1; x <= seam + 1; ++x) {
                const QColor a = raster.pixelColor(x, y);
                const QColor b = gpu.image.pixelColor(x, y);
                EXPECT_LE(std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()),
                                    std::abs(a.blue() - b.blue())}),
                          3)
                    << x << "," << y;
            }
        }
    }
}

TEST(RhiParity, CoverageCellsAreSharedAndReused) {
    FROST_REQUIRE_OFFSCREEN();
    const AllKinds frame;
    EditorDisplayList list = frame.build();
    RhiGlyphPainter painter(list.font);
    painter.prepare(list, 1.0);
    const GlyphAtlasStats first = painter.atlas().stats();
    // The same frame again rasterises nothing.
    painter.prepare(list, 1.0);
    EXPECT_EQ(painter.atlas().stats().rasterised, first.rasterised);
    // The chrome moved by whole pixels (the view grew) finds its cells again.
    for (DisplayShape& shape : list.chrome) {
        shape.rect.translate(7.0, 0.0);
        shape.points.translate(7.0, 0.0);
    }
    for (DisplayText& run : list.chromeText) {
        run.origin += QPointF(7.0, 0.0);
    }
    painter.prepare(list, 1.0);
    EXPECT_EQ(painter.atlas().stats().rasterised, first.rasterised);

    // A 3000 px squiggle is a dozen chunks, but its middle chunks share one cell.
    EditorDisplayList wide;
    wide.viewport = QRectF(0, 0, 3200, 40);
    wide.textArea = wide.viewport;
    wide.background = QColor(30, 31, 34);
    wide.font = list.font;
    wide.decorations.push_back(DisplayDecoration{QRectF(10.25, 0, 3000, 20),
                                                 14.0,
                                                 DecorationStyle::WavyUnderline,
                                                 QColor(240, 110, 110),
                                                 1.0,
                                                 text::ByteRange{},
                                                 std::nullopt,
                                                 {}});
    RhiGlyphPainter strips(list.font);
    strips.prepare(wide, 1.0);
    EXPECT_GE(strips.stats().quadsOf(DisplayElementKind::Decorations), 12U);
    EXPECT_LE(strips.atlas().stats().glyphs, 4U); // The head, the shared middle, the tail.
}

#ifdef _WIN32
// Whether this thread's desktop is a private one (the hidden-desktop runner's), not the user's.
bool onPrivateDesktop() {
    wchar_t name[256] = {};
    DWORD needed = 0;
    HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    if (desktop == nullptr || !GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed)) {
        return false;
    }
    return std::wstring_view(name) != L"Default";
}
#else
bool onPrivateDesktop() { return false; }
#endif

// The manual real-GPU check (ADR 0043 "Parity"): the view's own QRhiWidget viewport on the
// default (hardware) adapter draws the all-kinds frame like the raster backend. Run it through the
// hidden-desktop runner with QT_QPA_PLATFORM=windows; anywhere else it skips, so no window can
// reach the user's desktop.
TEST(RhiParity, TheShownQRhiViewportMatchesRasterOnTheWindowsPlatform) {
    if (QGuiApplication::platformName() != QStringLiteral("windows") || !onPrivateDesktop()) {
        GTEST_SKIP() << "the manual real-GPU check: needs QT_QPA_PLATFORM=windows on a private desktop";
    }
    HarnessOptions options;
    options.settings.renderer = "rhi";
    const AllKinds frame(options);
    EditorView& view = *frame.h->view;
    ASSERT_TRUE(pumpUntil([&] {
        const auto* surface = qobject_cast<RhiEditorViewport*>(view.viewport());
        return view.rendererState().active == EditorBackend::Rhi && surface != nullptr &&
               !surface->adapterName().isEmpty();
    })) << view.rendererState().reason;
    auto* surface = qobject_cast<RhiEditorViewport*>(view.viewport());
    ASSERT_NE(surface, nullptr);
    const EditorDisplayList list = frame.build();
    surface->setFrame(list);
    QImage gpu = surface->grabFramebuffer().convertToFormat(QImage::Format_ARGB32);
    ASSERT_FALSE(gpu.isNull()) << "grabFramebuffer failed";
    gpu.setDevicePixelRatio(1.0);
    const double dpr = surface->devicePixelRatioF();
    std::cout << "QRhi viewport adapter: " << surface->adapterName().toStdString() << ", dpr " << dpr << ", fallbacks "
              << view.rendererState().fallbacks << '\n';
    RecordProperty("adapter", surface->adapterName().toStdString());
    EXPECT_FALSE(surface->hasFailed());
    EXPECT_EQ(view.rendererState().fallbacks, 0U);
    const QImage raster = renderWithRaster(list, dpr);
    ASSERT_EQ(raster.size(), gpu.size());
    expectSameInk(raster, gpu, {devicePoint(list.stripeSummary.center(), dpr)});
    const QRect widget = deviceRect(list.inspection.rect, dpr);
    expectSameInk(raster.copy(widget), gpu.copy(widget), {}, 30);
    const QRect text = deviceRect(list.textArea, dpr);
    expectSameInk(raster.copy(text), gpu.copy(text), {});
}

} // namespace
} // namespace frost::ui
