// SPDX-License-Identifier: Apache-2.0
//
// HiDPI cross-checks on the offscreen platform (ADR 0015 decision 11). Registered twice: in a
// dpr-1 process, and as *.qtscale150 with QT_SCALE_FACTOR=1.5 and FROST_TEST_QT_SCALE=150. The
// SamplePanel is shown on the offscreen platform (never a desktop window), its backing store is
// captured, and the capture is compared with the goldens rendered in a dpr-1 process at an
// explicit dpr, so the explicit-dpr path is checked against Qt's real scaling.

#include "frost/io/path.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_atlas_set.hpp"
#include "frost/ui/icon_ids.hpp"

#include "support/appearance_fixture.hpp"
#include "support/golden.hpp"
#include "support/sample_panel.hpp"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>

namespace frost::ui {
namespace {

// FROST_TEST_QT_SCALE (percent), or 100 in the default registration.
int expectedScalePercent() {
    bool ok = false;
    const int value = qEnvironmentVariableIntValue("FROST_TEST_QT_SCALE", &ok);
    return ok ? value : 100;
}

// Showing a widget is safe only on the offscreen platform: no window may reach the desktop.
void requireOffscreenAndScale() {
    ASSERT_EQ(QGuiApplication::platformName(), QStringLiteral("offscreen"))
        << "the HiDPI tests show widgets and run only on the offscreen platform";
    ASSERT_EQ(qApp->devicePixelRatio(), expectedScalePercent() / 100.0)
        << "QT_SCALE_FACTOR does not match FROST_TEST_QT_SCALE";
}

// Shows `widget` (offscreen) and processes events until its window is exposed (5 s at most).
bool showAndExpose(QWidget& widget) {
    widget.show();
    QElapsedTimer timer;
    timer.start();
    while (widget.windowHandle() == nullptr || !widget.windowHandle()->isExposed()) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        if (timer.elapsed() > 5000) {
            return false;
        }
    }
    QApplication::processEvents();
    return true;
}

// How a capture was obtained; recorded with each comparison so the report can say what the
// oracle was.
enum class CaptureKind {
    BackingStore,        // QScreen::grabWindow returned the whole window
    BackingStoreCropped, // grabWindow returned only the top-left window-size device pixels
    Consistency,         // grabWindow failed; QWidget::grab (the goldens' own render path)
};

const char* describe(CaptureKind kind) {
    switch (kind) {
    case CaptureKind::BackingStore: return "backing store (grabWindow)";
    case CaptureKind::BackingStoreCropped: return "backing store (grabWindow, top-left crop)";
    case CaptureKind::Consistency: return "consistency (same render path)";
    }
    return "?";
}

/*
 * The backing store of `widget`'s window through QScreen::grabWindow. Under QT_SCALE_FACTOR the
 * offscreen plugin clamps the grab to the window's logical size counted in device pixels, so at
 * 150% the capture is the top-left two thirds of the window at full device resolution; `kind`
 * says so. When grabWindow returns nothing it falls back to QWidget::grab().
 */
QImage capture(QWidget& widget, CaptureKind& kind) {
    QScreen* screen = widget.windowHandle() != nullptr ? widget.windowHandle()->screen() : nullptr;
    QPixmap pixmap = screen != nullptr ? screen->grabWindow(widget.winId()) : QPixmap();
    const QSize device = widget.size() * widget.devicePixelRatio();
    if (pixmap.isNull()) {
        kind = CaptureKind::Consistency;
        pixmap = widget.grab();
    } else {
        kind = pixmap.size() == device ? CaptureKind::BackingStore : CaptureKind::BackingStoreCropped;
    }
    return pixmap.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

TEST(RealDprCrossCheck, BackingStoreMatchesGolden) {
    requireOffscreenAndScale();
    const int scale = expectedScalePercent();
    for (const char* variant : {"GlacierDark", "GlacierLight"}) {
        SCOPED_TRACE(variant);
        AppearanceSettings settings;
        settings.theme = std::string(variant) == "GlacierDark" ? "glacier-dark" : "glacier-light";
        const auto installed = test::install(settings);
        ASSERT_TRUE(installed);
        test::applyTestFontSettings();
        const auto mono = installed->manager->ensureEditorFonts();
        ASSERT_TRUE(mono);
        test::SamplePanel panel = test::makeSamplePanel(mono->primaryFamily);
        // makeSamplePanel shows the panel for real on the offscreen platform.
        ASSERT_TRUE(showAndExpose(*panel.root));
        CaptureKind kind = CaptureKind::BackingStore;
        const QImage captured = capture(*panel.root, kind);
        RecordProperty(std::string(variant) + "_oracle", describe(kind));
        std::printf("[ui hidpi] %s@%d: %s, %dx%d\n", variant, scale, describe(kind), captured.width(),
                    captured.height());
        const QString file = QStringLiteral("%1@%2.png").arg(QString::fromLatin1(variant)).arg(scale);
        QImage golden = test::loadGolden(file);
        ASSERT_FALSE(golden.isNull()) << "missing golden " << file.toStdString();
        if (kind == CaptureKind::BackingStoreCropped) {
            // Compare the region the capture covers; it must still be most of the left island.
            ASSERT_LE(captured.width(), golden.width());
            ASSERT_LE(captured.height(), golden.height());
            ASSERT_GE(captured.width() * 2, golden.width());
            ASSERT_GE(captured.height() * 2, golden.height());
            golden = golden.copy(QRect(QPoint(0, 0), captured.size()));
        }
        const test::Comparison comparison = test::compareImages(captured, golden);
        std::printf("[ui hidpi] %s@%d against its golden: %s\n", variant, scale, comparison.describe().c_str());
        EXPECT_TRUE(comparison.passes()) << file.toStdString() << ": " << comparison.describe() << " (captured "
                                         << captured.width() << "x" << captured.height() << ")";
        if (!comparison.passes()) {
            captured.save(
                QDir::current().filePath(QStringLiteral("ui-hidpi-%1@%2.actual.png").arg(variant).arg(scale)));
        }
    }
}

TEST(HiDpi, IconsCrispAt150) {
    requireOffscreenAndScale();
    const int scale = expectedScalePercent();
    const auto installed = test::install({});
    ASSERT_TRUE(installed);
    const FrostStyle* style = FrostStyle::current();
    IslandHost host(QBoxLayout::LeftToRight);
    auto* island = new IslandFrame;
    host.addIsland(island);
    auto* layout = new QVBoxLayout(island);
    auto* button = new QToolButton;
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setIcon(style->icon(icons::kActionsSearch));
    button->setIconSize(QSize(16, 16));
    layout->addWidget(button);
    host.resize(120, 80);
    ASSERT_TRUE(showAndExpose(host));
    CaptureKind kind = CaptureKind::BackingStore;
    const QImage captured = capture(host, kind);
    RecordProperty("oracle", describe(kind));
    std::printf("[ui hidpi] icon at %d%%: %s\n", scale, describe(kind));

    // The expected cell: the 16 px design at this scale, tinted Icon.Default over the island.
    const std::string path =
        *io::joinPath(*io::joinPath(test::resourceDir(), "icons"), "icons-" + std::to_string(scale) + ".fia");
    const auto bytes = io::realFileSystem().readFile(path);
    ASSERT_TRUE(bytes) << bytes.error().describe();
    const auto atlas = IconAtlasView::parse(std::as_bytes(std::span(*bytes)));
    ASSERT_TRUE(atlas) << atlas.error().describe();
    const auto index = atlas->find(icons::kActionsSearch, 16);
    ASSERT_TRUE(index);
    const AtlasEntryView entry = atlas->entry(*index);
    const AtlasPageView page = atlas->page(entry.page);
    ASSERT_EQ(entry.w, static_cast<int>(std::lround(16 * scale / 100.0)));
    const QColor tint = QColor::fromRgba(style->theme().iconColor(IconRole::Default).argb);
    const QColor bg = style->color(ColorKey::IslandBackground);
    const auto expectedAt = [&](int x, int y) {
        const std::size_t offset =
            static_cast<std::size_t>(entry.y + y) * page.stride + static_cast<std::size_t>(entry.x + x);
        const int m = std::to_integer<int>(page.pixels[offset]);
        const double a = static_cast<double>(tint.alphaF()) * m / 255.0;
        const auto mix = [a](int t, int b) { return static_cast<int>(std::lround(t * a + b * (1.0 - a))); };
        return QColor(mix(tint.red(), bg.red()), mix(tint.green(), bg.green()), mix(tint.blue(), bg.blue()));
    };
    // Search the button's region for the offset where the cell matches best.
    // The button sits at the top left, inside even a cropped capture.
    const QRect area = QRect(button->mapTo(&host, QPoint(0, 0)) * captured.devicePixelRatio(),
                             button->size() * captured.devicePixelRatio())
                           .intersected(captured.rect());
    ASSERT_GE(area.width(), entry.w);
    ASSERT_GE(area.height(), entry.h);
    int bestDiff = 256;
    QPoint bestAt;
    for (int oy = area.top(); oy + entry.h <= area.bottom() + 1; ++oy) {
        for (int ox = area.left(); ox + entry.w <= area.right() + 1; ++ox) {
            int worst = 0;
            for (int y = 0; y < entry.h && worst < bestDiff; ++y) {
                for (int x = 0; x < entry.w && worst < bestDiff; ++x) {
                    const QColor actual = captured.pixelColor(ox + x, oy + y);
                    const QColor expected = expectedAt(x, y);
                    worst = std::max({worst, std::abs(actual.red() - expected.red()),
                                      std::abs(actual.green() - expected.green()),
                                      std::abs(actual.blue() - expected.blue())});
                }
            }
            if (worst < bestDiff) {
                bestDiff = worst;
                bestAt = QPoint(ox, oy);
            }
        }
    }
    RecordProperty("icon_max_channel_diff", bestDiff);
    EXPECT_LE(bestDiff, 1) << "best match at " << bestAt.x() << "," << bestAt.y();
}

} // namespace
} // namespace frost::ui
