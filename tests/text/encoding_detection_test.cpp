// SPDX-License-Identifier: Apache-2.0
//
// EncodingDetection: the detection heuristic (encoding_detection.hpp; ADR 0018, D7 and D8) with
// its confidence constants, the BOM ambiguity, the NUL pattern, the fallback chains, unusable
// defaults, sample cuts and the B15 sample bound. Every test sets legacyCodePage explicitly.

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/encoding_detection.hpp"

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

using frost::sched::detail::SplitMix64;
using frost::text::detectEncoding;
using frost::text::DetectionSource;
using frost::text::DetectOptions;
using frost::text::Encoding;
using frost::text::EncodingDetection;
using frost::text::EncodingForm;
using frost::text::EncodingGuess;
using frost::text::test::fromHex;
using frost::text::test::fromU8;
using frost::text::test::toHex;

DetectOptions options(std::uint16_t legacy, Encoding def = Encoding::utf8(),
                      std::size_t sample = frost::text::kDetectSampleBytes) {
    DetectOptions o;
    o.legacyCodePage = legacy;
    o.defaultEncoding = def;
    o.sampleBytes = sample;
    return o;
}

EncodingDetection detect(std::string_view bytes, const DetectOptions& o) {
    return detectEncoding(bytes, o, frost::text::platformCodecs());
}

EncodingGuess guess(Encoding e, bool bom, std::uint8_t confidence, DetectionSource source) {
    return EncodingGuess{e, bom, confidence, source};
}

const Encoding kUtf8 = Encoding::utf8();
const Encoding kUtf16Le = Encoding::unicode(EncodingForm::Utf16Le);
const Encoding kUtf16Be = Encoding::unicode(EncodingForm::Utf16Be);
const Encoding kCp1252 = Encoding::legacy(1252);
const Encoding kCp932 = Encoding::legacy(932);

TEST(EncodingDetection, EachBomWins) {
    struct Case {
        std::string bytes;
        EncodingForm form;
    };
    const Case cases[] = {
        {fromHex("ef bb bf 41 c3"), EncodingForm::Utf8}, // Invalid UTF-8 after the BOM still wins.
        {fromHex("ff fe 41 00"), EncodingForm::Utf16Le},
        {fromHex("fe ff 00 41"), EncodingForm::Utf16Be},
        {fromHex("00 00 fe ff 00 00 00 41"), EncodingForm::Utf32Be},
    };
    for (const Case& c : cases) {
        const auto d = detect(c.bytes, options(1252));
        ASSERT_EQ(d.candidates.size(), 1u);
        EXPECT_EQ(d.candidates[0], guess(Encoding::unicode(c.form), true, 100, DetectionSource::Bom));
        EXPECT_FALSE(d.binaryLikely);
    }
}

TEST(EncodingDetection, Utf32LeBomAmbiguity) {
    const auto four = detect(fromHex("ff fe 00 00"), options(1252));
    ASSERT_EQ(four.candidates.size(), 2u);
    EXPECT_EQ(four.candidates[0], guess(Encoding::unicode(EncodingForm::Utf32Le), true, 100, DetectionSource::Bom));
    EXPECT_EQ(four.candidates[1], guess(kUtf16Le, true, 90, DetectionSource::Bom));
    const auto eight = detect(fromHex("ff fe 00 00 41 00 00 00"), options(1252));
    EXPECT_EQ(eight.candidates[0].encoding, Encoding::unicode(EncodingForm::Utf32Le));
    const auto six = detect(fromHex("ff fe 00 00 41 00"), options(1252));
    ASSERT_EQ(six.candidates.size(), 1u);
    EXPECT_EQ(six.candidates[0], guess(kUtf16Le, true, 100, DetectionSource::Bom));
}

// h units: zHi of them ASCII (high byte 0), the rest U+4E01 (both bytes non-zero).
std::string utf16Units(std::size_t h, std::size_t zHi, bool big) {
    std::string text;
    for (std::size_t u = 0; u < h; ++u) {
        text += u < zHi ? "a" : fromU8(u8"\u4e01");
    }
    return frost::text::test::oracleEncodeUtf16(text, big);
}

