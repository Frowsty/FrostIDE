// SPDX-License-Identifier: Apache-2.0
//
// Utf8Lossy, Utf16Codec and Utf32Codec: the lossy Unicode decoders and exact encoders
// (unicode_codec.hpp; ADR 0018, D5 and D16) against the Unicode standard's own examples and the
// independent oracles of encoding_oracle.hpp, with streaming (InputEnd::More) split invariance.
/*
 * The Utf8Lossy goldens are transcribed from the Unicode 16.0 core specification, chapter 3,
 * section 3.9 ("U+FFFD Substitution of Maximal Subparts", tables 3-8 to 3-11 and the section's
 * own example). Property tests take FROST_ENCODING_SEED=<s> to run one seed and print the command
 * that reproduces a failure.
 */

#include "frost/io/utf.hpp"
#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/unicode_codec.hpp"

#include "text/support/encoding_checks.hpp"
#include "text/support/encoding_oracle.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using frost::sched::detail::SplitMix64;
using frost::text::appendUtf16;
using frost::text::appendUtf16Lossy;
using frost::text::appendUtf32;
using frost::text::appendUtf32Lossy;
using frost::text::appendUtf8Lossy;
using frost::text::DecodeStats;
using frost::text::Endian;
using frost::text::InputEnd;
using frost::text::test::encodingSeeds;
using frost::text::test::fromHex;
using frost::text::test::fromU8;
using frost::text::test::OracleDecode;
using frost::text::test::reproduceSeed;
using frost::text::test::toHex;

constexpr std::string_view kExe = "text_encoding_tests";

// One lossy decoder: bytes, the stream's stats, the end flag; returns the bytes consumed.
using Decoder = std::function<std::size_t(std::string_view, std::string&, DecodeStats&, InputEnd)>;

Decoder utf8Decoder() {
    return
        [](std::string_view b, std::string& out, DecodeStats& s, InputEnd e) { return appendUtf8Lossy(b, out, s, e); };
}
Decoder utf16Decoder(Endian order) {
    return [order](std::string_view b, std::string& out, DecodeStats& s, InputEnd e) {
        return appendUtf16Lossy(b, order, out, s, e);
    };
}
Decoder utf32Decoder(Endian order) {
    return [order](std::string_view b, std::string& out, DecodeStats& s, InputEnd e) {
        return appendUtf32Lossy(b, order, out, s, e);
    };
}

struct Decoded {
    std::string text;
    DecodeStats stats;
};

Decoded decodeWhole(const Decoder& decode, std::string_view bytes) {
    Decoded d;
    const std::size_t consumed = decode(bytes, d.text, d.stats, InputEnd::Final);
    EXPECT_EQ(consumed, bytes.size());
    return d;
}

// Decodes `bytes` as a stream cut at `cuts` (sorted offsets): More for every piece, then Final
// for the unconsumed rest. `maxPending` bounds what More may leave unconsumed.
Decoded decodeStream(const Decoder& decode, std::string_view bytes, std::vector<std::size_t> cuts,
                     std::size_t maxPending) {
    Decoded d;
    std::string pending;
    std::size_t at = 0;
    cuts.push_back(bytes.size());
    for (const std::size_t cut : cuts) {
        pending.append(bytes.substr(at, cut - at));
        at = cut;
        const std::size_t consumed = decode(pending, d.text, d.stats, InputEnd::More);
        EXPECT_LE(consumed, pending.size());
        pending.erase(0, consumed);
        EXPECT_LE(pending.size(), maxPending) << "More left too much unconsumed";
    }
    const std::size_t consumed = decode(pending, d.text, d.stats, InputEnd::Final);
    EXPECT_EQ(consumed, pending.size());
    return d;
}

void expectMatchesOracle(const Decoded& got, const OracleDecode& want, std::string_view bytes) {
    EXPECT_EQ(toHex(got.text), toHex(want.utf8)) << "input " << toHex(bytes);
    EXPECT_EQ(got.stats.replacements, want.replacements) << "input " << toHex(bytes);
    EXPECT_EQ(got.stats.firstInvalidInput, want.firstInvalid) << "input " << toHex(bytes);
    EXPECT_EQ(got.stats.firstReplacementOutput, want.firstReplacementOutput) << "input " << toHex(bytes);
    EXPECT_EQ(got.stats.inputBytes, bytes.size());
    EXPECT_EQ(got.stats.outputBytes, got.text.size());
}

