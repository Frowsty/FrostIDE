// SPDX-License-Identifier: Apache-2.0
//
// Screenshot goldens and rendered-contrast checks (offscreen, ADR 0015 decision 11). The
// SamplePanel is rendered in-process with QWidget::render into a QImage at an explicit device
// pixel ratio, compared with tests/ui/goldens/<os>/<variant>@<pct>.png, and sampled at
// structural points whose colours come from the theme (independent of text rendering).

#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/model/contrast.hpp"
#include "frost/ui/model/theme_registry.hpp"

#include "support/appearance_fixture.hpp"
#include "support/golden.hpp"
#include "support/sample_panel.hpp"
#include <QApplication>
#include <QFontInfo>
#include <QScreen>
#include <QStyleOption>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <ostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::ui {
namespace {

// The process must render at dpr 1 and 96 DPI, so the explicit dpr is the only scale.
void expectNeutralProcess() {
    ASSERT_EQ(qApp->devicePixelRatio(), 1.0) << "unset QT_SCALE_FACTOR/QT_FONT_DPI";
    ASSERT_NE(QGuiApplication::primaryScreen(), nullptr);
    ASSERT_EQ(QGuiApplication::primaryScreen()->logicalDotsPerInch(), 96.0) << "unset QT_SCALE_FACTOR/QT_FONT_DPI";
}

struct Variant {
    const char* name;    // the test name suffix, e.g. GlacierDark_100
    const char* file;    // the golden, e.g. GlacierDark@100.png
    const char* themeId; // the theme to install
    Density density;
    std::uint16_t zoom;
    std::uint16_t dprPercent;
};

constexpr Variant kVariants[] = {
    {"GlacierDark_100", "GlacierDark@100.png", "glacier-dark", Density::Normal, 100, 100},
    {"GlacierDark_125", "GlacierDark@125.png", "glacier-dark", Density::Normal, 100, 125},
    {"GlacierDark_150", "GlacierDark@150.png", "glacier-dark", Density::Normal, 100, 150},
    {"GlacierDark_175", "GlacierDark@175.png", "glacier-dark", Density::Normal, 100, 175},
    {"GlacierLight_100", "GlacierLight@100.png", "glacier-light", Density::Normal, 100, 100},
    {"GlacierLight_125", "GlacierLight@125.png", "glacier-light", Density::Normal, 100, 125},
    {"GlacierLight_150", "GlacierLight@150.png", "glacier-light", Density::Normal, 100, 150},
    {"GlacierLight_175", "GlacierLight@175.png", "glacier-light", Density::Normal, 100, 175},
    {"GlacierClassic_100", "GlacierClassic@100.png", "glacier-classic", Density::Normal, 100, 100},
    {"GlacierHighContrast_100", "GlacierHighContrast@100.png", "glacier-high-contrast", Density::Normal, 100, 100},
    {"GlacierDarkCompact_100", "GlacierDarkCompact@100.png", "glacier-dark", Density::Compact, 100, 100},
    {"GlacierDarkZoom125_100", "GlacierDarkZoom125@100.png", "glacier-dark", Density::Normal, 125, 100},
    {"GlacierDarkZoom125_150", "GlacierDarkZoom125@150.png", "glacier-dark", Density::Normal, 125, 150},
};

// gtest prints the variant by name rather than as raw bytes.
void PrintTo(const Variant& variant, std::ostream* os) { *os << variant.name; }

// An installed theme plus the rendered sample panel.
struct Rendered {
    std::unique_ptr<test::Installed> installed;
    test::SamplePanel panel;
    QImage image;
    qreal dpr = 1.0;

