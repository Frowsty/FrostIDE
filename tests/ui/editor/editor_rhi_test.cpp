// SPDX-License-Identifier: Apache-2.0
//
// Tests of the QRhi editor backend and the renderer choice (ADR 0043), offscreen: the
// editor.renderer rule with injected RDP and WARP sessions, the glyph atlas (caching, growth, a
// rebuild on a font-size or device-pixel-ratio change), the QRhi painter rendered into an offscreen
// texture (Direct3D 11 on WARP, or QRhi's Null backend where Direct3D is missing) compared coarsely
// with the raster backend's image, and the view's live switches and fallbacks, which must keep the
// document, the caret and the selection.

#include "frost/io/mem_fs.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/registry.hpp"
#include "frost/settings/store.hpp"
#include "frost/settings/value.hpp"
#include "frost/ui/editor/editor_backend.hpp"
#include "frost/ui/editor/editor_renderer.hpp"
#include "frost/ui/editor/editor_settings.hpp"
#include "frost/ui/editor/glyph_atlas.hpp"
#include "frost/ui/editor/rhi_glyph_painter.hpp"
#include "frost/ui/editor/rhi_renderer.hpp"

#include "editor/editor_harness.hpp"
#include <QElapsedTimer>
#include <QImage>
#include <QRawFont>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace frost::ui {
namespace {

using test::EditorHarness;
using test::HarnessOptions;

// A Windows desktop session as the rule sees it (the offscreen test platform reports none).
RendererEnvironment windowsDesktop() {
    RendererEnvironment environment;
    environment.rhiAvailable = true;
    environment.platform = "windows";
    return environment;
}

// Delivers events until `done` holds or two seconds pass; returns `done()`.
bool pumpUntil(const std::function<bool()>& done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

// Delivers events for a short while (queued renderer switches, paints).
void pump() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 150) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

QFont editorFont(double points) {
    test::ensureEditorFonts();
    QFont font(QStringLiteral("JetBrains Mono"));
    font.setStyleHint(QFont::TypeWriter);
    font.setPointSizeF(points);
    font.setKerning(false);
    return font;
}

void selectRange(EditorView& view, std::uint64_t anchor, std::uint64_t head) {
    const text::Selection selection{ByteOffset{anchor}, ByteOffset{head}};
    view.setState(text::EditorState::from(text::SelectionSet::fromSelections(std::span(&selection, 1), 0)));
}

// --- the rule ---

TEST(EditorRendererRule, AutoPicksRhiOnAWindowsDesktopAndRasterOverRdpOrWarp) {
    const RendererEnvironment desktop = windowsDesktop();
    EXPECT_EQ(chooseEditorBackend(EditorRendererMode::Auto, desktop).backend, EditorBackend::Rhi);

    RendererEnvironment rdp = desktop;
    rdp.remoteSession = true;
    const BackendChoice remote = chooseEditorBackend(EditorRendererMode::Auto, rdp);
    EXPECT_EQ(remote.backend, EditorBackend::Raster);
    EXPECT_NE(remote.reason.find("remote session"), std::string::npos) << remote.reason;

    RendererEnvironment warp = desktop;
    warp.softwareAdapter = true;
    const BackendChoice software = chooseEditorBackend(EditorRendererMode::Auto, warp);
    EXPECT_EQ(software.backend, EditorBackend::Raster);
    EXPECT_NE(software.reason.find("software adapter"), std::string::npos) << software.reason;

    RendererEnvironment xcb; // Linux: raster until a Linux S1 leg exists (ADR 0007 decision 8).
    xcb.platform = "xcb";
    const BackendChoice none = chooseEditorBackend(EditorRendererMode::Auto, xcb);
    EXPECT_EQ(none.backend, EditorBackend::Raster);
    EXPECT_NE(none.reason.find("xcb"), std::string::npos) << none.reason;
}

TEST(EditorRendererRule, ExplicitModesWinOverTheSessionButNotOverAMissingBackend) {
    RendererEnvironment rdp = windowsDesktop();
    rdp.remoteSession = true;
    rdp.softwareAdapter = true;
    EXPECT_EQ(chooseEditorBackend(EditorRendererMode::Rhi, rdp).backend, EditorBackend::Rhi);
    EXPECT_EQ(chooseEditorBackend(EditorRendererMode::Raster, windowsDesktop()).backend, EditorBackend::Raster);
    EXPECT_EQ(chooseEditorBackend(EditorRendererMode::Rhi, RendererEnvironment{}).backend, EditorBackend::Raster);
}

TEST(EditorRendererRule, ParsingAdaptersAndTheOffscreenPlatform) {
    EXPECT_EQ(parseEditorRendererMode("auto"), EditorRendererMode::Auto);
    EXPECT_EQ(parseEditorRendererMode("raster"), EditorRendererMode::Raster);
    EXPECT_EQ(parseEditorRendererMode("rhi"), EditorRendererMode::Rhi);
    EXPECT_FALSE(parseEditorRendererMode("vulkan").has_value());
    EXPECT_EQ(toString(EditorBackend::Rhi), "rhi");
    EXPECT_TRUE(isSoftwareAdapter(0x1414, 0x8c, false));    // WARP.
    EXPECT_TRUE(isSoftwareAdapter(0, 0, true));             // Reported as a CPU device.
    EXPECT_FALSE(isSoftwareAdapter(0x10de, 0x2783, false)); // A discrete GPU.
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        EXPECT_FALSE(detectRendererEnvironment().rhiAvailable);
    }
}

