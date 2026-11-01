// SPDX-License-Identifier: Apache-2.0
//
// LegacyDriver: decodeLegacy and encodeLegacy (text_codec.hpp; ADR 0018, D3, D4 and D17) over the
// table-driven fake codec, against legacyOracle; and LegacyPlatform: the same driver over the
// real platform codecs (Microsoft NLS on Windows, glibc iconv on Linux) with expectations from
// probe F1/F7 and, where CPython, Windows and glibc agree, from CPython.

#include "frost/io/utf.hpp"
#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/text_codec.hpp"

#include "text/support/encoding_checks.hpp"
#include "text/support/encoding_fixtures.hpp"
#include "text/support/encoding_oracle.hpp"
#include "text/support/fake_codec.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::sched::detail::SplitMix64;
using frost::text::decodeLegacy;
using frost::text::DecodeStats;
using frost::text::encodeLegacy;
using frost::text::InputEnd;
using frost::text::ITextCodec;
using frost::text::LeadByteRange;
using frost::text::test::FakeCodecProvider;
using frost::text::test::FakeCodePage;
using frost::text::test::fromHex;
using frost::text::test::fromU8;
using frost::text::test::reproduceSeed;
using frost::text::test::toHex;

constexpr std::string_view kExe = "text_encoding_tests";
const std::string kFffd(frost::text::kReplacementUtf8);

std::vector<FakeCodePage::Unit> asciiUnits() {
    std::vector<FakeCodePage::Unit> units;
    for (unsigned b = 0; b < 0x80; ++b) {
        units.push_back({std::string(1, static_cast<char>(b)), static_cast<char32_t>(b)});
    }
    return units;
}

std::string byteString(std::initializer_list<unsigned> bytes) {
    std::string s;
    for (const unsigned b : bytes) {
        s.push_back(static_cast<char>(static_cast<std::uint8_t>(b)));
    }
    return s;
}

// A small Shift_JIS-like page: lead bytes 81-82; 81 40 -> U+3000, 81 42 -> U+3002, 82 A0 ->
// U+3042, 81 80 -> U+00D7; 81 41 and 81 FF are undefined; A1 -> U+FF61; A5 undefined; C1 {80}
// undefined (glibc-like), and 82 is also listed although it is a lead byte.
std::shared_ptr<FakeCodePage> smallDbcs() {
    auto units = asciiUnits();
    units.push_back({byteString({0xA1}), 0xFF61});
    units.push_back({byteString({0x81, 0x40}), 0x3000});
    units.push_back({byteString({0x81, 0x42}), 0x3002});
    units.push_back({byteString({0x81, 0x80}), 0x00D7});
    units.push_back({byteString({0x82, 0xA0}), 0x3042});
    return std::make_shared<FakeCodePage>("fake-dbcs", std::vector<LeadByteRange>{{0x81, 0x82}},
                                          std::vector<std::uint8_t>{0x80, 0x82}, std::move(units));
}

// A single-byte page whose C1 bytes 81 and 8D are undefined (glibc-like) or defined as themselves
// (Windows-like); E9 -> U+00E9 and 80 -> U+20AC.
std::shared_ptr<FakeCodePage> smallSbcs(bool windowsLike) {
    auto units = asciiUnits();
    units.push_back({byteString({0x80}), 0x20AC});
    units.push_back({byteString({0xE9}), 0x00E9});
    if (windowsLike) {
        units.push_back({byteString({0x81}), 0x0081});
        units.push_back({byteString({0x8D}), 0x008D});
    }
    return std::make_shared<FakeCodePage>("fake-sbcs", std::vector<LeadByteRange>{},
                                          std::vector<std::uint8_t>{0x81, 0x8D}, std::move(units));
}

struct Opened {
    FakeCodecProvider provider;
    std::unique_ptr<ITextCodec> codec;
};

