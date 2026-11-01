// SPDX-License-Identifier: Apache-2.0
//
// Fuzz-harness replay for the UI tests: runs a harness over a committed corpus directory and
// over deterministic mutations of it (byte flips, truncations, insertions and splices from a
// SplitMix64 stream), so the harnesses run in every normal test tree before a libFuzzer tree
// exists. Header-only; uses gtest and std::filesystem (test code).
#pragma once

#include "frost/core/error.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace frost::ui::fuzz {

using Harness = std::function<Result<void>(std::span<const std::uint8_t>)>;

// Every regular file of `dir`, sorted by name. A missing directory reads as empty.
inline std::vector<std::vector<std::uint8_t>> readCorpus(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::vector<std::vector<std::uint8_t>> corpus;
    for (const auto& file : files) {
        std::ifstream in(file, std::ios::binary);
        corpus.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return corpus;
}

// Runs `harness` over `seeds` and `mutations` mutated inputs; records a test failure per
// harness error (with the input's origin). SplitMix64 (see model_test_support.hpp) seeded with
// `seed` drives the mutations.
inline void replay(const Harness& harness, const std::vector<std::vector<std::uint8_t>>& seeds, int mutations,
                   std::uint64_t seed) {
    ASSERT_FALSE(seeds.empty()) << "no seed inputs";
    const auto run = [&](std::span<const std::uint8_t> input, const std::string& origin) {
        const auto result = harness(input);
        EXPECT_TRUE(result) << origin << ": " << result.error().describe();
    };
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        run(seeds[i], "seed " + std::to_string(i));
    }
    std::uint64_t state = seed;
    const auto next = [&state] {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    };
    for (int m = 0; m < mutations; ++m) {
        std::vector<std::uint8_t> input = seeds[next() % seeds.size()];
        const int edits = 1 + static_cast<int>(next() % 4);
        for (int e = 0; e < edits; ++e) {
            switch (next() % 5) {
            case 0: // Flip one bit.
                if (!input.empty()) {
                    input[next() % input.size()] ^= static_cast<std::uint8_t>(1U << (next() % 8));
                }
                break;
            case 1: // Overwrite one byte.
                if (!input.empty()) {
                    input[next() % input.size()] = static_cast<std::uint8_t>(next());
                }
                break;
            case 2: // Truncate.
                input.resize(input.empty() ? 0 : next() % input.size());
                break;
            case 3: { // Insert a few random bytes.
                const std::size_t at = input.empty() ? 0 : next() % (input.size() + 1);
                const std::size_t count = 1 + next() % 8;
                std::vector<std::uint8_t> bytes(count);
                for (std::uint8_t& b : bytes) {
                    b = static_cast<std::uint8_t>(next());
                }
                input.insert(input.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin(), bytes.end());
                break;
            }
            default: { // Splice: this input's prefix and another seed's suffix.
                const std::vector<std::uint8_t>& other = seeds[next() % seeds.size()];
                const std::size_t keep = input.empty() ? 0 : next() % input.size();
                const std::size_t from = other.empty() ? 0 : next() % other.size();
                input.resize(keep);
                input.insert(input.end(), other.begin() + static_cast<std::ptrdiff_t>(from), other.end());
                break;
            }
            }
        }
        run(input, "mutation " + std::to_string(m));
    }
}

} // namespace frost::ui::fuzz