TEST(EncodingDetection, Utf16NulPatternLe) {
    // 60 + 35 * zHi / h, at most 95, for zHi >= ceil(0.4 h).
    for (const auto& [zHi, confidence] : {std::pair{20u, 95u}, std::pair{12u, 81u}, std::pair{8u, 74u}}) {
        const auto d = detect(utf16Units(20, zHi, false), options(1252));
        ASSERT_FALSE(d.candidates.empty());
        EXPECT_EQ(d.candidates[0],
                  guess(kUtf16Le, false, static_cast<std::uint8_t>(confidence), DetectionSource::NulPattern))
            << zHi;
        EXPECT_EQ(d.candidates.size(), 1u) << "no fallback chain for UTF-16";
        EXPECT_FALSE(d.binaryLikely);
    }
    // Below ceil(0.4 h) it is not UTF-16.
    const auto below = detect(utf16Units(20, 7, false), options(1252));
    EXPECT_NE(below.candidates[0].encoding, kUtf16Le);
}

TEST(EncodingDetection, Utf16NulPatternBe) {
    for (const auto& [zHi, confidence] : {std::pair{20u, 95u}, std::pair{12u, 81u}, std::pair{8u, 74u}}) {
        const auto d = detect(utf16Units(20, zHi, true), options(1252));
        ASSERT_FALSE(d.candidates.empty());
        EXPECT_EQ(d.candidates[0],
                  guess(kUtf16Be, false, static_cast<std::uint8_t>(confidence), DetectionSource::NulPattern))
            << zHi;
    }
}

TEST(EncodingDetection, TooShortForUtf16) {
    for (const std::string& bytes : {std::string("A\0", 2), std::string("A\0B", 3)}) {
        const auto d = detect(bytes, options(1252));
        EXPECT_EQ(d.candidates[0], guess(kUtf8, false, 100, DetectionSource::AsciiOnly));
        EXPECT_TRUE(d.binaryLikely);
    }
}

TEST(EncodingDetection, AllNulsIsBinaryUtf8) {
    const auto d = detect(std::string(64, '\0'), options(1252));
    EXPECT_EQ(d.candidates[0], guess(kUtf8, false, 100, DetectionSource::AsciiOnly));
    EXPECT_TRUE(d.binaryLikely);
}

TEST(EncodingDetection, ValidUtf8Confidence) {
    const std::string one = fromU8(u8"\u00e9");
    for (const auto& [m, confidence] :
         {std::pair{1u, 85u}, std::pair{2u, 90u}, std::pair{3u, 95u}, std::pair{10u, 95u}}) {
        std::string bytes = "int x = 0; ";
        for (unsigned k = 0; k < m; ++k) {
            bytes += one + " ";
        }
        const auto d = detect(bytes, options(932));
        // The fallback chain adds the legacy page, then windows-1252, each at 30; its UTF-8 entry
        // is a duplicate of the first candidate and is left out.
        ASSERT_EQ(d.candidates.size(), 3u) << m;
        EXPECT_EQ(d.candidates[0],
                  guess(kUtf8, false, static_cast<std::uint8_t>(confidence), DetectionSource::Utf8Valid));
        EXPECT_EQ(d.candidates[1], guess(kCp932, false, 30, DetectionSource::LegacyFallback));
        EXPECT_EQ(d.candidates[2], guess(kCp1252, false, 30, DetectionSource::LegacyFallback));
    }
    // m = 0: pure ASCII, the default encoding.
    const auto ascii = detect("int x = 0;", options(932));
    EXPECT_EQ(ascii.candidates[0], guess(kUtf8, false, 100, DetectionSource::AsciiOnly));
}

TEST(EncodingDetection, AsciiOnlyUsesDefault) {
    const auto utf8 = detect("plain", options(1252));
    ASSERT_EQ(utf8.candidates.size(), 2u);
    EXPECT_EQ(utf8.candidates[0], guess(kUtf8, false, 100, DetectionSource::AsciiOnly));
    EXPECT_EQ(utf8.candidates[1], guess(kCp1252, false, 30, DetectionSource::LegacyFallback));
    const auto def1252 = detect("plain", options(932, kCp1252));
    ASSERT_EQ(def1252.candidates.size(), 3u);
    EXPECT_EQ(def1252.candidates[0], guess(kCp1252, false, 100, DetectionSource::AsciiOnly));
    EXPECT_EQ(def1252.candidates[1], guess(kUtf8, false, 30, DetectionSource::Utf8Valid));
    EXPECT_EQ(def1252.candidates[2], guess(kCp932, false, 30, DetectionSource::LegacyFallback));
}

TEST(EncodingDetection, AsciiOnlyCutIs90) {
    const std::string bytes(100, 'a');
    const auto d = detect(bytes, options(1252, kUtf8, 16));
    EXPECT_EQ(d.candidates[0], guess(kUtf8, false, 90, DetectionSource::AsciiOnly));
}

