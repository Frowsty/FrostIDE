// SPDX-License-Identifier: Apache-2.0
//
// Tests of Theme validation, the theme blob v1 codec and the theme file loaders
// (frost/ui/model/{theme,theme_blob}.hpp): round trips, truncation and bit-flip sweeps, field
// corruptions, MemFs fault injection, and concurrent reads of a shared Theme (TSan).

#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/ui/model/fnv1a.hpp"
#include "frost/ui/model/theme_blob.hpp"

#include "model_test_support.hpp"
#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace frost::ui {
namespace {

using test::randomThemeData;
using test::sampleThemeData;
using test::SplitMix64;

// The offsets of the header fields and body sections (theme_blob.hpp).
constexpr std::size_t kColors = kThemeBlobHeaderSize;
constexpr std::size_t kMetrics = kColors + kColorKeyCount * 4;
constexpr std::size_t kRoles = kMetrics + kMetricKeyCount * 8;
constexpr std::size_t kStrings = kRoles + kIconRoleCount * 4;

// Rewrites the checksum after a deliberate field change, so the decoder reaches the field check.
void reseal(std::vector<std::byte>& blob) {
    const std::uint64_t sum = fnv1a64(std::span(blob).subspan(48), fnv1a64(std::span<const std::byte>(blob).first(40)));
    std::memcpy(blob.data() + 40, &sum, 8);
}

template<class T>
void poke(std::vector<std::byte>& blob, std::size_t offset, T value) {
    std::memcpy(blob.data() + offset, &value, sizeof(T));
}

TEST(Theme, FromDataValidates) {
    ASSERT_TRUE(Theme::fromData(sampleThemeData()));
    const auto rejects = [](auto mutate, std::string_view what) {
        ThemeData data = sampleThemeData();
        mutate(data);
        const auto theme = Theme::fromData(data);
        ASSERT_FALSE(theme) << what;
        EXPECT_EQ(theme.error().code(), ErrorCode::InvalidArgument) << what;
    };
    rejects([](ThemeData& d) { d.id = ""; }, "empty id");
    rejects([](ThemeData& d) { d.id = "-dark"; }, "leading dash");
    rejects([](ThemeData& d) { d.id = "Glacier"; }, "upper case");
    rejects([](ThemeData& d) { d.id = std::string(65, 'a'); }, "id over 64");
    rejects([](ThemeData& d) { d.name = ""; }, "empty name");
    rejects([](ThemeData& d) { d.name = std::string(65, 'n'); }, "name over 64 bytes");
    rejects([](ThemeData& d) { d.name = "bad \xff utf8"; }, "invalid UTF-8");
    rejects([](ThemeData& d) { d.metrics[0] = std::numeric_limits<float>::quiet_NaN(); }, "NaN");
    rejects([](ThemeData& d) { d.compactMetrics[0] = std::numeric_limits<float>::infinity(); }, "inf");
    rejects([](ThemeData& d) { d.metrics[static_cast<std::size_t>(MetricKey::ButtonHeight)] = 65.0F; }, "range");
    rejects(
        [](ThemeData& d) {
            const auto i = static_cast<std::size_t>(MetricKey::ComponentArc);
            d.compactMetrics[i] = d.metrics[i] + 1.0F;
        },
        "compact slot differs without hasCompact");
    rejects(
        [](ThemeData& d) {
            const auto i = static_cast<std::size_t>(MetricKey::ButtonHeight);
            d.metrics[i] = 20.0F;
            d.compactMetrics[i] = 24.0F;
        },
        "compact above normal");
    rejects([](ThemeData& d) { d.colors[static_cast<std::size_t>(ColorKey::IslandBackground)] = Rgba{0x80ffffffU}; },
            "translucent opaque key");
    EXPECT_TRUE(isValidThemeId(std::string(64, 'a')));
    EXPECT_TRUE(isValidThemeId("0-9"));
    EXPECT_FALSE(isValidThemeId("a_b"));
}

TEST(ThemeBlob, RoundTripProperty) {
    SplitMix64 rng(20261101);
    for (int i = 0; i < 1000; ++i) {
        const ThemeData data = randomThemeData(rng);
        ASSERT_TRUE(Theme::fromData(data)) << Theme::fromData(data).error().describe();
        const std::vector<std::byte> blob = encodeThemeBlob(data);
        ASSERT_EQ(blob.size(), kStrings + data.id.size() + data.name.size());
        const auto theme = decodeThemeBlob(blob);
        ASSERT_TRUE(theme) << theme.error().describe();
        ASSERT_EQ(theme->data(), data);
        ASSERT_EQ(encodeThemeBlob(theme->data()), blob) << "deterministic";
    }
}

TEST(ThemeBlob, AccessorsAndSize) {
    ThemeData data = sampleThemeData("glacier-dark", "Glacier Dark");
    data.colors[static_cast<std::size_t>(ColorKey::LinkForeground)] = Rgba{0x806ca2ffU};
    const auto ti = static_cast<std::size_t>(MetricKey::TreeRowHeight);
    data.metrics[ti] = 24.0F;
    data.compactMetrics[ti] = 20.0F;
    data.iconColors[static_cast<std::size_t>(IconRole::Error)] = Rgba{0xfff2716fU};
    const std::vector<std::byte> blob = encodeThemeBlob(data);
    // The header, the body arrays (716 bytes), the id and the name: 788 bytes here.
    EXPECT_EQ(blob.size(), kThemeBlobHeaderSize + 4 * kColorKeyCount + 8 * kMetricKeyCount + 4 * kIconRoleCount +
                               data.id.size() + data.name.size());
    EXPECT_EQ(4 * kColorKeyCount + 8 * kMetricKeyCount + 4 * kIconRoleCount, 716U);
    EXPECT_EQ(blob.size(), 788U);
    const Theme theme = *decodeThemeBlob(blob);
    EXPECT_EQ(theme.id(), "glacier-dark");
    EXPECT_EQ(theme.name(), "Glacier Dark");
    EXPECT_TRUE(theme.isDark());
    EXPECT_FALSE(theme.isHighContrast());
    EXPECT_EQ(theme.color(ColorKey::LinkForeground), Rgba{0x806ca2ffU});
    EXPECT_EQ(theme.metric(MetricKey::TreeRowHeight), 24.0F);
    EXPECT_EQ(theme.metric(MetricKey::TreeRowHeight, Density::Compact), 20.0F);
    EXPECT_EQ(theme.iconColor(IconRole::Error), Rgba{0xfff2716fU});
}

TEST(ThemeBlob, TruncationSweep) {
    const std::vector<std::byte> blob = encodeThemeBlob(sampleThemeData());
    for (std::size_t n = 0; n < blob.size(); ++n) {
        const auto theme = decodeThemeBlob(std::span(blob).first(n));
        ASSERT_FALSE(theme) << n;
        ASSERT_EQ(theme.error().code(), ErrorCode::Corrupt) << n;
    }
    std::vector<std::byte> longer = blob;
    longer.push_back(std::byte{0});
    EXPECT_EQ(decodeThemeBlob(longer).error().code(), ErrorCode::Corrupt);
}

TEST(ThemeBlob, EveryBitFlipRejected) {
    SplitMix64 rng(3);
    const std::vector<std::byte> blob = encodeThemeBlob(randomThemeData(rng));
    for (std::size_t bit = 0; bit < blob.size() * 8; ++bit) {
        std::vector<std::byte> flipped = blob;
        flipped[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
        const auto theme = decodeThemeBlob(flipped);
        ASSERT_FALSE(theme) << "bit " << bit;
        const ErrorCode code = theme.error().code();
        ASSERT_TRUE(code == ErrorCode::Corrupt || code == ErrorCode::Unsupported) << "bit " << bit;
    }
}

TEST(ThemeBlob, FieldCorruptions) {
    const std::vector<std::byte> good = encodeThemeBlob(sampleThemeData());
    const auto expect = [&](auto mutate, ErrorCode code, std::string_view what) {
        std::vector<std::byte> blob = good;
        mutate(blob);
        reseal(blob);
        const auto theme = decodeThemeBlob(blob);
        ASSERT_FALSE(theme) << what;
        EXPECT_EQ(theme.error().code(), code) << what << ": " << theme.error().describe();
    };
    using V = std::vector<std::byte>;
    expect([](V& b) { b[0] = std::byte{'X'}; }, ErrorCode::Corrupt, "magic");
    expect([](V& b) { poke<std::uint16_t>(b, 8, 2); }, ErrorCode::Unsupported, "version");
    expect([](V& b) { poke<std::uint64_t>(b, 16, kRegistryHash ^ 1U); }, ErrorCode::Unsupported, "registry hash");
    expect([](V& b) { poke<std::uint16_t>(b, 10, 4); }, ErrorCode::Corrupt, "unknown flag bit");
    expect([](V& b) { poke<std::uint32_t>(b, 12, 1); }, ErrorCode::Corrupt, "reserved [12,16)");
    expect([](V& b) { poke<std::uint16_t>(b, 34, 1); }, ErrorCode::Corrupt, "reserved [34,36)");
    expect([](V& b) { poke<std::uint16_t>(b, 24, static_cast<std::uint16_t>(kColorKeyCount + 1)); }, ErrorCode::Corrupt,
           "colour count");
    expect([](V& b) { poke<std::uint16_t>(b, 26, static_cast<std::uint16_t>(kMetricKeyCount - 1)); },
           ErrorCode::Corrupt, "metric count");
    expect([](V& b) { poke<std::uint16_t>(b, 28, 0); }, ErrorCode::Corrupt, "role count");
    expect([](V& b) { poke<std::uint16_t>(b, 30, 1); }, ErrorCode::Corrupt, "id length");
    expect([](V& b) { poke<std::uint16_t>(b, 32, 0xffff); }, ErrorCode::Corrupt, "name length");
    expect([](V& b) { poke<std::uint32_t>(b, 36, 0); }, ErrorCode::Corrupt, "total size");
    expect([](V& b) { poke<float>(b, kMetrics, std::numeric_limits<float>::quiet_NaN()); }, ErrorCode::Corrupt, "NaN");
    expect([](V& b) { poke<float>(b, kMetrics + 4, std::numeric_limits<float>::infinity()); }, ErrorCode::Corrupt,
           "+inf");
    expect([](V& b) { poke<float>(b, kMetrics, -std::numeric_limits<float>::infinity()); }, ErrorCode::Corrupt, "-inf");
    expect([](V& b) { poke<float>(b, kMetrics + static_cast<std::size_t>(MetricKey::ButtonHeight) * 8, 1000.0F); },
           ErrorCode::Corrupt, "metric out of range");
    expect(
        [](V& b) {
            const std::size_t at = kMetrics + static_cast<std::size_t>(MetricKey::ComponentArc) * 8;
            poke<float>(b, at + 4, 3.0F);
        },
        ErrorCode::Corrupt, "compact slot without hasCompact");
    expect(
        [](V& b) {
            poke<std::uint32_t>(b, kColors + static_cast<std::size_t>(ColorKey::MainWindowBackground) * 4, 0x7f000000U);
        },
        ErrorCode::Corrupt, "translucent opaque key");
    expect([](V& b) { b[kStrings] = std::byte{'A'}; }, ErrorCode::Corrupt, "invalid id");
    expect([](V& b) { b[b.size() - 1] = std::byte{0xff}; }, ErrorCode::Corrupt, "non-UTF-8 name");
    // A bad checksum (not resealed).
    std::vector<std::byte> blob = good;
    blob[kColors] ^= std::byte{1};
    EXPECT_EQ(decodeThemeBlob(blob).error().code(), ErrorCode::Corrupt);
}

TEST(ThemeBlob, LoadThroughMemFs) {
    ManualClock clock;
    io::MemFs fs(clock, io::PathStyle::Posix);
    const std::vector<std::byte> blob = encodeThemeBlob(sampleThemeData("glacier-dark", "Glacier Dark"));
    ASSERT_TRUE(fs.createDirectories("/res/themes"));
    ASSERT_TRUE(fs.writeFileAtomic("/res/themes/glacier-dark.ftheme", test::toString(blob)));
    const std::string path = "/res/themes/glacier-dark.ftheme";

    const auto loaded = loadThemeFile(fs, path, "glacier-dark");
    ASSERT_TRUE(loaded) << loaded.error().describe();
    EXPECT_EQ(loaded->name(), "Glacier Dark");

    for (const ErrorCode code : {ErrorCode::IoError, ErrorCode::PermissionDenied, ErrorCode::Busy}) {
        fs.injectFault(io::FsOp::Read, path, code);
        const auto failed = loadThemeFile(fs, path, "glacier-dark");
        ASSERT_FALSE(failed);
        EXPECT_EQ(failed.error().code(), code);
        ASSERT_FALSE(failed.error().context().empty());
        EXPECT_NE(failed.error().context().back().find(path), std::string::npos) << failed.error().describe();
    }
    fs.injectFault(io::FsOp::Status, path, ErrorCode::PermissionDenied);
    EXPECT_EQ(loadThemeFile(fs, path, "glacier-dark").error().code(), ErrorCode::PermissionDenied);

    const auto missing = loadThemeFile(fs, "/res/themes/nope.ftheme", "nope");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);

    EXPECT_EQ(loadThemeFile(fs, path, "glacier-light").error().code(), ErrorCode::Corrupt) << "id mismatch";

    ASSERT_TRUE(fs.writeFileAtomic("/res/themes/big.ftheme", std::string(kMaxThemeFileBytes + 1, 'x')));
    EXPECT_EQ(loadThemeFile(fs, "/res/themes/big.ftheme", "big").error().code(), ErrorCode::Corrupt);
    ASSERT_TRUE(fs.createDirectories("/res/themes/dir.ftheme"));
    EXPECT_EQ(loadThemeFile(fs, "/res/themes/dir.ftheme", "dir").error().code(), ErrorCode::InvalidArgument);
}

TEST(ThemeBlob, ListInstalledThemes) {
    // listInstalledThemes joins paths in the native style, so the MemFs uses it too.
    ManualClock clock;
    io::MemFs fs(clock);
#ifdef _WIN32
    fs.addRoot("C:\\");
    const std::string dir = "C:\\themes";
    const auto at = [&](std::string_view name) { return dir + "\\" + std::string(name); };
#else
    const std::string dir = "/themes";
    const auto at = [&](std::string_view name) { return dir + "/" + std::string(name); };
#endif
    ASSERT_TRUE(fs.createDirectories(dir));
    ThemeData light = sampleThemeData("glacier-light", "Glacier Light");
    light.dark = false;
    ThemeData hc = sampleThemeData("glacier-high-contrast", "Glacier High Contrast");
    hc.highContrast = true;
    ASSERT_TRUE(fs.writeFileAtomic(at("glacier-light.ftheme"), test::toString(encodeThemeBlob(light))));
    ASSERT_TRUE(fs.writeFileAtomic(at("glacier-high-contrast.ftheme"), test::toString(encodeThemeBlob(hc))));
    ASSERT_TRUE(fs.writeFileAtomic(at("broken.ftheme"), "not a theme"));
    ASSERT_TRUE(fs.writeFileAtomic(at("renamed.ftheme"), test::toString(encodeThemeBlob(light))));
    ASSERT_TRUE(fs.writeFileAtomic(at("readme.txt"), "ignored"));
    std::vector<Error> skipped;
    const auto themes = listInstalledThemes(fs, dir, &skipped);
    ASSERT_TRUE(themes) << themes.error().describe();
    ASSERT_EQ(themes->size(), 2U);
    EXPECT_EQ((*themes)[0], (ThemeInfo{"glacier-high-contrast", "Glacier High Contrast", true, true}));
    EXPECT_EQ((*themes)[1], (ThemeInfo{"glacier-light", "Glacier Light", false, false}));
    EXPECT_EQ(skipped.size(), 2U) << "broken.ftheme and renamed.ftheme";
    EXPECT_EQ(listInstalledThemes(fs, at("missing")).error().code(), ErrorCode::NotFound);
}

TEST(Theme, ConcurrentReadsOfSharedTheme) {
    SplitMix64 rng(99);
    const auto theme = std::make_shared<const Theme>(*Theme::fromData(randomThemeData(rng)));
    std::vector<std::thread> threads;
    std::vector<std::uint64_t> sums(4);
    for (std::size_t t = 0; t < 4; ++t) {
        threads.emplace_back([&theme, &sums, t] {
            std::uint64_t sum = 0;
            for (int round = 0; round < 200; ++round) {
                for (const ColorKeyInfo& key : kColorKeys) {
                    sum += theme->color(key.key).argb;
                }
                for (const MetricKeyInfo& key : kMetricKeys) {
                    sum += static_cast<std::uint64_t>(theme->metric(key.key, Density::Compact));
                }
                for (const IconRoleInfo& role : kIconRoles) {
                    sum += theme->iconColor(role.role).argb;
                }
                sum += theme->id().size() + theme->name().size();
            }
            sums[t] = sum;
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(sums[0], sums[1]);
    EXPECT_EQ(sums[1], sums[2]);
    EXPECT_EQ(sums[2], sums[3]);
}

} // namespace
} // namespace frost::ui
