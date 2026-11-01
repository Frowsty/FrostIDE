// SPDX-License-Identifier: Apache-2.0
//
// Tests of UiScale and the zoom steps, the appearance settings seam (validation and the
// listener semantics of InMemoryAppearanceSettings), and Sync-with-OS resolution
// (frost/ui/model/{ui_scale,appearance_settings,listener,os_appearance}.hpp).

#include "frost/ui/model/appearance_settings.hpp"
#include "frost/ui/model/os_appearance.hpp"
#include "frost/ui/model/theme.hpp"
#include "frost/ui/model/ui_scale.hpp"

#include "model_test_support.hpp"
#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace frost::ui {
namespace {

TEST(UiScale, Table) {
    struct Row {
        double logical;
        std::uint16_t zoom;
        int px;
    };
    const Row rows[] = {
        {28, 100, 28}, {28, 125, 35}, {24, 125, 30}, {8, 100, 8},   {10, 125, 13}, // 12.5 rounds away from zero
        {2, 125, 3},   {1, 50, 1},    {1, 80, 1},    {1, 110, 1},   {3, 150, 5},   // 4.5 -> 5
        {0, 300, 0},   {-4, 100, 0},  {16, 175, 28}, {20, 200, 40}, {13, 90, 12},  // 11.7 -> 12
    };
    for (const Row& row : rows) {
        const UiScale scale{Density::Normal, row.zoom};
        EXPECT_EQ(scale.px(row.logical), row.px) << row.logical << " at " << row.zoom << "%";
    }
    EXPECT_DOUBLE_EQ((UiScale{Density::Compact, 125}).factor(), 1.25);
}

TEST(UiScale, Monotonic) {
    test::SplitMix64 rng(5);
    for (int i = 0; i < 10'000; ++i) {
        const double a = rng.uniform(0, 64);
        const double b = rng.uniform(0, 64);
        const auto z1 = static_cast<std::uint16_t>(kMinZoomPercent + rng.below(kMaxZoomPercent - kMinZoomPercent + 1));
        const auto z2 = static_cast<std::uint16_t>(kMinZoomPercent + rng.below(kMaxZoomPercent - kMinZoomPercent + 1));
        const UiScale s1{Density::Normal, z1};
        if (a <= b) {
            ASSERT_LE(s1.px(a), s1.px(b));
        }
        if (z1 <= z2) {
            ASSERT_LE(s1.px(a), (UiScale{Density::Normal, z2}).px(a));
        }
    }
}

TEST(UiScale, MetricAndFontPx) {
    ThemeData data = test::sampleThemeData();
    const auto bh = static_cast<std::size_t>(MetricKey::ButtonHeight);
    data.metrics[bh] = 28.0F;
    data.compactMetrics[bh] = 24.0F;
    const auto font = static_cast<std::size_t>(MetricKey::FontUiSize);
    data.metrics[font] = data.compactMetrics[font] = 13.0F;
    const Theme theme = *Theme::fromData(data);
    EXPECT_EQ((UiScale{Density::Normal, 125}).metricPx(theme, MetricKey::ButtonHeight), 35);
    EXPECT_EQ((UiScale{Density::Compact, 125}).metricPx(theme, MetricKey::ButtonHeight), 30);
    EXPECT_DOUBLE_EQ((UiScale{Density::Compact, 150}).fontPx(theme, MetricKey::FontUiSize), 19.5) << "unrounded";
}

TEST(UiScale, ZoomSteps) {
    // nextZoomStep: the smallest step above z, else z. previousZoomStep: the largest below z, else z.
    const std::map<std::uint16_t, std::pair<std::uint16_t, std::uint16_t>> expected = {
        {50, {80, 50}},    {60, {80, 60}},    {80, {90, 80}},    {100, {110, 90}},
        {200, {200, 175}}, {250, {250, 200}}, {300, {300, 200}}, {112, {125, 110}},
    };
    for (const auto& [zoom, steps] : expected) {
        EXPECT_EQ(nextZoomStep(zoom), steps.first) << zoom;
        EXPECT_EQ(previousZoomStep(zoom), steps.second) << zoom;
    }
    // Against the definitions, for every zoom in range.
    for (std::uint16_t z = kMinZoomPercent; z <= kMaxZoomPercent; ++z) {
        std::optional<std::uint16_t> next, previous;
        for (const std::uint16_t step : kZoomSteps) {
            if (step > z && !next) {
                next = step;
            }
            if (step < z) {
                previous = step;
            }
        }
        ASSERT_EQ(nextZoomStep(z), next.value_or(z));
        ASSERT_EQ(previousZoomStep(z), previous.value_or(z));
    }
}

TEST(AppearanceSettings, Validate) {
    EXPECT_TRUE(validate(AppearanceSettings{}));
    const auto rejects = [](auto mutate, std::string_view field) {
        AppearanceSettings settings;
        mutate(settings);
        const auto result = validate(settings);
        ASSERT_FALSE(result) << field;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
        EXPECT_NE(result.error().message().find(field), std::string::npos) << result.error().message();
    };
    rejects([](AppearanceSettings& s) { s.theme = "Glacier"; }, "ui.theme");
    rejects([](AppearanceSettings& s) { s.theme = ""; }, "ui.theme");
    rejects([](AppearanceSettings& s) { s.syncLightTheme = std::string(kSyncWithOs); }, "ui.theme.syncLight");
    rejects([](AppearanceSettings& s) { s.syncDarkTheme = "a b"; }, "ui.theme.syncDark");
    rejects([](AppearanceSettings& s) { s.syncHighContrastTheme = ""; }, "ui.theme.syncHighContrast");
    rejects([](AppearanceSettings& s) { s.zoomPercent = 49; }, "ui.zoom");
    rejects([](AppearanceSettings& s) { s.zoomPercent = 301; }, "ui.zoom");
    rejects([](AppearanceSettings& s) { s.density = static_cast<Density>(7); }, "ui.compactMode");
    AppearanceSettings sync;
    sync.theme = std::string(kSyncWithOs);
    sync.zoomPercent = 300;
    EXPECT_TRUE(validate(sync));
    sync.zoomPercent = 50;
    EXPECT_TRUE(validate(sync));
    EXPECT_EQ(AppearanceSettings{}.theme, kDefaultThemeId);
}

AppearanceSettings withZoom(std::uint16_t zoom) {
    AppearanceSettings settings;
    settings.zoomPercent = zoom;
    return settings;
}

TEST(AppearanceSettings, StoreNotifyUnsubscribe) {
    InMemoryAppearanceSettings store;
    std::vector<std::string> calls;
    ListenerToken a =
        store.subscribe([&](const AppearanceSettings& s) { calls.push_back("a" + std::to_string(s.zoomPercent)); });
    ListenerToken b =
        store.subscribe([&](const AppearanceSettings& s) { calls.push_back("b" + std::to_string(s.zoomPercent)); });
    ASSERT_TRUE(store.store(withZoom(125)));
    EXPECT_EQ(store.load().zoomPercent, 125);
    EXPECT_EQ(calls, (std::vector<std::string>{"a125", "b125"})) << "registration order";
    a.reset();
    EXPECT_FALSE(a.active());
    ASSERT_TRUE(store.store(withZoom(150)));
    EXPECT_EQ(calls, (std::vector<std::string>{"a125", "b125", "b150"}));
    // An invalid store changes nothing and notifies nobody.
    EXPECT_EQ(store.store(withZoom(10)).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(store.load().zoomPercent, 150);
    EXPECT_EQ(calls.size(), 3U);
    // A moved token keeps the subscription; the moved-from token is inert.
    ListenerToken moved = std::move(b);
    EXPECT_TRUE(moved.active());
    EXPECT_FALSE(b.active()); // NOLINT(bugprone-use-after-move): the moved-from state is specified.
    b.reset();
    ASSERT_TRUE(store.store(withZoom(175)));
    EXPECT_EQ(calls.back(), "b175");
}

TEST(AppearanceSettings, ReentrantStoreIsBusy) {
    InMemoryAppearanceSettings store;
    std::optional<Error> inner;
    ListenerToken token = store.subscribe([&](const AppearanceSettings&) {
        auto result = store.store(withZoom(200));
        if (!result) {
            inner = result.error();
        }
    });
    ASSERT_TRUE(store.store(withZoom(110)));
    ASSERT_TRUE(inner);
    EXPECT_EQ(inner->code(), ErrorCode::Busy);
    EXPECT_EQ(store.load().zoomPercent, 110) << "the reentrant store changed nothing";
    // The store is usable again after the notification.
    ASSERT_TRUE(store.store(withZoom(90)));
}

TEST(AppearanceSettings, UnsubscribeDuringNotify) {
    InMemoryAppearanceSettings store;
    int secondCalls = 0;
    ListenerToken second;
    ListenerToken first = store.subscribe([&](const AppearanceSettings&) { second.reset(); });
    second = store.subscribe([&](const AppearanceSettings&) { ++secondCalls; });
    ASSERT_TRUE(store.store(withZoom(125)));
    EXPECT_EQ(secondCalls, 0) << "removed before its turn, so not called";
    ASSERT_TRUE(store.store(withZoom(150)));
    EXPECT_EQ(secondCalls, 0);
    // A listener may reset its own token while it runs.
    int selfCalls = 0;
    ListenerToken self;
    self = store.subscribe([&](const AppearanceSettings&) {
        ++selfCalls;
        self.reset();
    });
    ASSERT_TRUE(store.store(withZoom(175)));
    ASSERT_TRUE(store.store(withZoom(200)));
    EXPECT_EQ(selfCalls, 1);
}

TEST(AppearanceSettings, SubscribeDuringNotifyNotCalledThisRound) {
    InMemoryAppearanceSettings store;
    std::vector<int> lateCalls;
    std::vector<ListenerToken> late;
    ListenerToken first = store.subscribe([&](const AppearanceSettings&) {
        late.push_back(store.subscribe([&](const AppearanceSettings& s) { lateCalls.push_back(s.zoomPercent); }));
    });
    ASSERT_TRUE(store.store(withZoom(125)));
    EXPECT_TRUE(lateCalls.empty()) << "a listener added during notify waits for the next store";
    ASSERT_TRUE(store.store(withZoom(150)));
    EXPECT_EQ(lateCalls, std::vector<int>{150});
}

TEST(AppearanceSettings, TokenOutlivesSourceIsNoop) {
    ListenerToken token;
    {
        InMemoryAppearanceSettings store;
        token = store.subscribe([](const AppearanceSettings&) {});
        EXPECT_TRUE(token.active());
    }
    EXPECT_FALSE(token.active());
    token.reset(); // No crash, no use of the destroyed table (ASan).
    ListenerToken empty;
    EXPECT_FALSE(empty.active());
}

TEST(Resolve, SyncWithOsTruthTable) {
    AppearanceSettings settings;
    settings.syncLightTheme = "l";
    settings.syncDarkTheme = "d";
    settings.syncHighContrastTheme = "h";
    for (const bool sync : {false, true}) {
        settings.theme = sync ? std::string(kSyncWithOs) : "chosen";
        for (const ColorScheme scheme : {ColorScheme::Unknown, ColorScheme::Light, ColorScheme::Dark}) {
            for (const bool hc : {false, true}) {
                std::string expected;
                if (!sync) {
                    expected = "chosen";
                } else if (hc) {
                    expected = "h";
                } else if (scheme == ColorScheme::Light) {
                    expected = "l";
                } else {
                    expected = "d"; // Dark, and Unknown resolves to the dark theme.
                }
                EXPECT_EQ(resolveThemeId(settings, {scheme, hc}), expected)
                    << sync << " " << static_cast<int>(scheme) << " " << hc;
            }
        }
    }
    // The result is a copy: a temporary settings argument is safe to use afterwards.
    const std::string id = resolveThemeId(AppearanceSettings{}, {ColorScheme::Light, false});
    EXPECT_EQ(id, "glacier-dark");
}

TEST(Resolve, CombineAppearance) {
    using enum ColorScheme;
    for (const ColorScheme p : {Unknown, Light, Dark}) {
        for (const ColorScheme s : {Unknown, Light, Dark}) {
            for (const bool ph : {false, true}) {
                for (const bool sh : {false, true}) {
                    const OsAppearance out = combineAppearance({p, ph}, {s, sh});
                    EXPECT_EQ(out.scheme, p != Unknown ? p : s);
                    EXPECT_EQ(out.highContrast, ph || sh);
                }
            }
        }
    }
}

TEST(Resolve, LinuxEnvironmentTable) {
    struct Row {
        std::optional<std::string> gtkTheme;
        OsAppearance expected;
    };
    const Row rows[] = {
        {std::nullopt, {ColorScheme::Unknown, false}},
        {"", {ColorScheme::Unknown, false}},
        {"Adwaita", {ColorScheme::Unknown, false}},
        {"Adwaita:dark", {ColorScheme::Dark, false}},
        {"Adwaita:DARK", {ColorScheme::Dark, false}},
        {"Yaru-dark", {ColorScheme::Dark, false}},
        {"Yaru-Dark-Blue", {ColorScheme::Dark, false}},
        {"HighContrast", {ColorScheme::Unknown, true}},
        {"HighContrastInverse-dark", {ColorScheme::Dark, true}},
        {"darkness", {ColorScheme::Unknown, false}},
        {"Adwaita:light", {ColorScheme::Unknown, false}},
    };
    for (const Row& row : rows) {
        const auto getenv = [&](std::string_view name) -> std::optional<std::string> {
            EXPECT_EQ(name, "GTK_THEME");
            return row.gtkTheme;
        };
        EXPECT_EQ(appearanceFromLinuxEnvironment(getenv), row.expected) << row.gtkTheme.value_or("<unset>");
    }
}

} // namespace
} // namespace frost::ui