TEST(EncodingDetection, EmptyUsesDefault) {
    const auto utf8 = detect("", options(1252));
    ASSERT_EQ(utf8.candidates.size(), 1u);
    EXPECT_EQ(utf8.candidates[0], guess(kUtf8, false, 100, DetectionSource::Empty));
    const auto cp = detect("", options(1252, kCp932));
    EXPECT_EQ(cp.candidates[0], guess(kCp932, false, 100, DetectionSource::Empty));
}

TEST(EncodingDetection, UnusableDefaultFallsBackToUtf8) {
    const auto utf16 = detect("ascii", options(1252, kUtf16Le));
    EXPECT_EQ(utf16.candidates[0], guess(kUtf8, false, 100, DetectionSource::AsciiOnly));
    EXPECT_EQ(detect("", options(1252, kUtf16Le)).candidates[0].encoding, kUtf8);
    EXPECT_EQ(detect("", options(1252, Encoding::legacy(936))).candidates[0].encoding, kUtf8);

    frost::text::test::FakeCodecProvider fake;
    SplitMix64 rng(1);
    auto page = std::make_shared<frost::text::test::FakeCodePage>(frost::text::test::randomDbcs(rng));
    fake.serve(page, 932);
    fake.serve(page, 1252);
    fake.failOpen(932, frost::ErrorCode::Unsupported);
    const auto d = detectEncoding("ascii", options(1252, kCp932), fake);
    EXPECT_EQ(d.candidates[0], guess(kUtf8, false, 100, DetectionSource::AsciiOnly));
}

TEST(EncodingDetection, UnknownLegacyCodePageSkipped) {
    for (const std::uint16_t legacy : {std::uint16_t{936}, std::uint16_t{0}}) {
        const auto d = detect(fromU8(u8"caf\u00e9"), options(legacy));
        ASSERT_EQ(d.candidates.size(), 2u) << legacy;
        EXPECT_EQ(d.candidates[0].encoding, kUtf8);
        EXPECT_EQ(d.candidates[1], guess(kCp1252, false, 30, DetectionSource::LegacyFallback));
        const auto invalid = detect("caf\xE9", options(legacy));
        ASSERT_EQ(invalid.candidates.size(), 2u);
        EXPECT_EQ(invalid.candidates[0], guess(kCp1252, false, 40, DetectionSource::LegacyFallback));
        EXPECT_EQ(invalid.candidates[1], guess(kUtf8, false, 10, DetectionSource::Lossy));
    }
}

TEST(EncodingDetection, InvalidUtf8RanksCleanLegacyFirst) {
    // A0 is valid in 1252 (U+00A0) and invalid in 932.
    const auto d = detect("caf\xA0 x", options(932));
    ASSERT_EQ(d.candidates.size(), 3u);
    EXPECT_EQ(d.candidates[0], guess(kCp1252, false, 40, DetectionSource::LegacyFallback));
    EXPECT_EQ(d.candidates[1], guess(kUtf8, false, 10, DetectionSource::Lossy));
    EXPECT_EQ(d.candidates[2], guess(kCp932, false, 10, DetectionSource::Lossy));
    // Junk that no candidate decodes: UTF-8 first among the lossy ones.
    const auto junk = detect(fromHex("81 a0 fd ff"), options(932));
    EXPECT_EQ(junk.candidates[0], guess(kCp1252, false, 40, DetectionSource::LegacyFallback)); // 81 is C1 in 1252 (D4).
}

TEST(EncodingDetection, Cp932WhenSystemLegacyIs932) {
    const auto& f = frost::text::test::fixture("cp932");
    const auto d = detect(f.bytes, options(932));
    ASSERT_FALSE(d.candidates.empty());
    EXPECT_EQ(d.candidates[0], guess(kCp932, false, 64, DetectionSource::LegacyFallback)) << "d = 14";
    // More than 20 pairs: the confidence caps at 70.
    std::string many;
    for (int k = 0; k < 30; ++k) {
        many += fromHex("93 fa");
    }
    EXPECT_EQ(detect(many, options(932)).candidates[0], guess(kCp932, false, 70, DetectionSource::LegacyFallback));
}

TEST(EncodingDetection, SampleCutsUtf8Sequence) {
    const std::string bytes = std::string(15, 'a') + fromU8(u8"\u00e9") + std::string(40, 'b');
    const auto d = detect(bytes, options(1252, kUtf8, 16));
    EXPECT_EQ(d.candidates[0], guess(kUtf8, false, 85, DetectionSource::Utf8Valid)) << "the cut C3 is not invalid";
    // Without the cut, the same prefix ending in C3 is invalid.
    const auto whole = detect(bytes.substr(0, 16), options(1252, kUtf8, 16));
    EXPECT_EQ(whole.candidates[0].source, DetectionSource::LegacyFallback);
}

