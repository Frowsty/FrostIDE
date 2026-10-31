// SPDX-License-Identifier: Apache-2.0
//
// Headless tests of the S1 editor views (offscreen Qt platform): corpus and lexer,
// the text model and fling curve, visible-line arithmetic, the QGlyphRun cache, a
// raster frame painted into a QImage (text drawn, marker decodable by the probe's
// decoder), the glyph atlas and the RHI view's quad list. The QRhiWidget itself
// needs a GPU and the real desktop, so s1_editor exercises it on screen.

#include "frost/perf/frame_marker.hpp"

#include "corpus.hpp"
#include "views.hpp"
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace frost::s1 {
namespace {

TEST(S1Corpus, IsDeterministicPrintableAsciiWithLongLines) {
    const auto a = makeCorpus(5000, 7);
    const auto b = makeCorpus(5000, 7);
    ASSERT_EQ(a.size(), 5000u);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, makeCorpus(5000, 8));
    std::size_t longLines = 0;
    for (const std::string& line : a) {
        longLines += line.size() >= 250 ? 1 : 0;
        for (const char c : line) {
            ASSERT_TRUE(c >= 32 && c < 127) << "line: " << line;
        }
    }
    EXPECT_GT(longLines, 20u);
}

TEST(S1Corpus, ClassifiesTokens) {
    std::vector<TokenClass> classes;
    const std::string line = R"(    if (count < 42) return "x"; // done)";
    classifyLine(line, classes);
    ASSERT_EQ(classes.size(), line.size());
    EXPECT_EQ(classes[0], TokenClass::Text); // Indentation.
    EXPECT_EQ(classes[4], TokenClass::Keyword);
    EXPECT_EQ(classes[7], TokenClass::Punctuation);
    EXPECT_EQ(classes[8], TokenClass::Text);
    EXPECT_EQ(classes[line.find("42")], TokenClass::Number);
    EXPECT_EQ(classes[line.find("return")], TokenClass::Keyword);
    EXPECT_EQ(classes[line.find('"')], TokenClass::String);
    EXPECT_EQ(classes[line.find("//")], TokenClass::Comment);
    EXPECT_EQ(classes.back(), TokenClass::Comment);

    classifyLine("#include <vector>", classes);
    EXPECT_TRUE(std::ranges::all_of(classes, [](TokenClass c) { return c == TokenClass::Preprocessor; }));
}

TEST(S1Corpus, InsertRelexesAndBumpsTheRevision) {
    TextModel model({"int x;", "abc"});
    EXPECT_EQ(model.revision(1), 0u);
    EXPECT_EQ(model.insert(1, 99, '1'), 4u); // Clamped to the end.
    EXPECT_EQ(model.line(1), "abc1");
    EXPECT_EQ(model.revision(1), 1u);
    EXPECT_EQ(model.insert(0, 0, '/'), 1u);
    EXPECT_EQ(model.insert(0, 1, '/'), 2u);
    EXPECT_EQ(model.classes(0).back(), TokenClass::Comment);
}

TEST(S1Corpus, FlingDecaysExponentially) {
    const Fling fling;
    EXPECT_DOUBLE_EQ(fling.offsetPx(0.0), 0.0);
    EXPECT_NEAR(fling.velocityAt(0.0), 24000.0, 1e-9);
    EXPECT_NEAR(fling.velocityAt(10.0), 24000.0 * std::exp(-10.0 / 3.0), 1e-9);
    // Numerical derivative matches the velocity.
    EXPECT_NEAR((fling.offsetPx(5.0 + 1e-6) - fling.offsetPx(5.0)) / 1e-6, fling.velocityAt(5.0), 0.1);
    EXPECT_LT(fling.offsetPx(10.0), 24000.0 * 3.0);
}

TEST(S1Views, VisibleLinesCoverTheViewAndClampAtTheEnd) {
    ViewState state;
    state.scrollPx = 1000.4;
    auto visible = visibleLines(state, 1392, 18, 200'000);
    EXPECT_EQ(visible.first, 55u);
    EXPECT_EQ(visible.topPx, -10);
    EXPECT_GE(visible.topPx + static_cast<int>(visible.count) * 18, 1392);
    state.scrollPx = 1e12;
    visible = visibleLines(state, 100, 10, 50);
    EXPECT_EQ(visible.first, 40u);
    EXPECT_EQ(visible.count, 10u);
    EXPECT_EQ(visible.topPx, 0);
    EXPECT_EQ(visibleLines(state, 100, 10, 0).count, 0u);
}

class S1Render : public ::testing::Test {
protected:
    void SetUp() override {
        // The offscreen platform's font database starts empty; register the font
        // the views use so glyphs are real outlines, not missing-glyph boxes.
        static const int registered = QFontDatabase::addApplicationFont(
            qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")) + QStringLiteral("/Fonts/consola.ttf"));
        m_font = TextFont::make(15);
        if (registered < 0 || !m_font.raw.isValid() || m_font.lineHeight <= 0 ||
            m_font.raw.familyName() != QStringLiteral("Consolas")) {
            GTEST_SKIP() << "Consolas is not available";
        }
    }

    TextFont m_font;
    Theme m_theme;
};

TEST_F(S1Render, GlyphRunCacheHitsMissesAndRebuildsOnEdit) {
    TextModel model(makeCorpus(100, 1));
    GlyphRunCache cache(m_font, 8, 1024);
    static_cast<void>(cache.runs(model, 3));
    static_cast<void>(cache.runs(model, 3));
    EXPECT_EQ(cache.misses(), 1u);
    EXPECT_EQ(cache.hits(), 1u);
    model.insert(3, 0, 'x');
    const auto& runs = cache.runs(model, 3);
    EXPECT_EQ(cache.misses(), 2u);
    std::size_t glyphs = 0;
    for (const auto& run : runs) {
        glyphs += static_cast<std::size_t>(run.run.glyphIndexes().size());
    }
    const auto line = model.line(3);
    EXPECT_EQ(glyphs, static_cast<std::size_t>(std::ranges::count_if(line, [](char c) { return c != ' '; })));
    for (std::size_t i = 10; i < 30; ++i) {
        static_cast<void>(cache.runs(model, i)); // Evicts line 3 (capacity 8).
    }
    static_cast<void>(cache.runs(model, 3));
    EXPECT_EQ(cache.misses(), 23u);
}

// Paints a raster frame into an RGB32 image (the backing store format) and returns it.
QImage paintFrame(const TextModel& model, const TextFont& font, const Theme& theme, const ViewState& state) {
    QImage image(640, 200, QImage::Format_RGB32);
    GlyphRunCache cache(font, 64, 1024);
    QPainter painter(&image);
    paintRaster(painter, model, cache, font, theme, state, image.size(), QRegion());
    painter.end();
    return image;
}

std::size_t countNonBackground(const QImage& image, const QColor& background, int fromRow) {
    std::size_t count = 0;
    for (int y = fromRow; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            count += image.pixelColor(x, y) != background ? 1 : 0;
        }
    }
    return count;
}

TEST_F(S1Render, RasterFrameDrawsTextAndADecodableMarker) {
    TextModel model(makeCorpus(1000, 3));
    ViewState state;
    state.scrollPx = 123.0;
    state.marker = 0xBEEF;
    const QImage frame = paintFrame(model, m_font, m_theme, state);
    // RGB32 is B, G, R, X in memory: the probe's 8-bit decoder reads it directly.
    const auto decoded = perf::decodeMarkerPixels(frame.constBits(), static_cast<std::size_t>(frame.bytesPerLine()), 4);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, 0xBEEF);
    EXPECT_GT(countNonBackground(frame, m_theme.background, 4), 1000u);

    state.fillOnly = true;
    const QImage fill = paintFrame(model, m_font, m_theme, state);
    EXPECT_EQ(countNonBackground(fill, m_theme.background, 4), 0u);
    EXPECT_EQ(perf::decodeMarkerPixels(fill.constBits(), static_cast<std::size_t>(fill.bytesPerLine()), 4), 0xBEEF);
}

TEST_F(S1Render, ClippedRepaintTouchesOnlyTheDirtyRegion) {
    TextModel model(makeCorpus(1000, 3));
    ViewState state;
    QImage image(640, 200, QImage::Format_RGB32);
    image.fill(Qt::magenta);
    GlyphRunCache cache(m_font, 64, 1024);
    QPainter painter(&image);
    const QRect dirty(0, 3 * m_font.lineHeight, 640, m_font.lineHeight);
    painter.setClipRect(dirty);
    paintRaster(painter, model, cache, m_font, m_theme, state, image.size(), QRegion(dirty));
    painter.end();
    EXPECT_EQ(image.pixelColor(5, 1), QColor(Qt::magenta));
    EXPECT_NE(image.pixelColor(5, dirty.center().y()), QColor(Qt::magenta));
}

TEST_F(S1Render, AtlasHasASolidCellAndGlyphCoverage) {
    const GlyphAtlas atlas = GlyphAtlas::build(m_font);
    ASSERT_FALSE(atlas.image.isNull());
    const QRect solid = atlas.cell('\0');
    EXPECT_EQ(qAlpha(atlas.image.pixel(solid.center())), 255);
    const auto coverage = [&](char c) {
        const QRect cell = atlas.cell(c);
        int sum = 0;
        for (int y = cell.top(); y <= cell.bottom(); ++y) {
            for (int x = cell.left(); x <= cell.right(); ++x) {
                sum += qAlpha(atlas.image.pixel(x, y));
            }
        }
        return sum;
    };
    EXPECT_EQ(coverage(' '), 0);
    EXPECT_GT(coverage('M'), coverage('.'));
    EXPECT_GT(coverage('.'), 0);
    EXPECT_NE(atlas.cell('A'), atlas.cell('B'));
    EXPECT_EQ(atlas.cell('\x7f'), solid); // Out of range maps to the solid cell.
}

TEST_F(S1Render, QuadsCoverVisibleGlyphsThenTheMarker) {
    TextModel model({"int a;", "", "  x = 1;"});
    const GlyphAtlas atlas = GlyphAtlas::build(m_font);
    ViewState state;
    state.marker = 0x0001;
    std::vector<QuadInstance> quads;
    buildQuads(model, m_font, atlas, m_theme, state, QSize(640, 200), quads);
    const std::size_t glyphs = 5 + 0 + 4; // Non-space characters.
    ASSERT_EQ(quads.size(), glyphs + perf::kMarkerCells);
    const perf::MarkerCells cells = perf::encodeMarker(0x0001);
    for (int i = 0; i < perf::kMarkerCells; ++i) {
        const QuadInstance& quad = quads[glyphs + static_cast<std::size_t>(i)];
        EXPECT_EQ(quad.x, static_cast<float>(4 * i));
        EXPECT_EQ(quad.w, 4.0F);
        EXPECT_EQ(quad.rgba, cells[static_cast<std::size_t>(i)] ? 0xFFFFFFFFU : 0xFF000000U);
    }
    EXPECT_EQ(quads[5].y, static_cast<float>(2 * m_font.lineHeight)); // Line 2, column 2.
    state.fillOnly = true;
    buildQuads(model, m_font, atlas, m_theme, state, QSize(640, 200), quads);
    EXPECT_EQ(quads.size(), static_cast<std::size_t>(perf::kMarkerCells));
}

} // namespace
} // namespace frost::s1