std::unique_ptr<Opened> open(std::shared_ptr<FakeCodePage> page) {
    auto o = std::make_unique<Opened>();
    o->provider.serve(std::move(page), 1252);
    auto codec = o->provider.open(1252);
    EXPECT_TRUE(codec.has_value());
    o->codec = std::move(*codec);
    return o;
}

struct Decoded {
    std::string text;
    DecodeStats stats;
    std::size_t consumed = 0;
};

Decoded decode(ITextCodec& codec, std::string_view bytes, InputEnd end = InputEnd::Final) {
    Decoded d;
    auto r = decodeLegacy(codec, bytes, d.text, d.stats, end);
    EXPECT_TRUE(r.has_value()) << r.error().describe();
    d.consumed = r.value_or(0);
    return d;
}

struct Encoded {
    std::string bytes;
    std::optional<std::uint64_t> stop;
};

Encoded encode(ITextCodec& codec, std::string_view utf8) {
    Encoded e;
    auto r = encodeLegacy(codec, utf8, e.bytes);
    EXPECT_TRUE(r.has_value()) << r.error().describe();
    e.stop = r.value_or(std::nullopt);
    return e;
}

// --- LegacyDriver (fake codec) --------------------------------------------------------------

TEST(LegacyDriver, DbcsLeadAsciiTrailReadsTrailAgain) {
    auto o = open(smallDbcs());
    const Decoded d = decode(*o->codec, "\x81"
                                        "A\x81\n");
    EXPECT_EQ(d.text, kFffd + "A" + kFffd + "\n") << "the ASCII trail and the LF are read again (WHATWG)";
    EXPECT_EQ(d.stats.replacements, 2u);
    EXPECT_EQ(d.stats.firstInvalidInput, 0u);
}

TEST(LegacyDriver, DbcsLeadHighInvalidTrailConsumesTwo) {
    auto o = open(smallDbcs());
    const Decoded d = decode(*o->codec, byteString({0x81, 0xFF, 0x42, 0x81, 0x40}));
    EXPECT_EQ(d.text, kFffd + "B" + fromU8(u8"\u3000"));
    EXPECT_EQ(d.stats.replacements, 1u);
}

TEST(LegacyDriver, DbcsLeadAtEnd) {
    auto o = open(smallDbcs());
    const Decoded final = decode(*o->codec, "A\x81");
    EXPECT_EQ(final.text, "A" + kFffd);
    EXPECT_EQ(final.consumed, 2u);
    const Decoded more = decode(*o->codec, "A\x81", InputEnd::More);
    EXPECT_EQ(more.text, "A");
    EXPECT_EQ(more.consumed, 1u);
    EXPECT_EQ(more.stats.inputBytes, 1u);
    EXPECT_EQ(more.stats.replacements, 0u);
}

TEST(LegacyDriver, SbcsUndefinedByte) {
    auto o = open(smallSbcs(false));
    const Decoded d = decode(*o->codec, "a\xA5"
                                        "b\xE9");
    EXPECT_EQ(d.text, "a" + kFffd + "b" + fromU8(u8"\u00e9"));
    EXPECT_EQ(d.stats.firstInvalidInput, 1u);
    EXPECT_EQ(d.stats.firstReplacementOutput, 1u);
}

TEST(LegacyDriver, WindowsOnlyC1Decode) {
    for (const bool windowsLike : {false, true}) {
        auto o = open(smallSbcs(windowsLike));
        const Decoded d = decode(*o->codec, "a\x81"
                                            "b\x8D");
        EXPECT_EQ(d.text, fromU8(u8"a\u0081b\u008d")) << windowsLike;
        EXPECT_EQ(d.stats.replacements, 0u);
    }
    // A lead byte in the list is still a lead byte: 82 alone is invalid, 80 maps to U+0080.
    auto o = open(smallDbcs());
    const Decoded d = decode(*o->codec, byteString({0x80, 0x82, 0x41}));
    EXPECT_EQ(d.text, fromU8(u8"\u0080") + kFffd + "A");
}