std::string repeatFffd(int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        out.append(frost::text::kReplacementUtf8);
    }
    return out;
}

void expectUtf8Golden(std::string_view hex, std::string_view expected, std::uint64_t replacements) {
    const std::string bytes = fromHex(hex);
    const Decoded d = decodeWhole(utf8Decoder(), bytes);
    EXPECT_EQ(toHex(d.text), toHex(expected));
    EXPECT_EQ(d.stats.replacements, replacements);
    EXPECT_TRUE(frost::io::isValidUtf8(d.text));
    expectMatchesOracle(d, frost::text::test::oracleUtf8Lossy(bytes), bytes);
}

std::vector<std::size_t> randomCuts(SplitMix64& rng, std::size_t size) {
    std::vector<std::size_t> cuts;
    const std::uint64_t count = rng.below(6);
    for (std::uint64_t k = 0; k < count; ++k) {
        cuts.push_back(static_cast<std::size_t>(rng.below(size + 1)));
    }
    std::ranges::sort(cuts);
    return cuts;
}

// Bytes biased toward the values UTF-8 decoders get wrong, with spliced valid sequences.
std::string biasedUtf8Bytes(SplitMix64& rng, std::size_t maxBytes) {
    constexpr unsigned char kBiased[] = {0x00, 0x0A, 0x0D, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0,
                                         0xC1, 0xC2, 0xDF, 0xE0, 0xED, 0xEE, 0xEF, 0xF0, 0xF4, 0xF5, 0xFF};
    std::string out;
    const std::size_t n = static_cast<std::size_t>(rng.below(maxBytes + 1));
    while (out.size() < n) {
        const std::uint64_t pick = rng.below(10);
        if (pick < 5) {
            out.push_back(static_cast<char>(kBiased[rng.below(std::size(kBiased))]));
        } else if (pick < 7) {
            out.push_back(static_cast<char>(rng.below(256)));
        } else {
            const std::uint64_t kind = rng.below(4);
            const char32_t c = kind == 0   ? static_cast<char32_t>(0x80 + rng.below(0x780))
                               : kind == 1 ? static_cast<char32_t>(0x800 + rng.below(0xD000))
                               : kind == 2 ? static_cast<char32_t>(0xE000 + rng.below(0x2000))
                                           : static_cast<char32_t>(0x10000 + rng.below(0x100000));
            frost::text::test::oracleAppendUtf8(c, out);
        }
    }
    out.resize(std::min(out.size(), n));
    return out;
}

// Random UTF-16/UTF-32 code units, biased toward surrogates and range limits.
std::string randomUnits(SplitMix64& rng, std::size_t width, Endian order, std::size_t maxUnits) {
    std::string out;
    const std::size_t units = static_cast<std::size_t>(rng.below(maxUnits + 1));
    for (std::size_t u = 0; u < units; ++u) {
        std::uint32_t v = 0;
        const std::uint64_t pick = rng.below(8);
        if (pick == 0) {
            v = static_cast<std::uint32_t>(0xD800 + rng.below(0x400)); // High surrogate.
        } else if (pick == 1) {
            v = static_cast<std::uint32_t>(0xDC00 + rng.below(0x400)); // Low surrogate.
        } else if (pick == 2 && width == 4) {
            constexpr std::uint32_t kEdges[] = {0x10FFFF, 0x110000, 0xFFFFFFFF, 0x7FFFFFFF, 0xFFFF, 0xFFFE, 0, 0xFEFF};
            v = kEdges[rng.below(std::size(kEdges))];
        } else if (pick == 3 && width == 4) {
            v = static_cast<std::uint32_t>(0x10000 + rng.below(0x100000));
        } else {
            v = static_cast<std::uint32_t>(rng.below(0x10000));
        }
        for (std::size_t k = 0; k < width; ++k) {
            const std::size_t shift = order == Endian::Big ? 8 * (width - 1 - k) : 8 * k;
            out.push_back(static_cast<char>(static_cast<std::uint8_t>((v >> shift) & 0xFF)));
        }
    }
    if (rng.below(4) == 0) {
        const std::uint64_t extra = 1 + rng.below(width - 1);
        for (std::uint64_t k = 0; k < extra; ++k) {
            out.push_back(static_cast<char>(rng.below(256)));
        }
    }
    return out;
}

