// SPDX-License-Identifier: Apache-2.0
//
// In-process appearance probes (label `perf`; ADR 0015 measurements, reported against B1).
// 50 iterations each; p50 and p99 are printed and recorded as test properties. Assertions run
// only in optimised, unsanitised builds, where timings mean something. These are new probes,
// not budgets: B1 itself is measured on screen by m1/automation-bench.

#include "frost/io/path.hpp"
#include "frost/ui/appearance_manager.hpp"
#include "frost/ui/fonts.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_atlas_set.hpp"
#include "frost/ui/model/theme_blob.hpp"

#include "support/appearance_fixture.hpp"
#include <QApplication>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace frost::ui {
namespace {

#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer)) ||                   \
    (defined(__has_feature) && __has_feature(thread_sanitizer)) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#ifdef NDEBUG
constexpr bool kOptimized = true;
#else
constexpr bool kOptimized = false;
#endif
constexpr bool kAssertBudgets = kOptimized && !kSanitized;
constexpr int kIterations = 50;

struct Percentiles {
    double p50Ms = 0;
    double p99Ms = 0;
};

// Runs `body` once cold (reported as "first") and then kIterations times, and reports the
// percentiles of the warm runs as `name`.
Percentiles measure(const char* name, const std::function<void()>& body) {
    const auto coldStart = std::chrono::steady_clock::now();
    body();
    const double firstMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - coldStart).count();
    std::vector<double> samples;
    samples.reserve(kIterations);
    for (int i = 0; i < kIterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        body();
        samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::ranges::sort(samples);
    const Percentiles result{samples[samples.size() / 2],
                             samples[std::min(samples.size() - 1, (samples.size() * 99) / 100)]};
    std::printf("[ui perf] %-26s first %.3f ms, p50 %.3f ms, p99 %.3f ms\n", name, firstMs, result.p50Ms, result.p99Ms);
    ::testing::Test::RecordProperty(std::string(name) + "_first_ms", std::to_string(firstMs));
    ::testing::Test::RecordProperty(std::string(name) + "_p50_ms", std::to_string(result.p50Ms));
    ::testing::Test::RecordProperty(std::string(name) + "_p99_ms", std::to_string(result.p99Ms));
    return result;
}

std::string resource(std::string_view dir, std::string_view file) {
    return *io::joinPath(*io::joinPath(test::resourceDir(), dir), file);
}

TEST(AppearancePerf, ThemeDecode) {
    const std::string path = resource("themes", "glacier-dark.ftheme");
    const auto timing = measure("theme_decode", [&] {
        auto theme = loadThemeFile(io::realFileSystem(), path, "glacier-dark");
        ASSERT_TRUE(theme);
    });
    if (kAssertBudgets) {
        EXPECT_LE(timing.p50Ms, 1.0);
    }
}

TEST(AppearancePerf, StyleConstructionAndSetStyle) {
    auto theme = loadThemeFile(io::realFileSystem(), resource("themes", "glacier-dark.ftheme"), "glacier-dark");
    ASSERT_TRUE(theme);
    const auto shared = std::make_shared<const Theme>(std::move(*theme));
    std::uint64_t generation = 1000;
    const auto timing = measure("style_construct_and_set", [&] {
        auto* style = new FrostStyle(shared, UiScale{}, nullptr, ++generation);
        QApplication::setStyle(style);
        QApplication::setPalette(style->standardPalette());
    });
    QApplication::setStyle(QStringLiteral("Fusion"));
    if (kAssertBudgets) {
        EXPECT_LE(timing.p50Ms, 5.0);
    }
}

TEST(AppearancePerf, AtlasOpenOneScale) {
    const std::string dir = *io::joinPath(test::resourceDir(), "icons");
    const std::array<std::uint16_t, 1> preload{100};
    const auto timing = measure("atlas_open_100", [&] {
        auto set = IconAtlasSet::open(io::realFileSystem(), dir, preload);
        ASSERT_TRUE(set);
    });
    if (kAssertBudgets) {
        EXPECT_LE(timing.p50Ms, 1.0);
    }
}

TEST(AppearancePerf, FontRegistrationAndInstallReported) {
    const std::string fonts = *io::joinPath(*io::joinPath(test::resourceDir(), "fonts"), "inter");
    measure("ui_font_registration", [&] {
        auto registered = registerBundledFonts(io::realFileSystem(), fonts, FontSet::Ui);
        ASSERT_TRUE(registered);
    });
    measure("install_end_to_end", [&] {
        const auto installed = test::install({});
        ASSERT_TRUE(installed);
    });
    // Reported only (ADR 0015, against B1).
}

} // namespace
} // namespace frost::ui