/*
 * A cut sample's tail counts as valid only when it is a proper prefix of a well-formed sequence by
 * table 3-7, including the special second-byte ranges of E0, ED, F0 and F4 (review of 22cbc80:
 * isProperUtf8Prefix had no direct test). Each sample is 14 ASCII bytes plus a cut tail; the file
 * continues with the bytes that would complete the sequence, then ASCII.
 */
TEST(EncodingDetection, SampleCutTailUsesTable37) {
    struct Case {
        std::string tail;
        std::string rest;
        bool utf8;
    };
    const Case cases[] = {
        {fromHex("e0 a0"), fromHex("80"), true},     {fromHex("e0 80"), fromHex("80"), false},
        {fromHex("ed 9f"), fromHex("bf"), true},     {fromHex("ed a0"), fromHex("80"), false},
        {fromHex("f0 90"), fromHex("80 80"), true},  {fromHex("f0 80"), fromHex("80 80"), false},
        {fromHex("f4 8f"), fromHex("bf bf"), true},  {fromHex("f4 90"), fromHex("80 80"), false},
        {fromHex("c2"), fromHex("a9"), true},        {fromHex("c1"), fromHex("bf"), false},
        {fromHex("e1 80"), fromHex("80"), true},     {fromHex("e1 c0"), fromHex("80"), false},
        {fromHex("f1 80 80"), fromHex("80"), true},  {fromHex("f1 80 c0"), fromHex("80"), false},
        {fromHex("f5 80"), fromHex("80 80"), false},
    };
    for (const Case& c : cases) {
        const std::string sample = std::string(16 - c.tail.size(), 'a') + c.tail;
        const auto d = detect(sample + c.rest + "zz", options(1252, kUtf8, 16));
        ASSERT_FALSE(d.candidates.empty());
        if (c.utf8) {
            EXPECT_EQ(d.candidates[0].source, DetectionSource::Utf8Valid) << toHex(c.tail);
        } else {
            EXPECT_EQ(d.candidates[0], guess(kCp1252, false, 40, DetectionSource::LegacyFallback)) << toHex(c.tail);
        }
    }
}

TEST(EncodingDetection, SampleCutsDbcsPair) {
    // 82 A0 (U+3042) pairs, invalid UTF-8; the 16-byte sample ends with a lead byte.
    std::string bytes = "abc";
    while (bytes.size() < 15) {
        bytes += fromHex("82 a0");
    }
    bytes.resize(15);
    bytes += fromHex("82 a0 82 a0") + "tail";
    ASSERT_EQ(static_cast<std::uint8_t>(bytes[15]), 0x82);
    const auto d = detect(bytes, options(932, kUtf8, 16));
    ASSERT_FALSE(d.candidates.empty());
    EXPECT_EQ(d.candidates[0].encoding, kCp932);
    EXPECT_EQ(d.candidates[0].source, DetectionSource::LegacyFallback) << "the cut lead byte is not invalid";
    EXPECT_EQ(d.candidates[0].confidence, 50 + 6);
}

TEST(EncodingDetection, SampleCutsSurrogatePair) {
    const std::string bytes =
        frost::text::test::oracleEncodeUtf16(std::string(7, 'a') + fromU8(u8"\U0001F600") + "zz", false);
    const auto d = detect(bytes, options(1252, kUtf8, 16));
    EXPECT_EQ(d.candidates[0], guess(kUtf16Le, false, 60 + 35 * 7 / 8, DetectionSource::NulPattern));
}

TEST(EncodingDetection, ReadsOnlySample) {
    const auto seeds = frost::text::test::encodingSeeds(200);
    for (std::uint64_t seed = seeds.first; seed < seeds.last; ++seed) {
        SplitMix64 rng(seed + 4242);
        constexpr std::size_t kSamples[] = {16, 64, 1000};
        const std::size_t sample = kSamples[rng.below(std::size(kSamples))];
        std::string x = frost::text::test::randomEncodingInput(rng, 2 * sample + 64);
        while (x.size() <= sample) {
            x.push_back(static_cast<char>('a' + rng.below(26)));
        }
        const DetectOptions o = options(seed % 2 == 0 ? 1252 : 932, kUtf8, sample);
        const auto base = detect(x, o);
        std::string longer = x;
        const std::uint64_t k = 4 * (1 + rng.below(16));
        for (std::uint64_t i = 0; i < k; ++i) {
            longer.push_back(static_cast<char>(rng.below(256)));
        }
        const auto extended = detect(longer, o);
        ASSERT_EQ(extended.candidates, base.candidates)
            << frost::text::test::reproduceSeed("text_encoding_tests", "EncodingDetection.ReadsOnlySample", seed);
        ASSERT_EQ(extended.binaryLikely, base.binaryLikely);
    }
}