// --- Utf8Lossy -----------------------------------------------------------------------------

TEST(Utf8Lossy, Table3_8_NonShortestForm) { expectUtf8Golden("C0 AF E0 80 BF F0 81 82 41", repeatFffd(8) + "A", 8); }

TEST(Utf8Lossy, Table3_9_Surrogates) { expectUtf8Golden("ED A0 80 ED BF BF ED AF 41", repeatFffd(8) + "A", 8); }

TEST(Utf8Lossy, Table3_10_OtherIllFormed) {
    expectUtf8Golden("F4 91 92 93 FF 41 80 BF 42", repeatFffd(5) + "A" + repeatFffd(2) + "B", 7);
}

TEST(Utf8Lossy, Table3_11_Truncated) { expectUtf8Golden("E1 80 E2 F0 91 92 F1 BF 41", repeatFffd(4) + "A", 4); }

TEST(Utf8Lossy, Section3_9Example) {
    expectUtf8Golden("61 F1 80 80 E1 80 C2 62 80 63 80 BF 64",
                     "a" + repeatFffd(3) + "b" + repeatFffd(1) + "c" + repeatFffd(2) + "d", 6);
}

TEST(Utf8Lossy, ValidIsIdentity) {
    const std::string valid = fromU8(u8"plain \u00e9\u20ac\U0001F600 \ufffd") + std::string("\0x", 2);
    const Decoded d = decodeWhole(utf8Decoder(), valid);
    EXPECT_EQ(d.text, valid);
    EXPECT_EQ(d.stats.replacements, 0u) << "a valid U+FFFD and U+0000 are not counted";
    EXPECT_EQ(d.stats.firstInvalidInput, std::nullopt);
    EXPECT_EQ(d.stats.firstReplacementOutput, std::nullopt);
    EXPECT_EQ(d.stats.inputBytes, valid.size());
    EXPECT_EQ(d.stats.outputBytes, valid.size());
}

TEST(Utf8Lossy, StatsAccumulateAcrossCalls) {
    DecodeStats stats;
    std::string out = "prefix"; // What `out` held before does not move the offsets.
    EXPECT_EQ(appendUtf8Lossy("abc", out, stats), 3u);
    EXPECT_EQ(appendUtf8Lossy(fromU8(u8"\u00e9"), out, stats), 2u);
    EXPECT_EQ(appendUtf8Lossy("x\xFFy\xFE", out, stats), 4u);
    EXPECT_EQ(stats.inputBytes, 9u);
    EXPECT_EQ(stats.replacements, 2u);
    EXPECT_EQ(stats.firstInvalidInput, 6u);      // "abc" + "é" + "x" before the FF.
    EXPECT_EQ(stats.firstReplacementOutput, 6u); // Of the concatenated output, without "prefix".
    EXPECT_EQ(stats.outputBytes, out.size() - 6);
    EXPECT_EQ(out, "prefixabc" + fromU8(u8"\u00e9") + "x" + repeatFffd(1) + "y" + repeatFffd(1));
}

