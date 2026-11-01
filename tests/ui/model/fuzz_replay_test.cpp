// SPDX-License-Identifier: Apache-2.0
//
// Replays the theme blob and icon atlas fuzz harnesses (tests/ui/fuzz) over their committed
// corpora (tests/fuzz/corpus/{theme_blob,icon_atlas}) plus generated valid inputs, and 2,000
// deterministic mutations each.

#include "frost/ui/model/icon_atlas_format.hpp"
#include "frost/ui/model/theme_blob.hpp"

#include "fuzz/icon_atlas_harness.hpp"
#include "fuzz/replay.hpp"
#include "fuzz/theme_blob_harness.hpp"
#include "model_test_support.hpp"
#include <gtest/gtest.h>

#include <filesystem>

namespace frost::ui {
namespace {

std::vector<std::uint8_t> toVector(std::span<const std::byte> bytes) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    return {data, data + bytes.size()};
}

const std::filesystem::path kCorpusRoot = std::filesystem::path(FROST_SOURCE_DIR) / "tests" / "fuzz" / "corpus";

TEST(UiFuzzReplay, ThemeBlob) {
    auto seeds = fuzz::readCorpus(kCorpusRoot / "theme_blob");
    EXPECT_GE(seeds.size(), 4U) << "the four compiled Glacier themes are committed as seeds";
    test::SplitMix64 rng(31);
    for (int i = 0; i < 4; ++i) {
        seeds.push_back(toVector(encodeThemeBlob(test::randomThemeData(rng))));
    }
    fuzz::replay(fuzz::runThemeBlobInput, seeds, 2000, 0x7e1e);
}

TEST(UiFuzzReplay, IconAtlas) {
    auto seeds = fuzz::readCorpus(kCorpusRoot / "icon_atlas");
    EXPECT_GE(seeds.size(), 1U) << "atlases baked from the synthetic icon fixtures are committed as seeds";
    // A small generated atlas with every cell kind (tinted, untinted, dark variant).
    test::SplitMix64 rng(37);
    AtlasWriter writer(125, 7);
    const auto cell = [&](std::uint16_t px, bool tinted) {
        AtlasCell c{px, px, std::vector<std::byte>(std::size_t{px} * px * (tinted ? 1U : 4U))};
        for (std::byte& b : c.pixels) {
            b = static_cast<std::byte>(rng.next());
        }
        return c;
    };
    ASSERT_TRUE(writer.add("actions.add", 16, true, IconRole::Default, cell(20, true)));
    ASSERT_TRUE(writer.add("actions.add", 20, true, IconRole::Default, cell(25, true)));
    ASSERT_TRUE(writer.add("nodes.file", 16, false, IconRole::Default, cell(20, false), cell(20, false)));
    seeds.push_back(toVector(*writer.finish()));
    fuzz::replay(fuzz::runIconAtlasInput, seeds, 2000, 0xa71a5);
}

} // namespace
} // namespace frost::ui
