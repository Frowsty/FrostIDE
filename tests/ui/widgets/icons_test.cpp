// SPDX-License-Identifier: Apache-2.0
//
// Icon runtime tests (offscreen): pixels against the raw baked masks at every baked scale, the
// unbaked-scale resample, design-size switching with compact mode, roles and overrides,
// re-tinting on a theme switch, unknown ids, the cache cap, mixed bakes and lazy-load fallback,
// and icon_ids.hpp against icons.json.

#include "frost/io/path.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_atlas_set.hpp"
#include "frost/ui/icon_ids.hpp"

#include "support/appearance_fixture.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <gtest/gtest.h>

#include <array>
#include <set>
#include <string>

namespace frost::ui {
namespace {

AppearanceSettings withTheme(std::string_view id) {
    AppearanceSettings settings;
    settings.theme = std::string(id);
    return settings;
}

// One baked atlas file read from the resource directory, with its parsed view.
struct RawAtlas {
    std::string bytes;
    std::optional<IconAtlasView> view;
};

std::unique_ptr<RawAtlas> readAtlas(std::uint16_t scale) {
    auto raw = std::make_unique<RawAtlas>();
    const std::string path =
        *io::joinPath(*io::joinPath(test::resourceDir(), "icons"), "icons-" + std::to_string(scale) + ".fia");
    auto bytes = io::realFileSystem().readFile(path);
    EXPECT_TRUE(bytes) << bytes.error().describe();
    raw->bytes = bytes.value_or("");
    auto view = IconAtlasView::parse(std::as_bytes(std::span(raw->bytes)));
    EXPECT_TRUE(view) << view.error().describe();
    if (view) {
        raw->view.emplace(*view);
    }
    return raw;
}

// The Alpha8 mask of (id, designSize) in `atlas`, as a QImage copy.
QImage mask(const IconAtlasView& atlas, std::string_view id, std::uint8_t designSize) {
    const auto index = atlas.find(id, designSize);
    EXPECT_TRUE(index) << id << "@" << int{designSize};
    if (!index) {
        return {};
    }
    const AtlasEntryView entry = atlas.entry(*index);
    const AtlasPageView page = atlas.page(entry.page);
    EXPECT_EQ(page.format, PixelFormat::Alpha8);
    const QImage pageImage(reinterpret_cast<const uchar*>(page.pixels.data()), page.width, page.height,
                           static_cast<qsizetype>(page.stride), QImage::Format_Alpha8);
    return pageImage.copy(entry.x, entry.y, entry.w, entry.h);
}

// Expects `image` (premultiplied) to equal `tint` x mask per channel, +-1 (spec: (t x m + 127) / 255).
void expectTinted(const QImage& image, const QImage& alpha, Rgba tint, QPoint origin = {}) {
    ASSERT_FALSE(alpha.isNull());
    const QImage actual = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int worst = 0;
    for (int y = 0; y < alpha.height(); ++y) {
        for (int x = 0; x < alpha.width(); ++x) {
            const int m = alpha.constScanLine(y)[x];
            const QRgb p = reinterpret_cast<const QRgb*>(actual.constScanLine(origin.y() + y))[origin.x() + x];
            const auto expect = [m](int channel) { return (channel * m + 127) / 255; };
            worst = std::max({worst, std::abs(qAlpha(p) - expect(tint.a())), std::abs(qRed(p) - expect(tint.r())),
                              std::abs(qGreen(p) - expect(tint.g())), std::abs(qBlue(p) - expect(tint.b()))});
        }
    }
    EXPECT_LE(worst, 1);
}

QImage transparentImage(int logical, qreal dpr) {
    QImage image(qRound(logical * dpr), qRound(logical * dpr), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    return image;
}

TEST(Icons, ExactAtBakedScales) {
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    const FrostStyle* style = FrostStyle::current();
    const QIcon icon = style->icon(icons::kActionsSearch);
    ASSERT_FALSE(icon.isNull());
    const Rgba tint = style->theme().iconColor(IconRole::Default);
    for (const std::uint16_t scale : kAtlasScales) {
        SCOPED_TRACE(scale);
        const qreal dpr = scale / 100.0;
        const auto raw = readAtlas(scale);
        ASSERT_TRUE(raw->view);
        const QImage alpha = mask(*raw->view, icons::kActionsSearch, 16);
        ASSERT_EQ(alpha.width(), qRound(16 * dpr));
        // Through QIcon::paint.
        QImage painted = transparentImage(16, dpr);
        {
            QPainter painter(&painted);
            icon.paint(&painter, QRect(0, 0, 16, 16));
        }
        expectTinted(painted, alpha, tint);
        // Through QIcon::pixmap(size, dpr).
        const QPixmap pixmap = icon.pixmap(QSize(16, 16), dpr, QIcon::Normal);
        EXPECT_EQ(pixmap.devicePixelRatio(), dpr);
        ASSERT_EQ(pixmap.width(), alpha.width());
        expectTinted(pixmap.toImage(), alpha, tint);
    }
}

TEST(Icons, DownsampleAtUnbakedScale) {
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    const FrostStyle* style = FrostStyle::current();
    const QPixmap pixmap = style->icon(icons::kActionsSearch).pixmap(QSize(16, 16), 1.1, QIcon::Normal);
    ASSERT_EQ(pixmap.width(), 18); // lround(16 x 1.1)
    // Consistency: the 125% cell (20 px, the smallest larger 16 px design) resampled, then tinted.
    const auto raw = readAtlas(125);
    const QImage resampled =
        mask(*raw->view, icons::kActionsSearch, 16).scaled(18, 18, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage expected(18, 18, QImage::Format_ARGB32_Premultiplied);
    expected.fill(QColor::fromRgba(style->theme().iconColor(IconRole::Default).argb));
    {
        QPainter painter(&expected);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.drawImage(0, 0, resampled);
    }
    EXPECT_EQ(pixmap.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied), expected);
}

TEST(Icons, CompactToggleSwitchesDesignSize) {
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    const QIcon icon = FrostStyle::current()->icon(icons::kRunRun, IconSlot::Toolbar);
    const Rgba tint = FrostStyle::current()->theme().iconColor(IconRole::Default);
    const auto raw = readAtlas(100);
    const int normal = FrostStyle::current()->metricPx(MetricKey::ToolbarIconSize);
    ASSERT_EQ(normal, 20);
    expectTinted(icon.pixmap(QSize(normal, normal), 1.0).toImage(), mask(*raw->view, icons::kRunRun, 20), tint);

    AppearanceSettings compact = installed->settings.load();
    compact.density = Density::Compact;
    ASSERT_TRUE(installed->manager->apply(compact));
    const int small = FrostStyle::current()->metricPx(MetricKey::ToolbarIconSize);
    ASSERT_EQ(small, 16);
    // The same QIcon, now drawn from the 16 px design.
    expectTinted(icon.pixmap(QSize(small, small), 1.0).toImage(), mask(*raw->view, icons::kRunRun, 16), tint);
    // And back, still the same QIcon.
    compact.density = Density::Normal;
    ASSERT_TRUE(installed->manager->apply(compact));
    expectTinted(icon.pixmap(QSize(normal, normal), 1.0).toImage(), mask(*raw->view, icons::kRunRun, 20), tint);
}

TEST(Icons, DisabledAndSelectedRoles) {
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    const FrostStyle* style = FrostStyle::current();
    const Theme& theme = style->theme();
    const auto raw = readAtlas(100);
    const QImage alpha = mask(*raw->view, icons::kActionsSearch, 16);
    const QIcon icon = style->icon(icons::kActionsSearch);
    expectTinted(icon.pixmap(QSize(16, 16), 1.0, QIcon::Disabled).toImage(), alpha,
                 theme.iconColor(IconRole::Disabled));
    expectTinted(icon.pixmap(QSize(16, 16), 1.0, QIcon::Selected).toImage(), alpha,
                 theme.iconColor(IconRole::Selected));
    expectTinted(icon.pixmap(QSize(16, 16), 1.0, QIcon::Active).toImage(), alpha, theme.iconColor(IconRole::Default));
    // The manifest's role: status.error is tinted Icon.Error, and Disabled still wins.
    const QImage error = mask(*raw->view, icons::kStatusError, 16);
    expectTinted(style->icon(icons::kStatusError).pixmap(QSize(16, 16), 1.0).toImage(), error,
                 theme.iconColor(IconRole::Error));
    expectTinted(style->icon(icons::kStatusError).pixmap(QSize(16, 16), 1.0, QIcon::Disabled).toImage(), error,
                 theme.iconColor(IconRole::Disabled));
}

TEST(Icons, RoleOverride) {
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    const FrostStyle* style = FrostStyle::current();
    const Theme& theme = style->theme();
    const auto raw = readAtlas(100);
    const QImage alpha = mask(*raw->view, icons::kActionsSearch, 16);
    // An override replaces the manifest role for Normal and Active, not Disabled or Selected.
    const QIcon accent = style->icon(icons::kActionsSearch, IconSlot::Small, IconRole::Accent);
    expectTinted(accent.pixmap(QSize(16, 16), 1.0).toImage(), alpha, theme.iconColor(IconRole::Accent));
    expectTinted(accent.pixmap(QSize(16, 16), 1.0, QIcon::Active).toImage(), alpha, theme.iconColor(IconRole::Accent));
    expectTinted(accent.pixmap(QSize(16, 16), 1.0, QIcon::Disabled).toImage(), alpha,
                 theme.iconColor(IconRole::Disabled));
    expectTinted(accent.pixmap(QSize(16, 16), 1.0, QIcon::Selected).toImage(), alpha,
                 theme.iconColor(IconRole::Selected));
    ASSERT_NE(theme.iconColor(IconRole::Accent), theme.iconColor(IconRole::Default));
}

TEST(Icons, ThemeSwitchRetints) {
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    const QIcon icon = FrostStyle::current()->icon(icons::kActionsSearch);
    const auto raw = readAtlas(100);
    const QImage alpha = mask(*raw->view, icons::kActionsSearch, 16);
    const Rgba dark = FrostStyle::current()->theme().iconColor(IconRole::Default);
    expectTinted(icon.pixmap(QSize(16, 16), 1.0).toImage(), alpha, dark);
    ASSERT_TRUE(installed->manager->apply(withTheme("glacier-light")));
    const Rgba light = FrostStyle::current()->theme().iconColor(IconRole::Default);
    ASSERT_NE(light, dark) << "the themes must differ for this test to mean anything";
    expectTinted(icon.pixmap(QSize(16, 16), 1.0).toImage(), alpha, light);
}

TEST(Icons, UnknownIdIsNullAndCounted) {
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    EXPECT_TRUE(FrostStyle::current()->icon("no.such-icon").isNull());
    const auto icons = installed->manager->icons();
    const std::size_t before = icons->missingLookups();
    EXPECT_TRUE(
        icons->pixmap("no.such-icon", IconSlot::Small, 16, 1.0, QIcon::Normal, nullptr, Density::Normal).isNull());
    EXPECT_EQ(icons->missingLookups(), before + 1);
    // Without a theme: opaque black at design size 16.
    const auto raw = readAtlas(100);
    expectTinted(icons->pixmap(icons::kActionsSearch, IconSlot::Small, 16, 1.0, QIcon::Normal, nullptr, Density::Normal)
                     .toImage(),
                 mask(*raw->view, icons::kActionsSearch, 16), Rgba{0xff000000U});
}

TEST(Icons, CacheCapRespected) {
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    const auto icons = installed->manager->icons();
    icons->clearCache();
    std::size_t requested = 0;
    for (int px = 16; px <= 400 && requested < 3 * IconAtlasSet::kPixmapCacheBytes; px += 4) {
        ASSERT_FALSE(icons
                         ->pixmap(icons::kActionsSearch, IconSlot::Small, px, 1.0, QIcon::Normal,
                                  &installed->manager->theme(), Density::Normal)
                         .isNull());
        requested += static_cast<std::size_t>(px) * static_cast<std::size_t>(px) * 4U;
        EXPECT_LE(icons->cacheBytes(), IconAtlasSet::kPixmapCacheBytes);
    }
    EXPECT_GT(requested, IconAtlasSet::kPixmapCacheBytes);
    EXPECT_GT(icons->cacheBytes(), 0U);
}

TEST(Icons, MismatchedSourceHashRejected) {
    auto mem = test::mirrorResources();
    ASSERT_TRUE(mem);
    // An atlas of another bake (the synthetic fixtures) at 125%.
    QFile other(QString::fromStdString(test::sourceDir()) + "/tests/fuzz/corpus/icon_atlas/fixtures-125.fia");
    ASSERT_TRUE(other.open(QIODevice::ReadOnly));
    const QByteArray bytes = other.readAll();
    ASSERT_TRUE(mem->fs.writeFileAtomic(mem->path("icons/icons-125.fia"),
                                        std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size()))));
    const std::array<std::uint16_t, 2> preload{100, 125};
    const auto set = IconAtlasSet::open(mem->fs, mem->path("icons"), preload);
    ASSERT_FALSE(set);
    EXPECT_EQ(set.error().code(), ErrorCode::Corrupt);
    EXPECT_NE(set.error().describe().find("different bakes"), std::string::npos) << set.error().describe();
    // Lazily: the mixed atlas is never used, and the counter records the failed open.
    const std::array<std::uint16_t, 1> only100{100};
    const auto lazy = IconAtlasSet::open(mem->fs, mem->path("icons"), only100);
    ASSERT_TRUE(lazy);
    EXPECT_FALSE((*lazy)
                     ->pixmap(icons::kActionsSearch, IconSlot::Small, 16, 1.25, QIcon::Normal, nullptr, Density::Normal)
                     .isNull());
    EXPECT_EQ((*lazy)->failedAtlasOpens(), 1U);
    EXPECT_FALSE((*lazy)->isLoaded(125));
}

TEST(Icons, FailedLazyLoadUsesNextBest) {
    auto mem = test::mirrorResources({"icons/icons-125.fia"});
    ASSERT_TRUE(mem);
    const std::array<std::uint16_t, 1> preload{100};
    const auto set = IconAtlasSet::open(mem->fs, mem->path("icons"), preload);
    ASSERT_TRUE(set) << set.error().describe();
    // 16 px at 125% wants the 16 px design at 125% (20 device px); the next best loaded
    // candidate is the 20 px design at 100%, also 20 device px.
    const QPixmap pixmap =
        (*set)->pixmap(icons::kActionsSearch, IconSlot::Small, 16, 1.25, QIcon::Normal, nullptr, Density::Normal);
    EXPECT_EQ((*set)->failedAtlasOpens(), 1U);
    const auto raw = readAtlas(100);
    expectTinted(pixmap.toImage(), mask(*raw->view, icons::kActionsSearch, 20), Rgba{0xff000000U});
    // The failure is remembered, not retried.
    static_cast<void>(
        (*set)->pixmap(icons::kActionsClose, IconSlot::Small, 16, 1.25, QIcon::Normal, nullptr, Density::Normal));
    EXPECT_EQ((*set)->failedAtlasOpens(), 1U);
}

TEST(Icons, PreloadScale) {
    EXPECT_EQ(preloadScaleFor(1.0, 100), 100);
    EXPECT_EQ(preloadScaleFor(1.25, 100), 125);
    EXPECT_EQ(preloadScaleFor(1.0, 110), 125);
    // 125% zoom on a 150% screen: 16 px is 20 logical, 30 device pixels, exactly the 20 px
    // design at 150% (selection rule 2), so 150 is preloaded, not the 200 above 187.5%.
    EXPECT_EQ(preloadScaleFor(1.5, 125), 150);
    EXPECT_EQ(preloadScaleFor(2.0, 200), 300);
    EXPECT_EQ(preloadScaleFor(1.0, 100, 20), 100) << "the 20 px Icon.size is baked at 100%";
    EXPECT_EQ(preloadScaleFor(0.0, 100), 100) << "an unknown ratio counts as 1";
}

TEST(Icons, PreloadedScaleServesTheFirstSmallIcon) {
    // Consistency with pixmap(): on every screen and zoom of interest, the preloaded atlas is
    // the one pixmap() reads for a Small icon, so start-up never loads a second atlas for it.
    const std::string dir = *io::joinPath(test::resourceDir(), "icons");
    for (const double dpr : {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0}) {
        for (const std::uint16_t zoom : kZoomSteps) {
            SCOPED_TRACE(testing::Message() << dpr << " x " << zoom << "%");
            const std::array<std::uint16_t, 1> preload{preloadScaleFor(dpr, zoom)};
            auto set = IconAtlasSet::open(io::realFileSystem(), dir, preload);
            ASSERT_TRUE(set) << set.error().describe();
            const int logical = UiScale{Density::Normal, zoom}.px(16.0);
            static_cast<void>((*set)->pixmap(icons::kActionsSearch, IconSlot::Small, logical, dpr, QIcon::Normal,
                                             nullptr, Density::Normal));
            for (const std::uint16_t scale : kAtlasScales) {
                EXPECT_EQ((*set)->isLoaded(scale), scale == preload[0]) << scale << "%";
            }
        }
    }
}

TEST(Icons, IdsMatchManifest) {
    QFile file(QString::fromStdString(test::sourceDir()) + "/resources/icons/icons.json");
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    std::set<std::string> manifest;
    for (const QJsonValue& icon : QJsonDocument::fromJson(file.readAll()).object().value("icons").toArray()) {
        manifest.insert(icon.toObject().value("id").toString().toStdString());
    }
    const std::set<std::string> constants(icons::kAllIconIds.begin(), icons::kAllIconIds.end());
    EXPECT_EQ(constants.size(), icons::kAllIconIds.size()) << "duplicate constants";
    EXPECT_EQ(constants, manifest);
    // And every constant is in the installed bake.
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    for (const std::string_view id : icons::kAllIconIds) {
        EXPECT_TRUE(installed->manager->icons()->contains(id)) << id;
    }
}

} // namespace
} // namespace frost::ui