TEST(Utf8Lossy, TruncatedAtEnd) {
    // One proper prefix of each lead class: 2-byte, E0, ED, other 3-byte, F0, F4, other 4-byte.
    for (const std::string_view hex :
         {"C3", "E0 A0", "ED 9F", "E1", "E1 80", "F0 90", "F0 90 80", "F4 8F BF", "F1", "F3 80 80"}) {
        const std::string tail = fromHex(hex);
        const std::string bytes = "ab" + tail;
        std::string out;
        DecodeStats final;
        EXPECT_EQ(appendUtf8Lossy(bytes, out, final, InputEnd::Final), bytes.size()) << hex;
        EXPECT_EQ(out, "ab" + repeatFffd(1)) << hex << ": a maximal subpart is one U+FFFD";
        EXPECT_EQ(final.firstInvalidInput, 2u);

        out.clear();
        DecodeStats more;
        EXPECT_EQ(appendUtf8Lossy(bytes, out, more, InputEnd::More), 2u) << hex;
        EXPECT_EQ(out, "ab");
        EXPECT_EQ(more.replacements, 0u);
        EXPECT_EQ(more.inputBytes, 2u);
    }
    // A prefix that can no longer become valid is replaced even with More.
    std::string out;
    DecodeStats more;
    EXPECT_EQ(appendUtf8Lossy("a\xE0\x80", out, more, InputEnd::More), 3u);
    EXPECT_EQ(out, "a" + repeatFffd(2));
    out.clear();
    DecodeStats more2;
    EXPECT_EQ(appendUtf8Lossy("a\xC0", out, more2, InputEnd::More), 2u);
    EXPECT_EQ(out, "a" + repeatFffd(1));
}

