// SPDX-License-Identifier: Apache-2.0
//
// The QRhi-versus-raster comparison harness of the editor tests (ADR 0043): renders a display list
// with RhiGlyphPainter into an offscreen texture (Direct3D 11 on WARP, or QRhi's Null backend where
// Direct3D is missing) and with the raster backend, and compares the two images per row. Shared by
// editor_rhi_test.cpp and editor_rhi_parity_test.cpp. FROST_TEST_RHI_ADAPTER=hardware renders on
// the default (hardware) adapter instead of WARP, for the manual real-GPU check on the hidden
// desktop (ADR 0043 "Parity"); ctest never sets it.
#pragma once

#include "frost/ui/editor/display_list.hpp"
#include "frost/ui/editor/editor_renderer.hpp"
#include "frost/ui/editor/rhi_glyph_painter.hpp"

#include <QHash>
#include <QImage>
#include <QPoint>
#include <QSize>
#include <QString>
#include <gtest/gtest.h>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace frost::ui::test {

// Per-row ink of an image: pixels that differ from the row's most common colour by more than 48 in
// a channel, and the leftmost and rightmost of them.
struct RowInk {
    int count = 0;
    int left = 0;
    int right = 0;
};

inline std::vector<RowInk> inkRows(const QImage& source) {
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    std::vector<RowInk> rows(static_cast<std::size_t>(image.height()));
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        RowInk& row = rows[static_cast<std::size_t>(y)];
        if (std::all_of(line, line + image.width(), [&](QRgb p) { return p == line[0]; })) {
            row = RowInk{0, image.width(), -1}; // One colour: its own mode, so no ink (a fast path).
            continue;
        }
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
inline bool inkNear(const std::vector<RowInk>& rows, std::size_t y) {
    for (std::size_t r = y == 0 ? 0 : y - 1; r <= y + 1 && r < rows.size(); ++r) {
        if (rows[r].count > 0) {
            return true;
        }
    }
    return false;
}

// Whether FROST_TEST_RHI_ADAPTER asks for the hardware adapter (the manual real-GPU check).
inline bool hardwareAdapterRequested() {
    return qEnvironmentVariable("FROST_TEST_RHI_ADAPTER") == QStringLiteral("hardware");
}

// A QRhi for offscreen rendering: Direct3D 11 on the WARP software adapter where it exists, so
// the test needs no GPU and gives the same pixels on every machine (the default adapter with
// FROST_TEST_RHI_ADAPTER=hardware), else the Null backend.
inline std::unique_ptr<QRhi> createTestRhi(bool& draws) {
    draws = false;
#ifdef _WIN32
    QRhiD3D11InitParams d3d;
    const QRhi::Flags flags = hardwareAdapterRequested() ? QRhi::Flags() : QRhi::Flags(QRhi::PreferSoftwareRenderer);
    if (QRhi* rhi = QRhi::create(QRhi::D3D11, &d3d, flags)) {
        draws = true;
        return std::unique_ptr<QRhi>(rhi);
    }
#endif
    QRhiNullInitParams null;
    return std::unique_ptr<QRhi>(QRhi::create(QRhi::Null, &null));
}

// What one offscreen QRhi frame gave: the image (empty on the Null backend), the painter's stats,
// the backend's name and the adapter's.
struct RhiFrame {
    QImage image;
    RhiFrameStats stats;
    std::string backend;
    std::string adapter;
};

// One QRhi shared by several renders (creating a Direct3D 11 device is most of a render's cost).
struct TestRhi {
    TestRhi() : rhi(createTestRhi(draws)) {}
    bool draws = false; // Before `rhi`: createTestRhi sets it.
    std::unique_ptr<QRhi> rhi;
};

// Renders `list` at `dpr` with `shared`'s QRhi into a fresh offscreen texture of the list's size
// times `dpr` and reads it back. Fails the test (and returns an empty frame) when the QRhi
// objects cannot be created.
inline RhiFrame renderWithRhi(TestRhi& shared, const EditorDisplayList& list, double dpr) {
    RhiFrame frame;
    QRhi* const rhi = shared.rhi.get();
    if (rhi == nullptr) {
        ADD_FAILURE() << "neither Direct3D 11 (WARP) nor QRhi's Null backend could be created";
        return frame;
    }
    frame.backend = rhi->backendName();
    frame.adapter = rhi->driverInfo().deviceName.toStdString();
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
    if (shared.draws && readback.pixelSize == size) {
        frame.image = QImage(reinterpret_cast<const uchar*>(readback.data.constData()), size.width(), size.height(),
                             size.width() * 4, QImage::Format_RGBA8888_Premultiplied)
                          .convertToFormat(QImage::Format_ARGB32);
    }
    return frame;
}

// renderWithRhi on a QRhi of its own.
inline RhiFrame renderWithRhi(const EditorDisplayList& list, double dpr) {
    TestRhi fresh;
    return renderWithRhi(fresh, list, dpr);
}

// The raster backend's image of `list` at `dpr` (the golden oracle).
inline QImage renderWithRaster(const EditorDisplayList& list, double dpr) {
    QImage image((list.viewport.size() * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    RasterEditorRenderer renderer(list.font);
    renderer.render(list, image);
    image.setDevicePixelRatio(1.0);
    return image.convertToFormat(QImage::Format_ARGB32);
}

/*
 * Coarse agreement of two renderings of one list (not a golden): solid colours at a few points
 * match within 2 levels; every inked row of one image has ink within a row in the other; inked
 * spans start and end within 3 px; the total ink is within 30 %. `minimumInk` is the raster
 * image's least total ink, a check that the frame is not blank (200 for a whole frame; smaller
 * for a crop or a frame of one element kind, 0 for a kind that may draw no ink at all).
 */
inline void expectSameInk(const QImage& raster, const QImage& gpu, const std::vector<QPoint>& solidPoints,
                          int minimumInk = 200) {
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
    if (minimumInk > 0) {
        EXPECT_GT(wantTotal, minimumInk);
    }
    EXPECT_NEAR(gotTotal, wantTotal, wantTotal * 0.3);
}

} // namespace frost::ui::test