TEST(EncodingDetection, UnavailableCodecSkipped) {
    frost::text::test::FakeCodecProvider fake;
    SplitMix64 rng(3);
    auto page = std::make_shared<frost::text::test::FakeCodePage>(frost::text::test::randomSbcs(rng));
    fake.serve(page, 932);
    fake.serve(page, 1252);
    fake.failOpen(932, frost::ErrorCode::Unsupported);
    const auto d = detectEncoding(fromU8(u8"caf\u00e9"), options(932), fake);
    for (const EncodingGuess& g : d.candidates) {
        EXPECT_NE(g.encoding, kCp932);
    }
    EXPECT_EQ(d.candidates.back().encoding, kCp1252);
}

// Only Unsupported means "not here": any other open failure keeps the page, so the loader reports
// the failure instead of detection hiding it (review of 22cbc80; TextFile.CodecOpenFailureReported).
TEST(EncodingDetection, FailedOpenKeepsPage) {
    frost::text::test::FakeCodecProvider fake;
    SplitMix64 rng(3);
    auto page = std::make_shared<frost::text::test::FakeCodePage>(frost::text::test::randomSbcs(rng));
    fake.serve(page, 932);
    fake.serve(page, 1252);
    fake.failOpen(932, frost::ErrorCode::Busy);
    const auto valid = detectEncoding(fromU8(u8"caf\u00e9"), options(932), fake);
    ASSERT_EQ(valid.candidates.size(), 3u);
    EXPECT_EQ(valid.candidates[1], guess(kCp932, false, 30, DetectionSource::LegacyFallback));
    const auto invalid = detectEncoding("caf\xE9", options(932), fake);
    const auto it = std::find_if(invalid.candidates.begin(), invalid.candidates.end(),
                                 [](const EncodingGuess& g) { return g.encoding == kCp932; });
    ASSERT_NE(it, invalid.candidates.end());
    EXPECT_EQ(*it, guess(kCp932, false, 10, DetectionSource::Lossy)) << "it never decoded the sample";
    // A failed default encoding is kept too.
    EXPECT_EQ(detectEncoding("", options(1252, kCp932), fake).candidates[0].encoding, kCp932);
}

// One open per distinct page, at most 3 (the default, the legacy page and 1252), and none for a
// BOM (review of 22cbc80: the availability probe and the sample decode used to open twice).
TEST(EncodingDetection, OpensEachPageOnce) {
    frost::text::test::FakeCodecProvider fake;
    SplitMix64 rng(4);
    auto page = std::make_shared<frost::text::test::FakeCodePage>(frost::text::test::randomSbcs(rng));
    for (const std::uint16_t id : {std::uint16_t{932}, std::uint16_t{1252}, std::uint16_t{1251}}) {
        fake.serve(page, id);
    }
    struct Case {
        std::string bytes;
        DetectOptions options;
        std::uint64_t opens;
    };
    const Case cases[] = {
        {"caf\xE9 x", options(932, kCp1252), 2},
        {"caf\xE9 x", options(932, Encoding::legacy(1251)), 3},
        {fromU8(u8"caf\u00e9"), options(932, Encoding::legacy(1251)), 3},
        {"caf\xE9 x", options(1252, kCp1252), 1},
        {"caf\xE9 x", options(1252), 1},
        {fromHex("ff fe 41 00"), options(932, kCp1252), 0},
    };
    for (const Case& c : cases) {
        fake.resetCounts();
        (void)detectEncoding(c.bytes, c.options, fake);
        EXPECT_EQ(fake.opens(), c.opens) << toHex(c.bytes) << " legacy " << c.options.legacyCodePage;
    }
}

TEST(EncodingDetection, Deterministic) {
    for (const auto& f : frost::text::test::encodingFixtures()) {
        const DetectOptions o = options(f.legacyCodePage);
        EXPECT_EQ(detect(f.bytes, o).candidates, detect(f.bytes, o).candidates) << f.name;
    }
}

} // namespace