TEST(EditorRendererRule, TheSettingIsRegisteredWithItsSchema) {
    const settings::KeySpec* spec = settings::builtinRegistry().find(settings::key::editorRenderer.name);
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(std::get<std::string>(spec->defaultValue), "auto");
    for (const char* value : {"auto", "raster", "rhi"}) {
        EXPECT_TRUE(settings::validateValue(spec->schema, settings::Value(std::string(value))).has_value()) << value;
    }
    EXPECT_FALSE(settings::validateValue(spec->schema, settings::Value(std::string("gpu"))).has_value());
    EXPECT_EQ(EditorViewSettings{}.renderer, "auto");
}

// --- the atlas ---

TEST(EditorGlyphAtlas, RasterisesAGlyphOnceAndKeepsCellsWhenItGrows) {
    EditorGlyphAtlas atlas(64, 256);
    const QRawFont raw = QRawFont::fromFont(editorFont(13.0));
    ASSERT_TRUE(raw.isValid());
    const int slot = atlas.fontSlot(raw);
    EXPECT_EQ(atlas.fontSlot(raw), slot);
    const QList<quint32> glyphs = raw.glyphIndexesForString(QStringLiteral("M "));
    ASSERT_EQ(glyphs.size(), 2);

    const AtlasEntry* m = atlas.glyph(slot, glyphs[0], 0);
    ASSERT_NE(m, nullptr);
    EXPECT_FALSE(m->rect.isEmpty());
    EXPECT_LT(m->offset.y(), 0); // The ink sits above the baseline.
    const QRect mRect = m->rect;
    EXPECT_EQ(atlas.glyph(slot, glyphs[0], 0)->rect, mRect);
    const AtlasEntry* space = atlas.glyph(slot, glyphs[1], 0);
    ASSERT_NE(space, nullptr);
    EXPECT_TRUE(space->rect.isEmpty());
    EXPECT_EQ(atlas.stats().rasterised, 2U);

    // Discs fill the 64-texel atlas until it doubles; the glyph's texels move with it.
    const QRgb before = atlas.image().pixel(mRect.center());
    for (int diameter = 20; diameter < 40 && atlas.size() < 128; ++diameter) {
        ASSERT_NE(atlas.disc(diameter), nullptr);
    }
    EXPECT_EQ(atlas.size(), 128);
    EXPECT_EQ(atlas.image().pixel(mRect.center()), before);
    EXPECT_EQ(atlas.glyph(slot, glyphs[0], 0)->rect, mRect);

    // Past its maximum it reports full instead of growing.
    EditorGlyphAtlas tiny(64, 64);
    EXPECT_EQ(tiny.disc(200), nullptr);
}