TEST(LegacyDriver, WindowsOnlyC1Encode) {
    for (const bool windowsLike : {false, true}) {
        auto o = open(smallSbcs(windowsLike));
        const Encoded e = encode(*o->codec, fromU8(u8"a\u0081b\u008d\u20ac"));
        EXPECT_EQ(toHex(e.bytes), "61 81 62 8d 80") << windowsLike;
        EXPECT_EQ(e.stop, std::nullopt);
        const Encoded stop = encode(*o->codec, fromU8(u8"a\u0082"));
        EXPECT_EQ(stop.stop, 1u) << "U+0082 is not in the list";
        EXPECT_EQ(stop.bytes, "a");
    }
}

TEST(LegacyDriver, OneWayMappingRejected) {
    auto page = smallSbcs(false);
    page->addOneWay(0x00A5, "\x5C"); // Like glibc's 932 U+00A5 -> 5C, which decodes to U+005C.
    page->addOneWay(0xF8F0, "\xA5"); // Like Windows' 932 U+F8F0 -> A0, which it refuses to decode.
    auto o = open(page);
    const Encoded yen = encode(*o->codec, fromU8(u8"ab\u00a5c"));
    EXPECT_EQ(yen.stop, 2u);
    EXPECT_EQ(yen.bytes, "ab");
    const Encoded pua = encode(*o->codec, fromU8(u8"\u00e9\uf8f0"));
    EXPECT_EQ(pua.stop, 2u);
    EXPECT_EQ(toHex(pua.bytes), "e9");
}

TEST(LegacyDriver, UnencodableOffsetIsCharStart) {
    auto o = open(smallDbcs());
    // Mixed 1-, 2- and 3-byte characters, then U+1F600 in the middle of the third 4 KiB chunk.
    std::string utf8;
    const std::string pieces[] = {"a", fromU8(u8"\u3042"), fromU8(u8"\u00d7"), "\n"};
    for (std::size_t i = 0; utf8.size() < 2 * 4096 + 1500; ++i) {
        utf8 += pieces[i % std::size(pieces)];
    }
    const std::size_t at = utf8.size();
    utf8 += fromU8(u8"\U0001F600") + "tail";
    const Encoded e = encode(*o->codec, utf8);
    ASSERT_EQ(e.stop, at);
    const auto want = frost::text::test::oracleEncodeLegacy(*smallDbcs(), utf8);
    EXPECT_EQ(e.stop, want.stop);
    EXPECT_EQ(e.bytes, want.bytes);
}

TEST(LegacyDriver, ChunkBoundaryNeverSplitsCharacters) {
    auto o = open(smallDbcs());
    // 3-byte characters, so 4096-byte chunk ends fall inside characters.
    for (const std::size_t lead : {0u, 1u, 2u}) {
        std::string utf8(lead, 'x');
        while (utf8.size() < 3 * 4096 + 7) {
            utf8 += fromU8(u8"\u3042\u3000");
        }
        const Encoded e = encode(*o->codec, utf8);
        EXPECT_EQ(e.stop, std::nullopt);
        const auto want = frost::text::test::oracleEncodeLegacy(*smallDbcs(), utf8);
        EXPECT_EQ(e.bytes, want.bytes) << lead;
        const Decoded back = decode(*o->codec, e.bytes);
        EXPECT_EQ(back.text, utf8);
    }
}

