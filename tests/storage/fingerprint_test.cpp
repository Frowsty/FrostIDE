// SPDX-License-Identifier: Apache-2.0
//
// XXH3 fingerprints: the regression golden of the persisted format (goldens/xxh3_vectors.txt),
// streaming against one-shot properties, the canonical form and hex parsing. xxHash's known
// answers are not used: its BSD-2 sources and documentation publish none for these inputs, and
// its test vectors are in GPL-2.0 files the clean-room rule excludes, so the golden is a
// regression pin produced by this build (FROST_STORAGE_REGENERATE_GOLDENS=1 rewrites it), not an
// independent oracle.

#include "frost/storage/fingerprint.hpp"

#include "support/storage_test_support.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

using frost::storage::Fingerprint128;
using frost::storage::xxh3Hash128;
using frost::storage::xxh3Hash128Concat;
using frost::storage::xxh3Hash64;
using frost::storage::Xxh3Stream128;
using frost::storage::Xxh3Stream64;

// The golden's input: bytes (i * 31 + 7) & 0xFF.
std::string goldenInput(std::size_t length) {
    std::string out(length, '\0');
    for (std::size_t i = 0; i < length; ++i) {
        out[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    }
    return out;
}

constexpr std::size_t kGoldenLengths[] = {
    0, 1, 3, 4, 8, 9, 16, 17, 128, 129, 240, 241, 1023, 1024, 1025, 65536, std::size_t{1} << 20};
constexpr std::uint64_t kGoldenSeeds[] = {0, 0x9E3779B97F4A7C15ull};

std::string goldenText() {
    std::string text = "# length seed xxh3-64 xxh3-128 (canonical hex); input bytes (i*31+7)&0xFF\n";
    for (const std::size_t length : kGoldenLengths) {
        const std::string input = goldenInput(length);
        for (const std::uint64_t seed : kGoldenSeeds) {
            text += std::format("{} {:016x} {:016x} {}\n", length, seed, xxh3Hash64(input, seed),
                                xxh3Hash128(input, seed).toHex());
        }
    }
    return text;
}

TEST(Fingerprint, Golden0_8_4) {
    const std::string path = std::string(FROST_STORAGE_GOLDEN_DIR) + "/xxh3_vectors.txt";
    const std::string expected = goldenText();
    if (frost::storage::test::environment("FROST_STORAGE_REGENERATE_GOLDENS") == "1") {
        std::ofstream(path, std::ios::binary) << expected;
    }
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in) << "missing golden " << path;
    std::stringstream buffer;
    buffer << in.rdbuf();
    EXPECT_EQ(buffer.str(), expected) << "XXH3 output changed: persisted fingerprints would no longer match";
}

TEST(Fingerprint, StreamingEqualsOneShotAtEveryBoundary) {
    // Splits around the stripe (64 bytes) and block (1024 bytes) boundaries of XXH3's streaming code.
    const std::string input = goldenInput(4096 + 300);
    const std::size_t splits[] = {0, 1, 63, 64, 65, 127, 128, 129, 239, 240, 241, 1023, 1024, 1025, 2048, 4096};
    for (const std::size_t first : splits) {
        for (const std::size_t second : splits) {
            if (first + second > input.size()) {
                continue;
            }
            const std::string_view a(input.data(), first);
            const std::string_view b(input.data() + first, second);
            const std::string_view c(input.data() + first + second, input.size() - first - second);
            Xxh3Stream64 s64;
            Xxh3Stream128 s128;
            for (const std::string_view piece : {a, b, c}) {
                s64.update(piece);
                s128.update(piece);
            }
            EXPECT_EQ(s64.digest(), xxh3Hash64(input)) << first << "+" << second;
            EXPECT_EQ(s128.digest(), xxh3Hash128(input)) << first << "+" << second;
        }
    }
}