TEST(Utf8Lossy, SplitInvariance) {
    const auto seeds = encodingSeeds(200);
    for (std::uint64_t seed = seeds.first; seed < seeds.last; ++seed) {
        SplitMix64 rng(seed * 0x9E37'79B9'7F4A'7C15ULL + 1);
        for (int input = 0; input < 16; ++input) {
            const std::string bytes = biasedUtf8Bytes(rng, 128);
            const Decoded whole = decodeWhole(utf8Decoder(), bytes);
            const Decoded streamed = decodeStream(utf8Decoder(), bytes, randomCuts(rng, bytes.size()), 3);
            ASSERT_EQ(toHex(streamed.text), toHex(whole.text))
                << "input " << toHex(bytes) << "; " << reproduceSeed(kExe, "Utf8Lossy.SplitInvariance", seed);
            ASSERT_EQ(streamed.stats, whole.stats) << reproduceSeed(kExe, "Utf8Lossy.SplitInvariance", seed);
        }
    }
}

TEST(Utf8Lossy, MatchesDefinitionOracle) {
    const auto seeds = encodingSeeds(200);
    for (std::uint64_t seed = seeds.first; seed < seeds.last; ++seed) {
        SplitMix64 rng(seed + 0x5EED);
        for (int input = 0; input < 64; ++input) {
            const std::string bytes = biasedUtf8Bytes(rng, 256);
            const Decoded got = decodeWhole(utf8Decoder(), bytes);
            const OracleDecode want = frost::text::test::oracleUtf8Lossy(bytes);
            ASSERT_EQ(toHex(got.text), toHex(want.utf8))
                << "input " << toHex(bytes) << "; " << reproduceSeed(kExe, "Utf8Lossy.MatchesDefinitionOracle", seed);
            ASSERT_EQ(got.stats.replacements, want.replacements);
            ASSERT_EQ(got.stats.firstInvalidInput, want.firstInvalid);
            ASSERT_EQ(got.stats.firstReplacementOutput, want.firstReplacementOutput);
        }
    }
}

TEST(Utf8Lossy, OutputWellFormed) {
    SplitMix64 rng(42);
    for (int input = 0; input < 2000; ++input) {
        std::string bytes(static_cast<std::size_t>(rng.below(64)), '\0');
        for (char& c : bytes) {
            c = static_cast<char>(rng.below(256));
        }
        const Decoded d = decodeWhole(utf8Decoder(), bytes);
        ASSERT_TRUE(frost::io::isValidUtf8(d.text)) << toHex(bytes);
    }
}

// --- Utf16Codec ----------------------------------------------------------------------------

TEST(Utf16Codec, SurrogatePairsBothOrders) {
    const std::string text = fromU8(u8"G clef \U0001D11E, grin \U0001F600, \u00e4\r\nline 2\r\n");
    const std::string le =
        fromHex("47 00 20 00 63 00 6c 00 65 00 66 00 20 00 34 d8 1e dd 2c 00 20 00 67 00 72 00 69 00 "
                "6e 00 20 00 3d d8 00 de 2c 00 20 00 e4 00 0d 00 0a 00 6c 00 69 00 6e 00 65 00 20 00 "
                "32 00 0d 00 0a 00");
    std::string be;
    for (std::size_t i = 0; i + 1 < le.size(); i += 2) {
        be.push_back(le[i + 1]);
        be.push_back(le[i]);
    }
    for (const auto& [order, bytes] : {std::pair{Endian::Little, le}, std::pair{Endian::Big, be}}) {
        const Decoded d = decodeWhole(utf16Decoder(order), bytes);
        EXPECT_EQ(d.text, text);
        EXPECT_EQ(d.stats.replacements, 0u);
        std::string encoded;
        appendUtf16(text, order, encoded);
        EXPECT_EQ(toHex(encoded), toHex(bytes));
    }
}

TEST(Utf16Codec, LoneHigh) {
    const Decoded d = decodeWhole(utf16Decoder(Endian::Little), fromHex("41 00 00 d8 42 00"));
    EXPECT_EQ(d.text, "A" + repeatFffd(1) + "B");
    EXPECT_EQ(d.stats.firstInvalidInput, 2u);
    EXPECT_EQ(d.stats.firstReplacementOutput, 1u);
}

TEST(Utf16Codec, LoneLow) {
    const Decoded d = decodeWhole(utf16Decoder(Endian::Big), fromHex("00 41 dc 00 00 42"));
    EXPECT_EQ(d.text, "A" + repeatFffd(1) + "B");
    EXPECT_EQ(d.stats.replacements, 1u);
}

TEST(Utf16Codec, HighHigh) {
    // The first high surrogate is replaced; the second pairs with the low one that follows.
    const Decoded d = decodeWhole(utf16Decoder(Endian::Little), fromHex("3d d8 3d d8 00 de"));
    EXPECT_EQ(d.text, repeatFffd(1) + fromU8(u8"\U0001F600"));
    EXPECT_EQ(d.stats.replacements, 1u);
}

TEST(Utf16Codec, HighAtEnd) {
    const std::string bytes = fromHex("41 00 3d d8");
    const Decoded final = decodeWhole(utf16Decoder(Endian::Little), bytes);
    EXPECT_EQ(final.text, "A" + repeatFffd(1));
    std::string out;
    DecodeStats more;
    EXPECT_EQ(appendUtf16Lossy(bytes, Endian::Little, out, more, InputEnd::More), 2u);
    EXPECT_EQ(out, "A");
    EXPECT_EQ(more.replacements, 0u);
    // The pair completes with the next call.
    EXPECT_EQ(appendUtf16Lossy(fromHex("3d d8 00 de"), Endian::Little, out, more, InputEnd::Final), 4u);
    EXPECT_EQ(out, "A" + fromU8(u8"\U0001F600"));
}

TEST(Utf16Codec, OddTrailingByte) {
    const Decoded d = decodeWhole(utf16Decoder(Endian::Big), fromHex("00 41 00"));
    EXPECT_EQ(d.text, "A" + repeatFffd(1));
    EXPECT_EQ(d.stats.firstInvalidInput, 2u);
    std::string out;
    DecodeStats more;
    EXPECT_EQ(appendUtf16Lossy(fromHex("00 41 00"), Endian::Big, out, more, InputEnd::More), 2u);
    EXPECT_EQ(out, "A");
}

TEST(Utf16Codec, SplitInvariance) {
    const auto seeds = encodingSeeds(200);
    for (std::uint64_t seed = seeds.first; seed < seeds.last; ++seed) {
        SplitMix64 rng(seed + 16);
        for (const Endian order : {Endian::Little, Endian::Big}) {
            for (int input = 0; input < 8; ++input) {
                const std::string bytes = randomUnits(rng, 2, order, 48);
                const Decoded whole = decodeWhole(utf16Decoder(order), bytes);
                const Decoded streamed = decodeStream(utf16Decoder(order), bytes, randomCuts(rng, bytes.size()), 3);
                ASSERT_EQ(toHex(streamed.text), toHex(whole.text))
                    << toHex(bytes) << "; " << reproduceSeed(kExe, "Utf16Codec.SplitInvariance", seed);
                ASSERT_EQ(streamed.stats, whole.stats) << reproduceSeed(kExe, "Utf16Codec.SplitInvariance", seed);
            }
        }
    }
}

TEST(Utf16Codec, MatchesOracle) {
    const auto seeds = encodingSeeds(200);
    for (std::uint64_t seed = seeds.first; seed < seeds.last; ++seed) {
        SplitMix64 rng(seed + 1616);
        for (const Endian order : {Endian::Little, Endian::Big}) {
            for (int input = 0; input < 16; ++input) {
                const std::string bytes = randomUnits(rng, 2, order, 64);
                const Decoded got = decodeWhole(utf16Decoder(order), bytes);
                const OracleDecode want = frost::text::test::oracleUtf16Lossy(bytes, order == Endian::Big);
                ASSERT_EQ(toHex(got.text), toHex(want.utf8))
                    << toHex(bytes) << "; " << reproduceSeed(kExe, "Utf16Codec.MatchesOracle", seed);
                ASSERT_EQ(got.stats.replacements, want.replacements);
                ASSERT_EQ(got.stats.firstInvalidInput, want.firstInvalid);
                ASSERT_EQ(got.stats.firstReplacementOutput, want.firstReplacementOutput);
            }
        }
    }
}

// Random scalars including the edges, as UTF-8.
std::string randomScalars(SplitMix64& rng, std::size_t count) {
    constexpr char32_t kEdges[] = {0x0,    0x7F,   0x80,   0x7FF,   0x800,    0xD7FF, 0xE000,
                                   0xFFFD, 0xFFFE, 0xFFFF, 0x10000, 0x10FFFF, 0xFEFF};
    std::string utf8;
    for (std::size_t i = 0; i < count; ++i) {
        char32_t c = 0;
        if (rng.below(3) == 0) {
            c = kEdges[rng.below(std::size(kEdges))];
        } else {
            do {
                c = static_cast<char32_t>(rng.below(0x110000));
            } while (c >= 0xD800 && c <= 0xDFFF);
        }
        frost::text::test::oracleAppendUtf8(c, utf8);
    }
    return utf8;
}

TEST(Utf16Codec, EncodeThenDecodeIdentity) {
    const auto seeds = encodingSeeds(200);
    for (std::uint64_t seed = seeds.first; seed < seeds.last; ++seed) {
        SplitMix64 rng(seed + 77);
        const std::string utf8 = randomScalars(rng, 40);
        for (const Endian order : {Endian::Little, Endian::Big}) {
            std::string encoded;
            appendUtf16(utf8, order, encoded);
            ASSERT_EQ(toHex(encoded), toHex(frost::text::test::oracleEncodeUtf16(utf8, order == Endian::Big)))
                << reproduceSeed(kExe, "Utf16Codec.EncodeThenDecodeIdentity", seed);
            const Decoded back = decodeWhole(utf16Decoder(order), encoded);
            ASSERT_EQ(back.text, utf8);
            ASSERT_EQ(back.stats.replacements, 0u);
        }
    }
}

// --- Utf32Codec ----------------------------------------------------------------------------

TEST(Utf32Codec, SurrogatePairsBothOrders) {
    const std::string text = fromU8(u8"x \U0001F600\n");
    const std::string le = fromHex("78 00 00 00 20 00 00 00 00 f6 01 00 0a 00 00 00");
    const std::string be = fromHex("00 00 00 78 00 00 00 20 00 01 f6 00 00 00 00 0a");
    for (const auto& [order, bytes] : {std::pair{Endian::Little, le}, std::pair{Endian::Big, be}}) {
        EXPECT_EQ(decodeWhole(utf32Decoder(order), bytes).text, text);
        std::string encoded;
        appendUtf32(text, order, encoded);
        EXPECT_EQ(toHex(encoded), toHex(bytes));
    }
}

TEST(Utf32Codec, Utf32OutOfRangeAndSurrogateUnits) {
    const std::string bytes = fromHex("41 00 00 00 00 00 11 00 00 d8 00 00 ff ff 10 00 ff ff ff ff 42 00 00 00");
    const Decoded d = decodeWhole(utf32Decoder(Endian::Little), bytes);
    EXPECT_EQ(d.text, "A" + repeatFffd(2) + fromU8(u8"\U0010FFFF") + repeatFffd(1) + "B");
    EXPECT_EQ(d.stats.replacements, 3u);
    EXPECT_EQ(d.stats.firstInvalidInput, 4u);
    EXPECT_EQ(d.stats.firstReplacementOutput, 1u);
}

TEST(Utf32Codec, Utf32Trailing1To3) {
    for (std::size_t extra = 1; extra <= 3; ++extra) {
        const std::string bytes = fromHex("00 00 00 41") + std::string(extra, '\x00');
        const Decoded d = decodeWhole(utf32Decoder(Endian::Big), bytes);
        EXPECT_EQ(d.text, "A" + repeatFffd(1)) << extra;
        EXPECT_EQ(d.stats.firstInvalidInput, 4u);
        std::string out;
        DecodeStats more;
        EXPECT_EQ(appendUtf32Lossy(bytes, Endian::Big, out, more, InputEnd::More), 4u) << extra;
        EXPECT_EQ(out, "A");
    }
}

TEST(Utf32Codec, SplitInvariance) {
    const auto seeds = encodingSeeds(200);
    for (std::uint64_t seed = seeds.first; seed < seeds.last; ++seed) {
        SplitMix64 rng(seed + 32);
        for (const Endian order : {Endian::Little, Endian::Big}) {
            for (int input = 0; input < 8; ++input) {
                const std::string bytes = randomUnits(rng, 4, order, 24);
                const Decoded whole = decodeWhole(utf32Decoder(order), bytes);
                const Decoded streamed = decodeStream(utf32Decoder(order), bytes, randomCuts(rng, bytes.size()), 3);
                ASSERT_EQ(toHex(streamed.text), toHex(whole.text))
                    << toHex(bytes) << "; " << reproduceSeed(kExe, "Utf32Codec.SplitInvariance", seed);
                ASSERT_EQ(streamed.stats, whole.stats);
            }
        }
    }
}

TEST(Utf32Codec, MatchesOracle) {
    const auto seeds = encodingSeeds(200);
    for (std::uint64_t seed = seeds.first; seed < seeds.last; ++seed) {
        SplitMix64 rng(seed + 3232);
        for (const Endian order : {Endian::Little, Endian::Big}) {
            for (int input = 0; input < 16; ++input) {
                const std::string bytes = randomUnits(rng, 4, order, 32);
                const Decoded got = decodeWhole(utf32Decoder(order), bytes);
                const OracleDecode want = frost::text::test::oracleUtf32Lossy(bytes, order == Endian::Big);
                ASSERT_EQ(toHex(got.text), toHex(want.utf8))
                    << toHex(bytes) << "; " << reproduceSeed(kExe, "Utf32Codec.MatchesOracle", seed);
                ASSERT_EQ(got.stats.replacements, want.replacements);
                ASSERT_EQ(got.stats.firstInvalidInput, want.firstInvalid);
                ASSERT_EQ(got.stats.firstReplacementOutput, want.firstReplacementOutput);
            }
        }
    }
}

TEST(Utf32Codec, EncodeThenDecodeIdentity) {
    const auto seeds = encodingSeeds(200);
    for (std::uint64_t seed = seeds.first; seed < seeds.last; ++seed) {
        SplitMix64 rng(seed + 99);
        const std::string utf8 = randomScalars(rng, 40);
        for (const Endian order : {Endian::Little, Endian::Big}) {
            std::string encoded;
            appendUtf32(utf8, order, encoded);
            ASSERT_EQ(toHex(encoded), toHex(frost::text::test::oracleEncodeUtf32(utf8, order == Endian::Big)))
                << reproduceSeed(kExe, "Utf32Codec.EncodeThenDecodeIdentity", seed);
            const Decoded back = decodeWhole(utf32Decoder(order), encoded);
            ASSERT_EQ(back.text, utf8);
            ASSERT_EQ(back.stats.replacements, 0u);
        }
    }
}

} // namespace