// A small display list: two text runs and a gutter number, as the view would build them.
EditorDisplayList sampleList(const QFont& font) {
    EditorDisplayList list;
    list.viewport = QRectF(0, 0, 320, 80);
    list.background = QColor(30, 31, 34);
    list.gutter = QRectF(0, 0, 40, 80);
    list.gutterBackground = QColor(40, 41, 44);
    list.textArea = QRectF(40, 0, 280, 80);
    list.font = font;
    const auto run = [](QPointF origin, const char* text, QColor color) {
        return DisplayText{origin,       QString::fromLatin1(text), color,        false,
                           std::nullopt, text::ByteRange{},         std::nullopt, false};
    };
    list.text.push_back(run(QPointF(46, 16), "int main()", QColor(200, 200, 200)));
    list.text.push_back(run(QPointF(46, 36), "return 42;", QColor(110, 170, 220)));
    list.gutterText.push_back(run(QPointF(10, 16), "1", QColor(120, 120, 120)));
    return list;
}

TEST(RhiGlyphPainter, FontSizeAndPixelRatioChangesRebuildTheAtlas) {
    RhiGlyphPainter painter(editorFont(13.0));
    const EditorDisplayList list = sampleList(painter.font());
    painter.prepare(list, 1.0);
    const GlyphAtlasStats first = painter.atlas().stats();
    EXPECT_GT(first.glyphs, 0U);
    ASSERT_GT(painter.stats().glyphQuads, 0U);
    const auto tallest = [&painter] {
        float height = 0.0F;
        for (const GlyphQuad& quad : painter.quads()) {
            if (quad.u0 != quad.u1) { // Glyph quads sample a texel range; solid ones one texel.
                height = std::max(height, quad.h);
            }
        }
        return height;
    };
    const float small = tallest();

    // The same frame again rasterises nothing new.
    painter.prepare(list, 1.0);
    EXPECT_EQ(painter.atlas().stats().rasterised, first.rasterised);
    EXPECT_EQ(painter.atlas().stats().rebuilds, first.rebuilds);

    // A bigger font (settings, zoom) resets the atlas and draws bigger glyphs.
    painter.setFont(editorFont(26.0));
    EXPECT_EQ(painter.atlas().stats().rebuilds, first.rebuilds + 1);
    EXPECT_EQ(painter.atlas().stats().glyphs, 0U);
    painter.prepare(list, 1.0);
    const float big = tallest();
    EXPECT_GT(big, small * 1.5F);

    // A new device pixel ratio (DPI change, monitor move) resets it too.
    painter.setFont(editorFont(13.0));
    const std::uint64_t rebuilds = painter.atlas().stats().rebuilds;
    painter.prepare(list, 2.0);
    EXPECT_EQ(painter.atlas().stats().rebuilds, rebuilds + 1);
    EXPECT_DOUBLE_EQ(painter.atlas().devicePixelRatio(), 2.0);
    EXPECT_GT(tallest(), small * 1.5F);
}

TEST(RhiGlyphPainter, TheRendererForwardsFontChangesToTheViewport) {
    RhiEditorViewport viewport(editorFont(13.0)); // Never shown: no QRhi is created.
    RhiEditorRenderer renderer(viewport);
    EXPECT_EQ(renderer.name(), "rhi");
    const std::uint64_t rebuilds = viewport.painter().atlas().stats().rebuilds;
    renderer.setFont(editorFont(20.0));
    EXPECT_EQ(viewport.painter().atlas().stats().rebuilds, rebuilds + 1);
    EXPECT_DOUBLE_EQ(viewport.painter().font().pointSizeF(), 20.0);
}

// --- rendering against the raster backend ---

// Per-row ink of an image: pixels that differ from the row's most common colour by more than 48 in
// a channel, and the leftmost and rightmost of them.
struct RowInk {
    int count = 0;
    int left = 0;
    int right = 0;
};