TEST(LegacyDriver, CallCountsLinear) {
    {
        auto o = open(smallSbcs(false));
        std::string bytes;
        while (bytes.size() < 64 * 1024) {
            bytes += "abcdefghijklmno\xA5"; // An undefined byte every 16 bytes.
        }
        o->provider.resetCounts();
        const Decoded d = decode(*o->codec, bytes);
        const std::uint64_t invalid = bytes.size() / 16;
        EXPECT_EQ(d.stats.replacements, invalid);
        EXPECT_LE(o->provider.decodeCalls(), 2 * invalid + 1);
    }
    {
        auto o = open(smallSbcs(false)); // Rejects U+0081 itself, like glibc.
        std::string utf8;
        while (utf8.size() < 64 * 1024) {
            utf8 += "abcdefghijklmn" + fromU8(u8"\u0081"); // 16 bytes.
        }
        o->provider.resetCounts();
        const Encoded e = encode(*o->codec, utf8);
        EXPECT_EQ(e.stop, std::nullopt);
        const std::uint64_t stops = utf8.size() / 16;
        const std::uint64_t chunks = (utf8.size() + 4095) / 4096;
        EXPECT_LE(o->provider.encodeCalls(), 2 * (chunks + stops));
        EXPECT_LE(o->provider.decodeCalls(), 2 * (chunks + stops));
    }
}

TEST(LegacyDriver, CodecDecodeErrorPropagates) {
    auto o = open(smallSbcs(false));
    o->provider.failDecodeAfter(0, ErrorCode::Busy);
    std::string text;
    DecodeStats stats;
    auto r = decodeLegacy(*o->codec, "abc", text, stats);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::Busy);
    ASSERT_FALSE(r.error().context().empty());
    EXPECT_EQ(r.error().context().back(), "while decoding fake-sbcs");
}

TEST(LegacyDriver, CodecEncodeErrorPropagates) {
    auto o = open(smallSbcs(false));
    o->provider.failEncodeAfter(0, ErrorCode::PermissionDenied);
    std::string bytes;
    auto r = encodeLegacy(*o->codec, "abc", bytes);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(r.error().context().back(), "while encoding fake-sbcs");
    // A decode failure inside the reversible check propagates too.
    auto o2 = open(smallSbcs(false));
    o2->provider.failDecodeAfter(0, ErrorCode::Timeout);
    std::string bytes2;
    auto r2 = encodeLegacy(*o2->codec, "abc", bytes2);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code(), ErrorCode::Timeout);
}

TEST(LegacyDriver, OverconsumptionIsInternal) {
    auto o = open(smallSbcs(false));
    o->provider.setOverconsume(true);
    std::string text;
    DecodeStats stats;
    auto r = decodeLegacy(*o->codec, "abc", text, stats);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::Internal);
}

TEST(LegacyDriver, IllFormedCodecOutputIsInternal) {
    auto o = open(smallSbcs(false));
    o->provider.setIllFormedOutput(true);
    std::string text;
    DecodeStats stats;
    auto r = decodeLegacy(*o->codec, "abc", text, stats);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::Internal);
}

// Random bytes over a page: its units, lead bytes, C1 bytes and junk.
std::string randomPageBytes(SplitMix64& rng, const FakeCodePage& page, std::size_t maxBytes) {
    std::string out;
    const std::size_t n = static_cast<std::size_t>(rng.below(maxBytes + 1));
    const auto& units = page.units();
    while (out.size() < n) {
        const std::uint64_t pick = rng.below(10);
        if (pick < 5) {
            out += units[rng.below(units.size())].bytes;
        } else if (pick < 7 && !page.info().leadBytes.empty()) {
            const auto& r = page.info().leadBytes[rng.below(page.info().leadBytes.size())];
            out.push_back(static_cast<char>(r.first + rng.below(r.last - r.first + 1u)));
        } else if (pick == 7 && !page.info().windowsOnlyC1.empty()) {
            out.push_back(static_cast<char>(page.info().windowsOnlyC1[rng.below(page.info().windowsOnlyC1.size())]));
        } else if (pick == 8) {
            out.push_back(static_cast<char>(0x20 + rng.below(0x60)));
        } else {
            out.push_back(static_cast<char>(rng.below(256)));
        }
    }
    return out;
}

