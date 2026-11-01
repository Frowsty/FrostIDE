// SPDX-License-Identifier: Apache-2.0
//
// Tests of the theme key registry, Rgba parsing and compositing, and the WCAG contrast
// arithmetic (frost/ui/model/{theme_registry,rgba,contrast}.hpp).

#include "frost/ui/model/contrast.hpp"
#include "frost/ui/model/rgba.hpp"
#include "frost/ui/model/theme.hpp"
#include "frost/ui/model/theme_registry.hpp"

#include "model_test_support.hpp"
#include <gtest/gtest.h>

#include <cmath>
#include <set>
#include <string>

namespace frost::ui {
namespace {

// The registry hash of the current tables [consistency]. A deliberate registry change (a new
// key, a renamed key, a changed range or opacity) updates this constant, and with it every
// compiled theme blob (rebuilt by the build).
constexpr std::uint64_t kExpectedRegistryHash = 0x3a4b4dee34a16308ULL;

TEST(Registry, TablesConsistent) {
    std::set<std::string_view> names;
    for (std::size_t i = 0; i < kColorKeys.size(); ++i) {
        EXPECT_EQ(static_cast<std::size_t>(kColorKeys[i].key), i);
        EXPECT_TRUE(names.insert(kColorKeys[i].name).second) << kColorKeys[i].name;
        // Every chain ends within kColorKeyCount steps.
        ColorKey key = kColorKeys[i].fallback;
        std::size_t steps = 0;
        while (key != ColorKey::kCount) {
            ASSERT_LT(++steps, kColorKeyCount) << kColorKeys[i].name;
            key = info(key).fallback;
        }
    }
    for (std::size_t i = 0; i < kMetricKeys.size(); ++i) {
        EXPECT_EQ(static_cast<std::size_t>(kMetricKeys[i].key), i);
        EXPECT_TRUE(names.insert(kMetricKeys[i].name).second) << kMetricKeys[i].name;
        EXPECT_LE(kMetricKeys[i].min, kMetricKeys[i].max);
    }
    for (std::size_t i = 0; i < kIconRoles.size(); ++i) {
        EXPECT_EQ(static_cast<std::size_t>(kIconRoles[i].role), i);
        EXPECT_TRUE(names.insert(kIconRoles[i].name).second) << kIconRoles[i].name;
    }
    for (const ContrastPair& pair : kContrastPairs) {
        ASSERT_LT(static_cast<std::size_t>(pair.background), kColorKeyCount);
        if (!info(pair.background).opaque) {
            ASSERT_NE(pair.underlay, ColorKey::kCount) << info(pair.background).name;
            EXPECT_TRUE(info(pair.underlay).opaque);
        }
    }
    for (const IconContrastPair& pair : kIconContrastPairs) {
        if (!info(pair.background).opaque) {
            ASSERT_NE(pair.underlay, ColorKey::kCount) << info(pair.background).name;
            EXPECT_TRUE(info(pair.underlay).opaque);
        }
        EXPECT_NE(pair.role, IconRole::Disabled) << "Disabled icons are exempt";
    }
    EXPECT_EQ(kRegistryHash, kExpectedRegistryHash)
        << std::hex << "kRegistryHash is 0x" << kRegistryHash << "; update kExpectedRegistryHash after a deliberate "
        << "registry change";
}

TEST(Registry, SpecifiedDefaultsAndFallbacks) {
    EXPECT_TRUE(info(ColorKey::MainWindowBackground).opaque);
    EXPECT_TRUE(info(ColorKey::IslandBackground).opaque);
    EXPECT_EQ(info(ColorKey::EditorBackground).fallback, ColorKey::IslandBackground);
    EXPECT_EQ(info(ColorKey::ToolWindowBackground).fallback, ColorKey::IslandBackground);
    EXPECT_EQ(info(ColorKey::PanelBackground).fallback, ColorKey::IslandBackground);
    EXPECT_EQ(info(ColorKey::ComboBoxBackground).fallback, ColorKey::TextFieldBackground);
    EXPECT_EQ(info(ColorKey::TreeSelectionBackground).fallback, ColorKey::ListSelectionBackground);
    EXPECT_EQ(info(ColorKey::MenuBackground).fallback, ColorKey::PopupBackground);
    EXPECT_EQ(info(MetricKey::WindowInactiveAlpha).unit, MetricUnit::Ratio);
    EXPECT_TRUE(info(MetricKey::IslandArc).hasCompact);
    EXPECT_FALSE(info(MetricKey::FontUiSize).hasCompact) << "compact mode never changes fonts";
    EXPECT_FALSE(info(MetricKey::FontSmallSize).hasCompact);
    EXPECT_EQ(info(IconRole::Default).name, "Icon.Default");
}

TEST(Registry, LookupByName) {
    for (const ColorKeyInfo& key : kColorKeys) {
        EXPECT_EQ(colorKeyByName(key.name), key.key) << key.name;
    }
    for (const MetricKeyInfo& key : kMetricKeys) {
        EXPECT_EQ(metricKeyByName(key.name), key.key) << key.name;
    }
    for (const IconRoleInfo& role : kIconRoles) {
        EXPECT_EQ(iconRoleByName(role.name), role.role) << role.name;
    }
    EXPECT_EQ(colorKeyByName("Button.default.background"), ColorKey::ButtonDefaultBackground);
    EXPECT_FALSE(colorKeyByName("button.default.background")) << "lookups are case-sensitive";
    EXPECT_FALSE(colorKeyByName(""));
    EXPECT_FALSE(colorKeyByName("Island.arc")) << "a metric is not a colour";
    EXPECT_FALSE(metricKeyByName("Island.background"));
    EXPECT_FALSE(iconRoleByName("Icon.default"));
    static_assert(colorKeyByName("MainWindow.background") == ColorKey::MainWindowBackground);
}

TEST(Rgba, ParseFormat) {
    EXPECT_EQ(parseHexColor("#1a2B3c"), Rgba{0xff1a2b3cU});
    EXPECT_EQ(parseHexColor("#1A2B3C80"), Rgba{0x801a2b3cU}) << "alpha comes last";
    EXPECT_EQ(parseHexColor("#00000000"), Rgba{0x00000000U});
    for (const std::string_view bad : {"", "#", "#123", "#1234", "#12345", "#1234567", "#123456789", "123456",
                                       "#12345G", "#GGGGGG", " #123456", "#123456 ", "##12345", "#-12345"}) {
        EXPECT_FALSE(parseHexColor(bad)) << bad;
    }
    EXPECT_EQ(Rgba{0xff0a0b0cU}.toHex(), "#0A0B0C");
    EXPECT_EQ(Rgba{0x7f0a0b0cU}.toHex(), "#0A0B0C7F");
    static_assert(parseHexColor("#FFFFFF") == Rgba{0xffffffffU});
}

TEST(Rgba, CompositeOver) {
    const Rgba white{0xffffffffU};
    const Rgba black{0xff000000U};
    EXPECT_EQ(compositeOver(Rgba{0x00ff0000U}, white), white) << "fully transparent";
    EXPECT_EQ(compositeOver(Rgba{0xffff0000U}, white), Rgba{0xffff0000U}) << "fully opaque";
    // 50% white over black: (255 * 128 + 0 * 127 + 127) / 255 = 128.
    EXPECT_EQ(compositeOver(Rgba{0x80ffffffU}, black), Rgba{0xff808080U});
    test::SplitMix64 rng(7);
    for (int i = 0; i < 10'000; ++i) {
        const Rgba top{static_cast<std::uint32_t>(rng.next())};
        const Rgba bottom{static_cast<std::uint32_t>(rng.next()) | 0xff000000U};
        const Rgba out = compositeOver(top, bottom);
        ASSERT_TRUE(out.opaque());
        // Each channel lies between the two inputs' channels.
        for (const auto channel : {&Rgba::r, &Rgba::g, &Rgba::b}) {
            const int lo = std::min((top.*channel)(), (bottom.*channel)());
            const int hi = std::max((top.*channel)(), (bottom.*channel)());
            ASSERT_GE((out.*channel)(), lo);
            ASSERT_LE((out.*channel)(), hi);
        }
    }
}

TEST(Contrast, KnownAnswers) {
    // Published values: WCAG's black/white maximum, and WebAIM's contrast checker for the greys.
    const auto ratio = [](std::string_view a, std::string_view b) {
        return contrastRatio(*parseHexColor(a), *parseHexColor(b));
    };
    EXPECT_NEAR(ratio("#000000", "#FFFFFF"), 21.0, 1e-9);
    EXPECT_NEAR(ratio("#767676", "#FFFFFF"), 4.54, 0.005);
    EXPECT_NEAR(ratio("#777777", "#FFFFFF"), 4.48, 0.005);
    EXPECT_NEAR(ratio("#595959", "#FFFFFF"), 7.00, 0.005);
    EXPECT_DOUBLE_EQ(ratio("#3870D8", "#3870D8"), 1.0);
    EXPECT_NEAR(relativeLuminance(*parseHexColor("#FFFFFF")), 1.0, 1e-12);
    EXPECT_NEAR(relativeLuminance(*parseHexColor("#000000")), 0.0, 1e-12);
    // The linear segment of the sRGB curve: channel 10/255 = 0.0392 <= 0.04045.
    EXPECT_NEAR(relativeLuminance(*parseHexColor("#0A0A0A")), (10.0 / 255.0) / 12.92, 1e-12);
}

TEST(Contrast, RatioProperties) {
    test::SplitMix64 rng(11);
    for (int i = 0; i < 10'000; ++i) {
        const Rgba a{static_cast<std::uint32_t>(rng.next()) | 0xff000000U};
        const Rgba b{static_cast<std::uint32_t>(rng.next()) | 0xff000000U};
        const double ab = contrastRatio(a, b);
        ASSERT_DOUBLE_EQ(ab, contrastRatio(b, a));
        ASSERT_GE(ab, 1.0);
        ASSERT_LE(ab, 21.0 + 1e-9);
        // Compositing stays in range, and its end points are the plain colours.
        const auto alpha = static_cast<std::uint32_t>(rng.below(256));
        const Rgba translucent{(a.argb & 0x00ffffffU) | (alpha << 24)};
        const double composited = contrastRatio(compositeOver(translucent, b), b);
        ASSERT_GE(composited, 1.0);
        ASSERT_LE(composited, 21.0 + 1e-9);
        ASSERT_DOUBLE_EQ(contrastRatio(compositeOver(Rgba{a.argb | 0xff000000U}, b), b), ab);
        ASSERT_DOUBLE_EQ(contrastRatio(compositeOver(Rgba{a.argb & 0x00ffffffU}, b), b), 1.0);
    }
}

TEST(Contrast, MinimumContrastTable) {
    struct Row {
        ContrastKind kind;
        std::optional<double> normal, highContrast;
    };
    const Row rows[] = {
        {ContrastKind::IslandSeparation, 1.20, 1.20},
        {ContrastKind::Text, 4.5, 7.0},
        {ContrastKind::SecondaryText, 4.5, 7.0},
        {ContrastKind::DisabledText, std::nullopt, 4.5},
        {ContrastKind::NonText, 3.0, 3.0},
        {ContrastKind::Border, std::nullopt, 3.0},
    };
    for (const Row& row : rows) {
        EXPECT_EQ(minimumContrast(row.kind, false), row.normal) << static_cast<int>(row.kind);
        EXPECT_EQ(minimumContrast(row.kind, true), row.highContrast) << static_cast<int>(row.kind);
    }
}

TEST(Contrast, PairContrastCompositesOverUnderlay) {
    ThemeData data = test::sampleThemeData();
    data.colors[static_cast<std::size_t>(ColorKey::ListBackground)] = Rgba{0xff000000U};
    // 50% white selection over black is #808080; white text on it.
    data.colors[static_cast<std::size_t>(ColorKey::ListSelectionBackground)] = Rgba{0x80ffffffU};
    data.colors[static_cast<std::size_t>(ColorKey::ListSelectionForeground)] = Rgba{0xffffffffU};
    const Theme theme = *Theme::fromData(data);
    const ContrastPair pair{ColorKey::ListSelectionForeground, ColorKey::ListSelectionBackground, ContrastKind::Text,
                            ColorKey::ListBackground};
    EXPECT_NEAR(pairContrast(theme, pair), contrastRatio(Rgba{0xffffffffU}, Rgba{0xff808080U}), 1e-12);
    // A translucent foreground is composited over the (composited) background.
    data.colors[static_cast<std::size_t>(ColorKey::ListSelectionForeground)] = Rgba{0x00ffffffU};
    EXPECT_NEAR(pairContrast(*Theme::fromData(data), pair), 1.0, 1e-12);
    // Icon pairs use the role colour.
    data.iconColors[static_cast<std::size_t>(IconRole::Selected)] = Rgba{0xff000000U};
    const IconContrastPair iconPair{IconRole::Selected, ColorKey::ListSelectionBackground, ContrastKind::NonText,
                                    ColorKey::ListBackground};
    EXPECT_NEAR(pairContrast(*Theme::fromData(data), iconPair), contrastRatio(Rgba{0xff000000U}, Rgba{0xff808080U}),
                1e-12);
}

} // namespace
} // namespace frost::ui