std::vector<RowInk> inkRows(const QImage& source) {
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    std::vector<RowInk> rows(static_cast<std::size_t>(image.height()));
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        QHash<QRgb, int> counts;
        for (int x = 0; x < image.width(); ++x) {
            ++counts[line[x]];
        }
        QRgb mode = line[0];
        for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
            if (it.value() > counts.value(mode)) {
                mode = it.key();
            }
        }
        RowInk& row = rows[static_cast<std::size_t>(y)];
        row.left = image.width();
        row.right = -1;
        for (int x = 0; x < image.width(); ++x) {
            const QRgb p = line[x];
            const int diff = std::max(
                {std::abs(qRed(p) - qRed(mode)), std::abs(qGreen(p) - qGreen(mode)), std::abs(qBlue(p) - qBlue(mode))});
            if (diff > 48) {
                ++row.count;
                row.left = std::min(row.left, x);
                row.right = std::max(row.right, x);
            }
        }
    }
    return rows;
}

// Whether `rows` has ink within one row of `y`.
bool inkNear(const std::vector<RowInk>& rows, std::size_t y) {
    for (std::size_t r = y == 0 ? 0 : y - 1; r <= y + 1 && r < rows.size(); ++r) {
        if (rows[r].count > 0) {
            return true;
        }
    }
    return false;
}

// A QRhi for offscreen rendering: Direct3D 11 on the WARP software adapter where it exists, so
// the test needs no GPU and gives the same pixels on every machine, else the Null backend.
std::unique_ptr<QRhi> createTestRhi(bool& draws) {
    draws = false;
#ifdef _WIN32
    QRhiD3D11InitParams d3d;
    if (QRhi* rhi = QRhi::create(QRhi::D3D11, &d3d, QRhi::PreferSoftwareRenderer)) {
        draws = true;
        return std::unique_ptr<QRhi>(rhi);
    }
#endif
    QRhiNullInitParams null;
    return std::unique_ptr<QRhi>(QRhi::create(QRhi::Null, &null));
}

// What one offscreen QRhi frame gave: the image (empty on the Null backend) and the painter's stats.
struct RhiFrame {
    QImage image;
    RhiFrameStats stats;
    std::string backend;
};