TEST(Fingerprint, RandomChunkingProperty) {
    for (std::uint64_t seed = 0; seed < 500; ++seed) {
        std::mt19937_64 rng(seed);
        const std::size_t length = rng() % 5000;
        const std::string input = frost::storage::test::randomBytes(length, seed * 7 + 1);
        const std::uint64_t hashSeed = (seed % 3 == 0) ? 0 : rng();
        Xxh3Stream64 s64(hashSeed);
        Xxh3Stream128 s128(hashSeed);
        std::vector<std::string_view> pieces;
        for (std::size_t offset = 0; offset < length;) {
            const std::size_t take = std::min<std::size_t>(length - offset, rng() % 700);
            const std::string_view piece(input.data() + offset, take);
            s64.update(piece);
            s128.update(piece);
            pieces.push_back(piece);
            offset += take;
        }
        ASSERT_EQ(s64.digest(), xxh3Hash64(input, hashSeed)) << "seed " << seed;
        ASSERT_EQ(s128.digest(), xxh3Hash128(input, hashSeed)) << "seed " << seed;
        if (hashSeed == 0) {
            ASSERT_EQ(xxh3Hash128Concat(pieces), xxh3Hash128(input)) << "seed " << seed;
        }
        // digest() does not end the stream; reset() starts over with the same seed.
        s128.reset();
        s128.update(input);
        ASSERT_EQ(s128.digest(), xxh3Hash128(input, hashSeed));
    }
}

TEST(Fingerprint, ConcatEqualsConcatenation) {
    const std::string a = "hello, ";
    const std::string b;
    const std::string c = frost::storage::test::randomBytes(3000, 3);
    const std::string_view pieces[] = {a, b, c, a};
    EXPECT_EQ(xxh3Hash128Concat(pieces), xxh3Hash128(a + b + c + a));
    EXPECT_EQ(xxh3Hash128Concat({}), xxh3Hash128(""));
}

TEST(Fingerprint, SeedChangesResult) {
    const std::string input = goldenInput(100);
    EXPECT_NE(xxh3Hash64(input, 0), xxh3Hash64(input, 1));
    EXPECT_NE(xxh3Hash128(input, 0), xxh3Hash128(input, 1));
    Xxh3Stream64 seeded(1);
    seeded.update(input);
    EXPECT_EQ(seeded.digest(), xxh3Hash64(input, 1));
}

TEST(Fingerprint, MoveTransfersState) {
    Xxh3Stream128 a;
    a.update("abc");
    Xxh3Stream128 b(std::move(a));
    b.update("def");
    EXPECT_EQ(b.digest(), xxh3Hash128("abcdef"));
    Xxh3Stream128 c;
    c = std::move(b);
    EXPECT_EQ(c.digest(), xxh3Hash128("abcdef"));
}

TEST(Fingerprint, HexRoundTripAndRejectsMalformed) {
    const Fingerprint128 value{0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
    EXPECT_EQ(value.toHex(), "0123456789abcdeffedcba9876543210");
    auto parsed = Fingerprint128::fromHex("0123456789ABCDEFfedcba9876543210");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, value);
    for (const std::string_view bad :
         {"", "0123", "0123456789abcdeffedcba987654321", "0123456789abcdeffedcba98765432100",
          "0123456789abcdeffedcba987654321g", " 123456789abcdeffedcba9876543210"}) {
        auto rejected = Fingerprint128::fromHex(bad);
        ASSERT_FALSE(rejected.has_value()) << bad;
        EXPECT_EQ(rejected.error().code(), frost::ErrorCode::InvalidArgument);
    }
}

TEST(Fingerprint, CanonicalBytesBigEndianAndOrderPreserving) {
    const Fingerprint128 value{0x0102030405060708ull, 0x090A0B0C0D0E0F10ull};
    const auto bytes = value.canonicalBytes();
    for (std::size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(static_cast<unsigned>(bytes[i]), i + 1);
    }
    auto back = Fingerprint128::fromCanonicalBytes(std::string_view(reinterpret_cast<const char*>(bytes.data()), 16));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, value);
    EXPECT_FALSE(Fingerprint128::fromCanonicalBytes("short").has_value());
    // Property: memcmp of the canonical bytes agrees with operator<=>.
    std::mt19937_64 rng(42);
    for (int i = 0; i < 2000; ++i) {
        // Small values too, so equal halves and equal values occur.
        const Fingerprint128 a{rng() % 4, rng() % 3 == 0 ? rng() : rng() % 4};
        const Fingerprint128 b{rng() % 4, rng() % 3 == 0 ? rng() : rng() % 4};
        const auto ab = a.canonicalBytes();
        const auto bb = b.canonicalBytes();
        const int cmp = std::memcmp(ab.data(), bb.data(), 16);
        const auto order = a <=> b;
        EXPECT_EQ(cmp < 0, order < 0);
        EXPECT_EQ(cmp == 0, order == 0);
        EXPECT_EQ(cmp > 0, order > 0);
    }
}

} // namespace
