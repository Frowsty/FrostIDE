// SPDX-License-Identifier: Apache-2.0
//
// KeymapFuzzReplay: replays the input_keymap seed corpus (tests/fuzz/corpus/input_keymap)
// through the fuzz core in every normal tree, plus seeded mutations of it. TODO(M1): delete
// once fuzz_replay.input_keymap is registered with frost_add_fuzz_target (FrostFuzz.cmake
// lands with m1/piece-tree).

#include "frost/input/keymap.hpp"

#include "input/fuzz/keymap_fuzz_core.hpp"
#include "input/support/test_actions.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace frost::input {
namespace {

std::vector<std::vector<std::uint8_t>> corpus() {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(FROST_INPUT_FUZZ_CORPUS)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    std::vector<std::vector<std::uint8_t>> inputs;
    for (const auto& file : files) {
        std::ifstream in(file, std::ios::binary);
        inputs.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return inputs;
}

TEST(KeymapFuzzReplay, SeedCorpus) {
    const auto inputs = corpus();
    ASSERT_GE(inputs.size(), 12U) << FROST_INPUT_FUZZ_CORPUS;
    std::size_t parsed = 0;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto result = test::runKeymapFuzzInput(inputs[i]);
        EXPECT_TRUE(result.has_value()) << "input " << i << ": " << result.error().message();
        const std::string_view text(reinterpret_cast<const char*>(inputs[i].data()), inputs[i].size());
        parsed += parseKeymap(text).has_value() ? 1U : 0U;
    }
    EXPECT_GE(parsed, 2U) << "the corpus holds valid keymaps";
    EXPECT_LT(parsed, inputs.size()) << "and invalid ones";
}

TEST(KeymapFuzzReplay, BuiltinKeymapAndEmptyInput) {
    const std::string_view text = builtinKeymapText("intellij");
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
    EXPECT_TRUE(test::runKeymapFuzzInput(std::span(bytes, text.size())).has_value());
    EXPECT_TRUE(test::runKeymapFuzzInput({}).has_value());
}

TEST(KeymapFuzzReplay, SeededMutations) {
    const auto inputs = corpus();
    ASSERT_FALSE(inputs.empty());
    std::mt19937_64 rng(0xF022u);
    constexpr std::string_view kTokens[] = {
        "Ctrl+",       "Alt+",      "Shift+", "Meta+", "DoubleShift",      ", ",          " | ",      " = ",
        "[default]\n", "[macos]\n", "\n",     "\r\n",  "frost-keymap 1\n", "editor.copy", "\xC3\xBC", "\xFF",
        "#",           "Ctrl+K, C"};
    for (int i = 0; i < 3000; ++i) {
        std::vector<std::uint8_t> data = inputs[rng() % inputs.size()];
        const int edits = 1 + static_cast<int>(rng() % 6);
        for (int e = 0; e < edits; ++e) {
            const std::size_t at = data.empty() ? 0 : rng() % (data.size() + 1);
            switch (rng() % 4) {
            case 0: // Flip a byte.
                if (!data.empty() && at < data.size()) {
                    data[at] = static_cast<std::uint8_t>(rng());
                }
                break;
            case 1: // Delete a run.
                if (at < data.size()) {
                    const std::size_t n = std::min<std::size_t>(rng() % 16, data.size() - at);
                    data.erase(data.begin() + static_cast<std::ptrdiff_t>(at),
                               data.begin() + static_cast<std::ptrdiff_t>(at + n));
                }
                break;
            default: { // Insert a token.
                const std::string_view token = kTokens[rng() % std::size(kTokens)];
                data.insert(data.begin() + static_cast<std::ptrdiff_t>(at), token.begin(), token.end());
                break;
            }
            }
        }
        const auto result = test::runKeymapFuzzInput(data);
        ASSERT_TRUE(result.has_value()) << "mutation " << i << ": " << result.error().message();
    }
}

} // namespace
} // namespace frost::input