// Renders `list` at `dpr` into a fresh offscreen texture of the list's size times `dpr` and reads
// it back. Fails the test (and returns an empty frame) when the QRhi objects cannot be created.
RhiFrame renderWithRhi(const EditorDisplayList& list, double dpr) {
    RhiFrame frame;
    bool draws = false;
    std::unique_ptr<QRhi> rhi = createTestRhi(draws);
    if (!rhi) {
        ADD_FAILURE() << "neither Direct3D 11 (WARP) nor QRhi's Null backend could be created";
        return frame;
    }
    frame.backend = rhi->backendName();
    const QSize size = (list.viewport.size() * dpr).toSize();
    std::unique_ptr<QRhiTexture> texture(
        rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    EXPECT_TRUE(texture->create());
    std::unique_ptr<QRhiTextureRenderTarget> target(rhi->newTextureRenderTarget({texture.get()}));
    std::unique_ptr<QRhiRenderPassDescriptor> pass(target->newCompatibleRenderPassDescriptor());
    target->setRenderPassDescriptor(pass.get());
    EXPECT_TRUE(target->create());

    RhiGlyphPainter painter(list.font);
    const Result<void> ready = painter.initialize(*rhi, *pass);
    EXPECT_TRUE(ready.has_value()) << ready.error().describe();
    QRhiCommandBuffer* cb = nullptr;
    if (!ready || rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) {
        ADD_FAILURE() << "could not start an offscreen frame";
        return frame;
    }
    painter.prepare(list, dpr);
    painter.record(*cb, *target);
    QRhiReadbackResult readback;
    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
    batch->readBackTexture({texture.get()}, &readback);
    cb->resourceUpdate(batch);
    EXPECT_EQ(rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
    painter.releaseResources(); // Before the QRhi goes.
    frame.stats = painter.stats();
    if (draws && readback.pixelSize == size) {
        frame.image = QImage(reinterpret_cast<const uchar*>(readback.data.constData()), size.width(), size.height(),
                             size.width() * 4, QImage::Format_RGBA8888_Premultiplied)
                          .convertToFormat(QImage::Format_ARGB32);
    }
    return frame;
}

// The raster backend's image of `list` at `dpr` (the golden oracle).
QImage renderWithRaster(const EditorDisplayList& list, double dpr) {
    QImage image((list.viewport.size() * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    RasterEditorRenderer renderer(list.font);
    renderer.render(list, image);
    image.setDevicePixelRatio(1.0);
    return image.convertToFormat(QImage::Format_ARGB32);
}

// Coarse agreement of two renderings of one list (not a golden): solid colours at a few points
// match within 2 levels; every inked row of one image has ink within a row in the other; inked
// spans start and end within 3 px; the total ink is within 30 %.
void expectSameInk(const QImage& raster, const QImage& gpu, const std::vector<QPoint>& solidPoints) {
    ASSERT_EQ(raster.size(), gpu.size());
    for (const QPoint& at : solidPoints) {
        const QRgb a = raster.pixel(at);
        const QRgb b = gpu.pixel(at);
        EXPECT_LE(
            std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))}), 2)
            << "at " << at.x() << "," << at.y();
    }
    const std::vector<RowInk> want = inkRows(raster);
    const std::vector<RowInk> got = inkRows(gpu);
    int wantTotal = 0;
    int gotTotal = 0;
    for (std::size_t y = 0; y < want.size(); ++y) {
        wantTotal += want[y].count;
        gotTotal += got[y].count;
        if (want[y].count > 0) {
            EXPECT_TRUE(inkNear(got, y)) << "raster ink in row " << y << " missing from the QRhi image";
        }
        if (got[y].count > 0) {
            EXPECT_TRUE(inkNear(want, y)) << "QRhi ink in row " << y << " where the raster image has none";
        }
        if (want[y].count > 0 && got[y].count > 0) {
            EXPECT_LE(std::abs(want[y].left - got[y].left), 3) << "row " << y;
            EXPECT_LE(std::abs(want[y].right - got[y].right), 3) << "row " << y;
        }
    }
    EXPECT_GT(wantTotal, 200);
    EXPECT_NEAR(gotTotal, wantTotal, wantTotal * 0.3);
}

TEST(RhiGlyphPainter, RendersTheDisplayListWhereTheRasterBackendDoes) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("int main() {\n    return 42;\n}\n// a longer comment line, to the right\n\nend\n");
    selectRange(*h.view, 4, 8); // "main"
    h.view->setLineMarkers({LineMarker{LineIndex{1}, MarkerSeverity::Error, "boom"}});
    const EditorDisplayList list = h.view->buildDisplayList();
    ASSERT_FALSE(list.text.empty());
    ASSERT_FALSE(list.markers.empty());
    const DisplayFill* selection = nullptr;
    for (const DisplayFill& fill : list.fills) {
        if (fill.color == h.view->colors().selection) {
            selection = &fill;
        }
    }
    ASSERT_NE(selection, nullptr);

    // 100 % and 150 % (a HiDPI monitor): the atlas is rasterised at the ratio either way.
    for (const double dpr : {1.0, 1.5}) {
        SCOPED_TRACE(dpr);
        const RhiFrame frame = renderWithRhi(list, dpr);
        EXPECT_GT(frame.stats.glyphQuads, 20U);
        EXPECT_EQ(frame.stats.droppedGlyphs, 0U);
        EXPECT_EQ(frame.stats.draws, 3U); // Fills, the text area, the gutter.
        EXPECT_GE(frame.stats.textureUploads, 1U);
        if (frame.image.isNull()) {
            GTEST_SKIP() << "no Direct3D 11 device (" << frame.backend
                         << "): the quads were built and recorded, but the Null backend draws no pixels";
        }
        const QSize size = frame.image.size();
        const QPoint inSelection = (selection->rect.center() * dpr).toPoint();
        expectSameInk(renderWithRaster(list, dpr), frame.image,
                      {QPoint(size.width() - 30, size.height() - 10), QPoint(2, size.height() - 10), inSelection});
    }
}