TEST(LegacyDriver, RandomTablesMatchOracle) {
    const auto seeds = frost::text::test::encodingSeeds(100);
    for (std::uint64_t seed = seeds.first; seed < seeds.last; ++seed) {
        SplitMix64 rng(seed * 7919 + 3);
        auto page = std::make_shared<FakeCodePage>(seed % 2 == 0 ? frost::text::test::randomSbcs(rng)
                                                                 : frost::text::test::randomDbcs(rng));
        auto o = open(page);
        const std::string repro = reproduceSeed(kExe, "LegacyDriver.RandomTablesMatchOracle", seed);
        for (int input = 0; input < 32; ++input) {
            const std::string bytes = randomPageBytes(rng, *page, 96);
            const Decoded got = decode(*o->codec, bytes);
            const auto want = frost::text::test::legacyOracle(*page, bytes);
            ASSERT_EQ(toHex(got.text), toHex(want.utf8)) << toHex(bytes) << "; " << repro;
            ASSERT_EQ(got.stats.replacements, want.replacements) << repro;
            ASSERT_EQ(got.stats.firstInvalidInput, want.firstInvalid) << repro;
            ASSERT_EQ(got.stats.firstReplacementOutput, want.firstReplacementOutput) << repro;
            ASSERT_TRUE(frost::io::isValidUtf8(got.text));

            // Split invariance with More.
            Decoded streamed;
            std::string pending;
            const std::size_t cut = bytes.empty() ? 0 : static_cast<std::size_t>(rng.below(bytes.size() + 1));
            for (const std::string_view piece :
                 {std::string_view(bytes).substr(0, cut), std::string_view(bytes).substr(cut)}) {
                pending.append(piece);
                auto r = decodeLegacy(*o->codec, pending, streamed.text, streamed.stats, InputEnd::More);
                ASSERT_TRUE(r.has_value());
                pending.erase(0, *r);
                ASSERT_LE(pending.size(), 1u);
            }
            auto last = decodeLegacy(*o->codec, pending, streamed.text, streamed.stats, InputEnd::Final);
            ASSERT_TRUE(last.has_value());
            ASSERT_EQ(streamed.text, got.text) << toHex(bytes) << "; " << repro;
            ASSERT_EQ(streamed.stats, got.stats) << repro;

            // Encoding the decoded text is the reversible filter.
            const Encoded e = encode(*o->codec, got.text);
            const auto wantEncode = frost::text::test::oracleEncodeLegacy(*page, got.text);
            ASSERT_EQ(e.stop, wantEncode.stop) << toHex(bytes) << "; " << repro;
            ASSERT_EQ(toHex(e.bytes), toHex(wantEncode.bytes)) << repro;
        }
    }
}

// --- LegacyPlatform (real codecs) ------------------------------------------------------------

std::unique_ptr<ITextCodec> platform(std::uint16_t codePage) {
    auto codec = frost::text::platformCodecs().open(codePage);
    EXPECT_TRUE(codec.has_value()) << codePage << ": " << codec.error().describe();
    return codec ? std::move(*codec) : nullptr;
}

void expectSample(std::string_view fixtureName, std::uint16_t codePage) {
    const auto& f = frost::text::test::fixture(fixtureName);
    auto codec = platform(codePage);
    ASSERT_NE(codec, nullptr);
    const Decoded d = decode(*codec, f.bytes);
    EXPECT_EQ(d.text, f.text);
    EXPECT_EQ(d.stats.replacements, 0u);
    const Encoded e = encode(*codec, f.text);
    EXPECT_EQ(e.stop, std::nullopt);
    EXPECT_EQ(toHex(e.bytes), toHex(f.bytes));
}

TEST(LegacyPlatform, Cp1252Sample) { expectSample("cp1252", 1252); }
TEST(LegacyPlatform, Cp932Sample) { expectSample("cp932", 932); }

TEST(LegacyPlatform, Cp932HalfwidthKatakana) {
    auto codec = platform(932);
    ASSERT_NE(codec, nullptr);
    EXPECT_EQ(decode(*codec, fromHex("b6 c0")).text, fromU8(u8"\uff76\uff80"));
    EXPECT_EQ(toHex(encode(*codec, fromU8(u8"\uff76\uff80")).bytes), "b6 c0");
}