    [[nodiscard]] const FrostStyle& style() const { return *FrostStyle::current(); }
    [[nodiscard]] QColor theme(ColorKey key) const { return style().color(key); }
    // The device pixel containing logical point `p` of `widget`.
    [[nodiscard]] QPoint device(const QWidget* widget, QPointF p) const {
        const QPointF root = QPointF(widget->mapTo(panel.root.get(), QPoint(0, 0))) + p;
        return {static_cast<int>(std::floor(root.x() * dpr)), static_cast<int>(std::floor(root.y() * dpr))};
    }
    [[nodiscard]] QColor at(QPoint devicePoint) const { return image.pixelColor(devicePoint); }
};

std::unique_ptr<Rendered> render(const char* themeId, Density density, std::uint16_t zoom, qreal dpr) {
    auto result = std::make_unique<Rendered>();
    AppearanceSettings settings;
    settings.theme = themeId;
    settings.density = density;
    settings.zoomPercent = zoom;
    result->installed = test::install(settings);
    if (!result->installed) {
        return nullptr;
    }
    test::applyTestFontSettings();
    const auto mono = result->installed->manager->ensureEditorFonts();
    EXPECT_TRUE(mono) << mono.error().describe();
    result->panel = test::makeSamplePanel(mono ? mono->primaryFamily : QString());
    result->dpr = dpr;
    result->image = test::renderAt(*result->panel.root, dpr);
    return result;
}

// `top` composited over the opaque `bottom` (source-over).
QColor over(const QColor& top, const QColor& bottom) {
    const double a = top.alphaF();
    const auto mix = [a](int t, int b) { return static_cast<int>(std::lround(t * a + b * (1.0 - a))); };
    return QColor(mix(top.red(), bottom.red()), mix(top.green(), bottom.green()), mix(top.blue(), bottom.blue()));
}

// Opaque colours equal within one step per channel (the renderer's rounding).
::testing::AssertionResult sameColor(const QColor& actual, const QColor& expected) {
    const int diff = std::max({std::abs(actual.red() - expected.red()), std::abs(actual.green() - expected.green()),
                               std::abs(actual.blue() - expected.blue()), std::abs(actual.alpha() - expected.alpha())});
    if (diff <= 1) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << actual.name(QColor::HexArgb).toStdString() << " is not "
                                         << expected.name(QColor::HexArgb).toStdString();
}

QRect logicalGeometry(const test::SamplePanel& panel, const QWidget* widget) {
    return {widget->mapTo(panel.root.get(), QPoint(0, 0)), widget->size()};
}

// The structural samples (ADR 0015 decision 11). Interior points only: every sample is at
// least one device pixel away from an antialiased edge at every scale.
void expectStructure(const Rendered& r) {
    const test::SamplePanel& panel = r.panel;
    const QColor window = r.theme(ColorKey::MainWindowBackground);
    const QColor island = r.theme(ColorKey::IslandBackground);
    const int gap = r.style().metricPx(MetricKey::IslandGap);
    const int arc = r.style().metricPx(MetricKey::IslandArc);
    const int margin = r.style().metricPx(MetricKey::LayoutMargin);

    // The top band and, when there is one, the gap between the islands.
    EXPECT_TRUE(sameColor(r.at(r.device(panel.root.get(), {test::kPanelWidth / 2.0, test::kTopBand / 2.0})), window))
        << "top band";
    const QRect left = logicalGeometry(panel, panel.left);
    const QRect right = logicalGeometry(panel, panel.right);
    if (gap > 0) {
        const double x = ((left.x() + left.width()) * r.dpr + right.x() * r.dpr) / 2.0;
        const QPoint p(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(left.center().y() * r.dpr)));
        EXPECT_TRUE(sameColor(r.at(p), window)) << "gap between the islands";
    }
    // The island interior, inside the layout margin.
    EXPECT_TRUE(sameColor(r.at(r.device(panel.left, {margin / 2.0, left.height() / 2.0})), island))
        << "island interior";
    // Just inside the corner radius, outside the rounded island.
    if (arc > 2) {
        const QPoint corner(static_cast<int>(std::ceil(left.x() * r.dpr)),
                            static_cast<int>(std::ceil(left.y() * r.dpr)));
        EXPECT_TRUE(sameColor(r.at(corner), window)) << "island corner";
    }
    // The default button, left of its centred label.
    const QWidget* button = panel.defaultButton;
    EXPECT_TRUE(sameColor(r.at(r.device(button, {4.0, button->height() / 2.0})),
                          over(r.theme(ColorKey::ButtonDefaultBackground), island)))
        << "default button";
    // The focus ring's top edge, in the gallery's focused-button cell.
    const QRect cell = panel.gallery->cell(2);
    const double ring = std::max(1.0, std::round(r.style().metricPx(MetricKey::ComponentFocusWidth) * r.dpr));
    const QPointF galleryOrigin(panel.gallery->mapTo(panel.root.get(), QPoint(0, 0)));
    const QPoint ringPoint(static_cast<int>(std::floor((galleryOrigin.x() + cell.center().x()) * r.dpr)),
                           static_cast<int>(std::floor((galleryOrigin.y() + cell.y()) * r.dpr + ring / 2.0)));
    EXPECT_TRUE(sameColor(r.at(ringPoint), over(r.theme(ColorKey::ComponentFocusColor),
                                                over(r.theme(ColorKey::ButtonBackground), island))))
        << "focus ring";
    /*
     * The selected tree row, right of its text. Qt draws a view without keyboard focus with its
     * selection inactive (no State_Active), as IntelliJ-style trees do, and no widget of the
     * panel holds focus, so the sample is Tree.selectionInactiveBackground; a focused view would
     * draw Tree.selectionBackground (the fallback of List.selectionBackground).
     */
    const QTreeView* tree = panel.tree;
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        EXPECT_TRUE(tree->isActiveWindow()) << "makeSamplePanel activates the panel on the offscreen platform";
    }
    const QRect row = tree->visualRect(tree->model()->index(1, 0));
    const bool focused = tree->hasFocus() && tree->isActiveWindow();
    const ColorKey selection = focused ? ColorKey::TreeSelectionBackground : ColorKey::TreeSelectionInactiveBackground;
    const QPoint rowPoint = r.device(tree->viewport(), {tree->viewport()->width() - 6.0, row.center().y() + 0.0});
    EXPECT_TRUE(sameColor(r.at(rowPoint), over(r.theme(selection), r.theme(ColorKey::TextFieldBackground))))
        << "selected row (" << (focused ? "focused" : "unfocused") << ")";
    // The checked check box, inside its box above the check mark.
    {
        QStyleOptionButton option;
        option.initFrom(panel.checked);
        option.state |= QStyle::State_On;
        const QRect box = r.style().subElementRect(QStyle::SE_CheckBoxIndicator, &option, panel.checked);
        EXPECT_TRUE(
            sameColor(r.at(r.device(panel.checked, {box.x() + box.width() / 2.0, box.y() + box.height() / 4.0})),
                      over(r.theme(ColorKey::CheckBoxCheckedBackground), island)))
            << "checked check box";
    }
    // The checked radio's centre dot.
    {
        QStyleOptionButton option;
        option.initFrom(panel.checkedRadio);
        option.state |= QStyle::State_On;
        const QRect box = r.style().subElementRect(QStyle::SE_RadioButtonIndicator, &option, panel.checkedRadio);
        const QPointF centre(box.x() + box.width() / 2.0, box.y() + box.height() / 2.0);
        EXPECT_TRUE(
            sameColor(r.at(r.device(panel.checkedRadio, centre)),
                      over(r.theme(ColorKey::CheckBoxCheckmarkColor), r.theme(ColorKey::CheckBoxCheckedBackground))))
            << "radio dot";
    }
    // The disabled tool button's icon: its most-covered pixel lies on the line from the island
    // background to the Icon.Disabled tint (bg + a x (tint - bg)), with a well above zero. The
    // coverage of a 1 px stroke never reaches 255, so the tint is checked along that line.
    {
        const QColor tint = QColor::fromRgba(r.style().theme().iconColor(IconRole::Disabled).argb);
        const QRect area = logicalGeometry(panel, panel.disabledTool);
        const double dr = tint.redF() - island.redF();
        const double dg = tint.greenF() - island.greenF();
        const double db = tint.blueF() - island.blueF();
        const double length = dr * dr + dg * dg + db * db;
        ASSERT_GT(length, 0.0);
        double best = -1.0;
        QColor bestColor;
        for (int y = static_cast<int>(std::ceil(area.top() * r.dpr)); y < static_cast<int>((area.bottom() + 1) * r.dpr);
             ++y) {
            for (int x = static_cast<int>(std::ceil(area.left() * r.dpr));
                 x < static_cast<int>((area.right() + 1) * r.dpr); ++x) {
                const QColor c = r.image.pixelColor(x, y);
                const double a = ((c.redF() - island.redF()) * dr + (c.greenF() - island.greenF()) * dg +
                                  (c.blueF() - island.blueF()) * db) /
                                 length;
                if (a > best) {
                    best = a;
                    bestColor = c;
                }
            }
        }
        EXPECT_GE(best, 0.4) << "the disabled icon is drawn";
        const QColor expected(static_cast<int>(std::lround((island.redF() + best * dr) * 255.0)),
                              static_cast<int>(std::lround((island.greenF() + best * dg) * 255.0)),
                              static_cast<int>(std::lround((island.blueF() + best * db) * 255.0)));
        const int off =
            std::max({std::abs(bestColor.red() - expected.red()), std::abs(bestColor.green() - expected.green()),
                      std::abs(bestColor.blue() - expected.blue())});
        EXPECT_LE(off, 3) << "disabled icon " << bestColor.name().toStdString() << " is not on the line to "
                          << tint.name().toStdString();
    }
}