// --- the view ---

TEST(EditorRenderer, OffscreenAutoStaysOnRaster) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("text\n");
    pump();
    EXPECT_EQ(h.view->rendererState().active, EditorBackend::Raster);
    EXPECT_FALSE(h.view->rendererState().pending);
    EXPECT_EQ(h.view->renderer().name(), "raster");
    EXPECT_NE(h.view->rendererState().reason.find("no QRhi editor backend"), std::string::npos)
        << h.view->rendererState().reason;
}

TEST(EditorRenderer, AutoKeepsRasterInARemoteSession) {
    FROST_REQUIRE_OFFSCREEN();
    HarnessOptions options;
    RendererEnvironment rdp = windowsDesktop();
    rdp.remoteSession = true;
    options.renderer.environment = rdp;
    EditorHarness h("text\n", options);
    pump();
    EXPECT_EQ(h.view->rendererState().active, EditorBackend::Raster);
    EXPECT_EQ(h.view->rendererState().fallbacks, 0U);
    EXPECT_NE(h.view->rendererState().reason.find("remote session"), std::string::npos);
}

TEST(EditorRenderer, ForcedCreationFailureFallsBackAndKeepsTheModel) {
    FROST_REQUIRE_OFFSCREEN();
    HarnessOptions options;
    options.renderer.environment = windowsDesktop();
    options.renderer.failRhiCreation = true;
    options.show = false;
    EditorHarness h("first line\nsecond line\nthird line\n", options);
    // QRhi is chosen, but waits for the first (raster) frame.
    EXPECT_EQ(h.view->rendererState().active, EditorBackend::Raster);
    EXPECT_TRUE(h.view->rendererState().pending);
    EXPECT_EQ(h.view->rendererState().reason, "auto: QRhi (Direct3D 11)");

    selectRange(*h.view, 11, 17); // "second"
    const text::EditorState before = h.view->editor().state();
    const std::string text = h.text();
    std::vector<EditorBackend> seen;
    QObject::connect(h.view.get(), &EditorView::rendererChanged,
                     [&] { seen.push_back(h.view->rendererState().active); });

    h.view->show();
    ASSERT_TRUE(pumpUntil([&] { return h.view->rendererState().fallbacks > 0; }));
    EXPECT_EQ(seen, (std::vector<EditorBackend>{EditorBackend::Rhi, EditorBackend::Raster}));
    EXPECT_EQ(h.view->rendererState().active, EditorBackend::Raster);
    EXPECT_EQ(h.view->renderer().name(), "raster");
    EXPECT_EQ(h.view->rendererState().reason.rfind("fallback: forced QRhi creation failure", 0), 0U)
        << h.view->rendererState().reason;
    EXPECT_EQ(qobject_cast<RhiEditorViewport*>(h.view->viewport()), nullptr);
    EXPECT_EQ(h.text(), text);
    EXPECT_EQ(h.view->editor().state(), before);

    // It stays on raster, and the new viewport paints and takes input.
    pump();
    EXPECT_EQ(h.view->rendererState().fallbacks, 1U);
    const std::uint64_t paints = h.view->paintCount();
    h.type("x");
    pump();
    EXPECT_EQ(h.text(), "first line\nx line\nthird line\n");
    EXPECT_GT(h.view->paintCount(), paints);
}

TEST(EditorRenderer, AutoTurnsDownASoftwareAdapterAtRunTime) {
    FROST_REQUIRE_OFFSCREEN();
    HarnessOptions options;
    options.renderer.environment = windowsDesktop();
    // The adapter becomes known when the QRhi viewport initialises; the hook reports WARP then.
    options.renderer.softwareAdapter = "Microsoft Basic Render Driver";
    options.show = false;
    EditorHarness h("text\n", options);
    h.view->show();
    ASSERT_TRUE(pumpUntil([&] { return h.view->rendererState().fallbacks > 0; }));
    const std::string& reason = h.view->rendererState().reason;
    EXPECT_NE(reason.find("software adapter (WARP)"), std::string::npos) << reason;
    EXPECT_NE(reason.find("Microsoft Basic Render Driver"), std::string::npos) << reason;
    EXPECT_EQ(h.view->rendererState().active, EditorBackend::Raster);
}