TEST(LegacyPlatform, Cp932NecIbmDuplicate) {
    auto codec = platform(932);
    ASSERT_NE(codec, nullptr);
    EXPECT_EQ(decode(*codec, fromHex("87 90")).text, fromU8(u8"\u2252"));
    EXPECT_EQ(decode(*codec, fromHex("81 e0")).text, fromU8(u8"\u2252"));
    EXPECT_EQ(toHex(encode(*codec, fromU8(u8"\u2252")).bytes), "81 e0");
}

TEST(LegacyPlatform, Cp932EudcPua) {
    auto codec = platform(932);
    ASSERT_NE(codec, nullptr);
    EXPECT_EQ(decode(*codec, fromHex("f0 40")).text, fromU8(u8"\ue000"));
    EXPECT_EQ(toHex(encode(*codec, fromU8(u8"\ue000")).bytes), "f0 40");
}

TEST(LegacyPlatform, Cp932YenAndTildeUnencodable) {
    auto codec = platform(932);
    ASSERT_NE(codec, nullptr);
    // One-way on one OS each (F2): glibc maps U+00A5/U+203E/U+2014 to bytes that decode to other
    // characters, Windows maps U+F8F0 to A0, which it refuses to decode.
    for (const std::u8string_view c : {u8"\u00a5", u8"\u203e", u8"\u2014", u8"\uf8f0"}) {
        const Encoded e = encode(*codec, "x" + fromU8(c));
        EXPECT_EQ(e.stop, 1u) << toHex(fromU8(c));
        EXPECT_EQ(e.bytes, "x");
    }
}

TEST(LegacyPlatform, Cp932InvalidSequencesSameEverywhere) {
    auto codec = platform(932);
    ASSERT_NE(codec, nullptr);
    struct Case {
        std::string_view hex;
        std::u8string_view text;
        std::uint64_t replacements;
    };
    const Case cases[] = {
        {"85 41", u8"\ufffdA", 1}, {"81 7f", u8"\ufffd\u007f", 1},     {"81 0a", u8"\ufffd\n", 1},
        {"81 ff", u8"\ufffd", 1},  {"85 80", u8"\ufffd", 1},           {"a0", u8"\ufffd", 1},
        {"fd", u8"\ufffd", 1},     {"41 81", u8"A\ufffd", 1},          {"80", u8"\u0080", 0},
        {"81 41", u8"\u3001", 0},  {"fe ff 41", u8"\ufffd\ufffdA", 2},
    };
    for (const Case& c : cases) {
        const Decoded d = decode(*codec, fromHex(c.hex));
        EXPECT_EQ(toHex(d.text), toHex(fromU8(c.text))) << c.hex;
        EXPECT_EQ(d.stats.replacements, c.replacements) << c.hex;
        EXPECT_EQ(d.stats.firstInvalidInput.has_value(), c.replacements != 0) << c.hex;
    }
    // 80 encodes back to 80 (D4), on both OSes.
    EXPECT_EQ(toHex(encode(*codec, fromU8(u8"\u0080")).bytes), "80");
}

TEST(LegacyPlatform, Cp1252C1Bytes) {
    auto codec = platform(1252);
    ASSERT_NE(codec, nullptr);
    const std::string bytes = fromHex("81 8d 8f 90 9d");
    const Decoded d = decode(*codec, bytes);
    EXPECT_EQ(d.text, fromU8(u8"\u0081\u008d\u008f\u0090\u009d"));
    EXPECT_EQ(d.stats.replacements, 0u);
    const Encoded e = encode(*codec, d.text);
    EXPECT_EQ(e.stop, std::nullopt);
    EXPECT_EQ(toHex(e.bytes), toHex(bytes));
    // A C1 control that 1252 maps elsewhere (80 is the euro sign) is unencodable.
    EXPECT_EQ(encode(*codec, fromU8(u8"\u0080")).stop, 0u);
}

} // namespace