class Golden : public ::testing::TestWithParam<Variant> {};

TEST_P(Golden, Matches) {
    expectNeutralProcess();
    const Variant& v = GetParam();
    const auto r = render(v.themeId, v.density, v.zoom, v.dprPercent / 100.0);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->image.width(), static_cast<int>(std::lround(test::kPanelWidth * v.dprPercent / 100.0)));
    expectStructure(*r);
    test::expectMatchesGolden(r->image, QString::fromLatin1(v.file));
}

INSTANTIATE_TEST_SUITE_P(ThemeScreenshot, Golden, ::testing::ValuesIn(kVariants),
                         [](const ::testing::TestParamInfo<Variant>& info) { return std::string(info.param.name); });

TEST(ThemeScreenshot, FontIsBundled) {
    expectNeutralProcess();
#ifdef _WIN32
    BOOL smoothing = FALSE;
    UINT type = 0;
    SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &smoothing, 0);
    SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &type, 0);
    std::printf("[ui screenshot] font smoothing %d, type %u (the goldens use PreferNoHinting, NoSubpixelAntialias)\n",
                smoothing, type);
    RecordProperty("font_smoothing", std::to_string(smoothing) + "/" + std::to_string(type));
#endif
    const auto r = render("glacier-dark", Density::Normal, 100, 1.0);
    ASSERT_TRUE(r);
    ASSERT_FALSE(r->installed->report.uiFamilies.isEmpty());
    EXPECT_EQ(QFontInfo(r->panel.label->font()).family(), r->installed->report.uiFamilies.front());
    const auto mono = r->installed->manager->ensureEditorFonts();
    ASSERT_TRUE(mono);
    EXPECT_EQ(QFontInfo(r->panel.monoLabel->font()).family(), mono->primaryFamily);
    EXPECT_TRUE(QFontInfo(r->panel.monoLabel->font()).fixedPitch());
}