TEST(EditorRenderer, TheSettingSwitchesTheBackendLive) {
    FROST_REQUIRE_OFFSCREEN();
    ManualClock clock;
    io::MemFs fs{clock, io::PathStyle::Posix};
    const std::string appFile = "/home/u/.config/frostide/settings.json";
    ASSERT_TRUE(fs.createDirectories("/home/u/.config/frostide"));
    auto store = settings::SettingsStore::open(
        settings::builtinRegistry(), fs, settings::SettingsPaths{.applicationFile = appFile, .project = {}},
        settings::StoreOptions{.projectTrusted = false, .migrations = nullptr, .style = io::PathStyle::Posix});
    ASSERT_TRUE(store);
    HarnessOptions options;
    options.renderer.environment = windowsDesktop();
    EditorHarness h("alpha\nbeta\n", options);
    EditorSettingsBinding binding(**store, fs, "", *h.view);
    selectRange(*h.view, 6, 10); // "beta"
    const text::EditorState before = h.view->editor().state();

    struct Seen {
        EditorBackend backend;
        bool rhiViewport;
        double fontPoints;
    };
    std::vector<Seen> seen;
    QObject::connect(h.view.get(), &EditorView::rendererChanged, [&] {
        auto* surface = qobject_cast<RhiEditorViewport*>(h.view->viewport());
        seen.push_back(Seen{h.view->rendererState().active, surface != nullptr,
                            surface != nullptr ? surface->painter().font().pointSizeF() : 0.0});
    });

    ASSERT_TRUE((*store)->set(settings::Scope::Application, settings::key::editorRenderer, std::string("raster")));
    ASSERT_TRUE(pumpUntil([&] { return h.view->settings().renderer == "raster"; }));
    EXPECT_EQ(h.view->rendererState().active, EditorBackend::Raster);
    EXPECT_EQ(h.view->rendererState().mode, EditorRendererMode::Raster);
    EXPECT_EQ(h.view->rendererState().reason, "editor.renderer is raster");
    EXPECT_EQ(h.view->renderer().name(), "raster");

    // rhi attaches the QRhi viewport at once (the view has painted), with the editor font. The
    // offscreen platform cannot composite it, so it may fall back afterwards; the model stays.
    seen.clear();
    ASSERT_TRUE((*store)->set(settings::Scope::Application, settings::key::editorFontSize, std::int64_t{18}));
    ASSERT_TRUE((*store)->set(settings::Scope::Application, settings::key::editorRenderer, std::string("rhi")));
    ASSERT_TRUE(pumpUntil([&] { return !seen.empty(); }));
    EXPECT_EQ(seen.front().backend, EditorBackend::Rhi);
    EXPECT_TRUE(seen.front().rhiViewport);
    EXPECT_DOUBLE_EQ(seen.front().fontPoints, 18.0);
    EXPECT_EQ(h.view->rendererState().mode, EditorRendererMode::Rhi);
    EXPECT_EQ(h.view->editor().state(), before);
    pump();

    ASSERT_TRUE((*store)->set(settings::Scope::Application, settings::key::editorRenderer, std::string("raster")));
    ASSERT_TRUE(pumpUntil([&] { return h.view->settings().renderer == "raster"; }));
    pump(); // A failure the QRhi viewport queued before it went must not count any more.
    EXPECT_EQ(h.view->rendererState().active, EditorBackend::Raster);
    EXPECT_EQ(h.view->rendererState().reason, "editor.renderer is raster");
    EXPECT_EQ(qobject_cast<RhiEditorViewport*>(h.view->viewport()), nullptr);
    EXPECT_EQ(h.view->renderer().name(), "raster");
    EXPECT_EQ(h.text(), "alpha\nbeta\n");
    EXPECT_EQ(h.view->editor().state(), before);
}

} // namespace
} // namespace frost::ui