TEST(GoldenComparator, BoundsOutliersInCountAndSize) {
    QImage golden(40, 30, QImage::Format_ARGB32);
    golden.fill(QColor(30, 40, 50));
    const auto withDiffs = [&](int pixels, int delta) {
        QImage image = golden.copy();
        for (int i = 0; i < pixels; ++i) {
            image.setPixelColor(i % 40, i / 40, QColor(30 + delta, 40, 50));
        }
        return test::compareImages(image, golden);
    };
    EXPECT_TRUE(withDiffs(0, 0).passes());
    EXPECT_TRUE(withDiffs(1200, test::kChannelTolerance).passes()) << "every pixel within the tolerance";
    EXPECT_TRUE(withDiffs(test::kMaxOutlierPixels, test::kMaxOutlierChannelDiff).passes()) << "antialiasing noise";
    EXPECT_FALSE(withDiffs(test::kMaxOutlierPixels + 1, test::kChannelTolerance + 1).passes()) << "too many";
    const test::Comparison recoloured = withDiffs(1, test::kMaxOutlierChannelDiff + 1);
    EXPECT_FALSE(recoloured.passes()) << "one pixel far off is a regression, not noise";
    EXPECT_EQ(recoloured.maxChannelDiff, test::kMaxOutlierChannelDiff + 1);
    EXPECT_EQ(recoloured.pixelsOverTolerance, 1);
    EXPECT_FALSE(withDiffs(test::kMaxOutlierPixels, 200).passes()) << "a small shape drawn in the wrong colour";
    EXPECT_FALSE(test::compareImages(golden.copy(0, 0, 39, 30), golden).passes()) << "sizes differ";
}

constexpr const char* kThemes[] = {"glacier-dark", "glacier-light", "glacier-classic", "glacier-high-contrast"};

TEST(ThemeContrast, RenderedIslandSeparation) {
    expectNeutralProcess();
    for (const char* id : kThemes) {
        SCOPED_TRACE(id);
        const auto r = render(id, Density::Normal, 100, 1.0);
        ASSERT_TRUE(r);
        const QColor band = r->at(r->device(r->panel.root.get(), {test::kPanelWidth / 2.0, test::kTopBand / 2.0}));
        const int margin = r->style().metricPx(MetricKey::LayoutMargin);
        const QColor island = r->at(r->device(r->panel.left, {margin / 2.0, r->panel.left->height() / 2.0}));
        const double ratio = contrastRatio(Rgba{band.rgba()}, Rgba{island.rgba()});
        RecordProperty(std::string(id) + "_island_separation", std::to_string(ratio));
        EXPECT_GE(ratio, 1.20) << band.name().toStdString() << " vs " << island.name().toStdString();
    }
}

TEST(ThemeContrast, DeclaredPairs) {
    for (const char* id : kThemes) {
        SCOPED_TRACE(id);
        AppearanceSettings settings;
        settings.theme = id;
        const auto installed = test::install(settings);
        ASSERT_TRUE(installed);
        const Theme& theme = installed->manager->theme();
        for (const ContrastPair& pair : kContrastPairs) {
            if (const auto minimum = minimumContrast(pair.kind, theme.isHighContrast())) {
                EXPECT_GE(pairContrast(theme, pair), *minimum)
                    << info(pair.foreground).name << " on " << info(pair.background).name;
            }
        }
        for (const IconContrastPair& pair : kIconContrastPairs) {
            if (const auto minimum = minimumContrast(pair.kind, theme.isHighContrast())) {
                EXPECT_GE(pairContrast(theme, pair), *minimum)
                    << info(pair.role).name << " on " << info(pair.background).name;
            }
        }
    }
}

} // namespace
} // namespace frost::ui
