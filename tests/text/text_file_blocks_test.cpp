// SPDX-License-Identifier: Apache-2.0
//
// TextFileBlocks: decodeBytes across its decode blocks (text_file.cpp's decodeBlocks; ADR 0018,
// D16 and D17), the capacity bound of a decoded text, the round-trip check past its first 64 KiB
// chunk, and the codec error contract at the loader (added for the reviews of 22cbc80).
/*
 * Each block case states its expected text and fidelity by construction (an ASCII run, the
 * sequence that straddles the block boundary, a tail), so a regression in the block loop's carry
 * (pos += consumed), its InputEnd::More at a block end or where it checks the buffer limit shows up
 * as a difference. The real 1 MiB block (kDecodeBlockBytes) is crossed by a lone UTF-16 surrogate,
 * a Shift_JIS pair, lossy UTF-8, a UTF-8 BOM file and the early TooLarge stop; the other cases set
 * LoadOptions::blockBytes to 64 B-128 KiB, which keeps text_encoding_tests inside its budgets (5 s
 * in Windows Debug, 15 s under ASan) on every leg. SmallBlocksMatchOneBlock checks the knob's
 * contract (the block size never changes the result), which the fuzz target's check (d) fuzzes.
 * Big strings are compared with sameText, which reports the first difference instead of printing
 * megabytes.
 */

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/text_codec.hpp"
#include "frost/text/text_file.hpp"
#include "frost/text/unicode_codec.hpp"

#include "text/support/encoding_checks.hpp"
#include "text/support/encoding_fixtures.hpp"
#include "text/support/encoding_oracle.hpp"
#include "text/support/fake_codec.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace std::string_view_literals;
using frost::ByteOffset;
using frost::ErrorCode;
using frost::sched::detail::SplitMix64;
using frost::text::AttemptOutcome;
using frost::text::decodeBytes;
using frost::text::DecodedText;
using frost::text::Encoding;
using frost::text::EncodingForm;
using frost::text::LineSeparator;
using frost::text::LoadOptions;
using frost::text::LossKind;
using frost::text::SaveConsent;
using frost::text::test::FakeCodecProvider;
using frost::text::test::FakeCodePage;
using frost::text::test::fromHex;
using frost::text::test::fromU8;
using frost::text::test::toHex;

constexpr std::size_t kBlock = frost::text::kDecodeBlockBytes; // The default block, 1 MiB.
static_assert(kBlock == std::size_t{1} << 20, "the 1 MiB cases below are sized for this block");

const Encoding kUtf8 = Encoding::utf8();
const Encoding kUtf16Le = Encoding::unicode(EncodingForm::Utf16Le);
const Encoding kUtf16Be = Encoding::unicode(EncodingForm::Utf16Be);
const Encoding kCp932 = Encoding::legacy(932);
const Encoding kCp1252 = Encoding::legacy(1252);
const std::string& replacementText() {
    static const std::string value(frost::text::kReplacementUtf8);
    return value;
}

// Options for a load; `blockBytes` 0 keeps the default block.
LoadOptions options(std::uint16_t legacy, std::optional<Encoding> override = std::nullopt, std::size_t blockBytes = 0) {
    LoadOptions o;
    o.detect.legacyCodePage = legacy;
    o.detect.defaultEncoding = kUtf8;
    o.defaultSeparator = LineSeparator::Lf;
    o.encoding = override;
    if (blockBytes != 0) {
        o.blockBytes = blockBytes;
    }
    return o;
}

// `unit` repeated `count` times, built by doubling (a few appends, cheap in Debug).
std::string repeat(std::string_view unit, std::size_t count) {
    const std::size_t total = unit.size() * count;
    std::string out;
    out.reserve(total);
    if (count > 0) {
        out.append(unit);
    }
    while (out.size() < total) {
        out.append(out, 0, std::min(out.size(), total - out.size()));
    }
    return out;
}

// Equal, or the first difference with 16 bytes of context on each side.
::testing::AssertionResult sameText(std::string_view actual, std::string_view expected) {
    if (actual == expected) {
        return ::testing::AssertionSuccess();
    }
    const std::size_t common = std::min(actual.size(), expected.size());
    const auto at = static_cast<std::size_t>(
        std::mismatch(actual.begin(), actual.begin() + static_cast<std::ptrdiff_t>(common), expected.begin()).first -
        actual.begin());
    const std::size_t from = at < 16 ? 0 : at - 16;
    return ::testing::AssertionFailure() << "sizes " << actual.size() << " and " << expected.size()
                                         << ", first difference at byte " << at << ": "
                                         << toHex(actual.substr(from, 32)) << " vs "
                                         << toHex(expected.substr(from, 32));
}

std::string_view textOf(const DecodedText& d, std::string_view bytes) {
    return d.payloadIsText ? bytes.substr(d.payloadOffset) : std::string_view(d.text);
}

// What a load must report: the text, and for a lossy load the replacement count and the first
// loss's file and text offsets.
struct Expect {
    std::string text;
    std::uint64_t replacements = 0;
    std::optional<std::uint64_t> fileOffset;
    std::optional<std::uint64_t> textOffset;
};

void expectLoad(const std::string& bytes, const LoadOptions& o, Encoding encoding, const Expect& e) {
    auto d = decodeBytes(bytes, o);
    ASSERT_TRUE(d.has_value()) << d.error().describe();
    EXPECT_EQ(d->encoding.format.encoding, encoding);
    EXPECT_TRUE(sameText(textOf(*d, bytes), e.text));
    const frost::text::LoadFidelity& f = d->encoding.fidelity;
    EXPECT_EQ(f.loss, e.replacements > 0 ? LossKind::Replaced : LossKind::None);
    EXPECT_EQ(f.replacements, e.replacements);
    EXPECT_EQ(f.firstLossFileOffset, e.fileOffset);
    EXPECT_EQ(f.firstLossTextOffset,
              e.textOffset ? std::optional<ByteOffset>(ByteOffset{*e.textOffset}) : std::optional<ByteOffset>());
}

// --- Sequences across a block boundary -------------------------------------------------------

TEST(TextFileBlocks, Utf16SurrogatePairAcrossBlocks) {
    // U+1F600's high surrogate ends the first 64-byte block; the decoder must leave it for the
    // second. With a BOM the blocks start after it, so the pair straddles the same payload offset.
    constexpr std::size_t block = 64;
    const std::size_t before = block / 2 - 1;
    const std::string text = std::string(before, 'a') + fromU8(u8"\U0001F600") + std::string(40, 'b');
    for (const bool big : {false, true}) {
        SCOPED_TRACE(big ? "UTF-16BE" : "UTF-16LE");
        const Encoding e = big ? kUtf16Be : kUtf16Le;
        const std::string pair = fromHex(big ? "d8 3d de 00" : "3d d8 00 de");
        const std::string payload =
            repeat(big ? "\0a"sv : "a\0"sv, before) + pair + repeat(big ? "\0b"sv : "b\0"sv, 40);
        ASSERT_EQ(payload.substr(block - 2, 4), pair);
        expectLoad(payload, options(1252, e, block), e, Expect{text, 0, std::nullopt, std::nullopt});
        expectLoad(fromHex(big ? "fe ff" : "ff fe") + payload, options(1252, std::nullopt, block), e,
                   Expect{text, 0, std::nullopt, std::nullopt});
    }
}

TEST(TextFileBlocks, Utf16LoneHighSurrogateEndsBlock) {
    // The default 1 MiB block: a high surrogate ends the first block of the payload (blocks start
    // after the BOM) and the second starts with 'c': one U+FFFD, at file offset 2 + (1 MiB - 2).
    const std::size_t before = kBlock / 2 - 1;
    const std::string bytes = fromHex("ff fe") + repeat("a\0"sv, before) + fromHex("3d d8") + repeat("c\0"sv, 100);
    ASSERT_EQ(bytes.substr(2 + kBlock - 2, 4), fromHex("3d d8 63 00"));
    const std::string text = std::string(before, 'a') + replacementText() + std::string(100, 'c');
    expectLoad(bytes, options(1252), kUtf16Le, Expect{text, 1, kBlock, before});
}

TEST(TextFileBlocks, ShiftJisAcrossBlocks) {
    // The lead byte is the first block's last byte, so it must wait for its trail (InputEnd::More).
    // Decoding it as Final would give U+FFFD and then decode the trail alone (A0 is invalid; 80 is
    // 932's windowsOnlyC1 byte, U+0080).
    // A valid pair across the default 1 MiB block, which also passes the round-trip check there.
    const std::string head(kBlock - 1, 'a');
    expectLoad(head + fromHex("82 a0") + "z\n", options(932, kCp932), kCp932,
               Expect{head + fromU8(u8"あz\n"), 0, std::nullopt, std::nullopt});
    // An invalid pair across a 64-byte block: one 2-byte unit, one U+FFFD.
    const std::string shortHead(63, 'a');
    expectLoad(shortHead + fromHex("85 80") + "z", options(932, kCp932, 64), kCp932,
               Expect{shortHead + replacementText() + "z", 1, 63, 63});
}

TEST(TextFileBlocks, Utf8LossyAcrossBlocks) {
    // The default 1 MiB block.
    struct Case {
        std::string bytes;
        std::string text;
        std::uint64_t at; // The first (only) replacement, the same in the file and the text.
    };
    const std::string head(kBlock - 1, 'a');
    const std::string head2(kBlock - 2, 'a');
    const Case cases[] = {
        // A 3-byte character straddles the boundary and survives; FF in the second block is lossy.
        {head + fromHex("e6 97 a5") + std::string(100, 'b') + "\xFF" + "c",
         head + fromU8(u8"日") + std::string(100, 'b') + replacementText() + "c", kBlock + 102},
        // A truncated 3-byte sequence across the boundary: one U+FFFD for its maximal subpart.
        {head + fromHex("e6 97") + "b", head + replacementText() + "b", kBlock - 1},
        // A 4-byte character split 2 + 2, then FF.
        {head2 + fromHex("f0 9f 98 80") + "\xFF", head2 + fromU8(u8"\U0001F600") + replacementText(), kBlock + 2},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.at);
        expectLoad(c.bytes, options(1252, kUtf8), kUtf8, Expect{c.text, 1, c.at, c.at});
    }
}

TEST(TextFileBlocks, Utf8BomInvalidAfterFirstBlock) {
    // The BOM decides; the payload's 1 MiB blocks start after it, and file offsets include it.
    const std::string body(kBlock + 5, 'a');
    const std::string bytes = fromHex("ef bb bf") + body + "\xFF" + "z";
    auto d = decodeBytes(bytes, options(1252));
    ASSERT_TRUE(d.has_value()) << d.error().describe();
    EXPECT_TRUE(d->encoding.format.bom);
    EXPECT_FALSE(d->payloadIsText);
    EXPECT_EQ(d->payloadOffset, 3u);
    expectLoad(bytes, options(1252), kUtf8, Expect{body + replacementText() + "z", 1, 3 + kBlock + 5, kBlock + 5});
}

// --- The buffer limit across blocks (D17) ---------------------------------------------------

// The platform codecs, counting the input bytes decodePrefix consumes (over all calls and codecs).
class CountingCodecs final : public frost::text::ITextCodecProvider {
public:
    [[nodiscard]] frost::Result<std::unique_ptr<frost::text::ITextCodec>>
    open(std::uint16_t codePage) noexcept override {
        auto inner = frost::text::platformCodecs().open(codePage);
        if (!inner) {
            return std::unexpected(std::move(inner).error());
        }
        return std::unique_ptr<frost::text::ITextCodec>(std::make_unique<Codec>(std::move(*inner), m_decoded));
    }
    [[nodiscard]] std::uint64_t decoded() const noexcept { return m_decoded; }

private:
    class Codec final : public frost::text::ITextCodec {
    public:
        Codec(std::unique_ptr<frost::text::ITextCodec> inner, std::uint64_t& counter) noexcept
            : m_inner(std::move(inner)), m_counter(&counter) {}
        [[nodiscard]] const frost::text::CodePageInfo& info() const noexcept override { return m_inner->info(); }
        [[nodiscard]] frost::Result<std::size_t> decodePrefix(std::string_view bytes,
                                                              std::string& utf8) noexcept override {
            auto consumed = m_inner->decodePrefix(bytes, utf8);
            if (consumed) {
                *m_counter += *consumed;
            }
            return consumed;
        }
        [[nodiscard]] frost::Result<std::size_t> encodePrefix(std::string_view utf8,
                                                              std::string& bytes) noexcept override {
            return m_inner->encodePrefix(utf8, bytes);
        }

    private:
        std::unique_ptr<frost::text::ITextCodec> m_inner;
        std::uint64_t* m_counter;
    };
    std::uint64_t m_decoded = 0;
};

TEST(TextFileBlocks, TooLargeStopsAfterTheCrossingBlock) {
    // Four default blocks of ASCII through windows-1252, which decodes each valid block in one
    // call: a limit of half a block abandons the candidate after its first block, not at the end.
    CountingCodecs codecs;
    LoadOptions o = options(1252, kCp1252);
    o.buffer.maxBufferBytes = kBlock / 2;
    auto r = decodeBytes(std::string(4 * kBlock, 'a'), o, codecs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(codecs.decoded(), kBlock) << "abandoned after the first of four blocks";
}

TEST(TextFileBlocks, LimitCheckedOnTheLastBlock) {
    // UTF-16LE ASCII in two 128 KiB blocks, 128 KiB of text; only the last block crosses a limit
    // one byte below the text. The projected capacity stops at the limit, so the last block is
    // staged and the limit is checked before it is appended.
    constexpr std::size_t block = 128 * 1024;
    const std::size_t units = block;
    const std::string bytes = repeat("a\0"sv, units);
    LoadOptions o = options(1252, kUtf16Le, block);
    o.buffer.maxBufferBytes = units - 1;
    auto over = decodeBytes(bytes, o);
    ASSERT_FALSE(over.has_value());
    EXPECT_EQ(over.error().code(), ErrorCode::OutOfRange);
    o.buffer.maxBufferBytes = units;
    auto fits = decodeBytes(bytes, o);
    ASSERT_TRUE(fits.has_value()) << fits.error().describe();
    EXPECT_TRUE(sameText(fits->text, std::string(units, 'a')));
}

// --- The decoded text's capacity (D17, B15) -------------------------------------------------

// The bound decodeBytes documents: at most 1/16 of the text plus 64 KiB unused (64 bytes more for
// allocator rounding).
::testing::AssertionResult boundedCapacity(const std::string& text) {
    const std::size_t limit = text.size() + text.size() / 16 + 64 * 1024 + 64;
    if (text.capacity() <= limit) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "capacity " << text.capacity() << " for " << text.size()
                                         << " bytes of text (limit " << limit << ")";
}

TEST(TextFileBlocks, DecodedTextCapacityIsBounded) {
    /*
     * Before the review fix the text kept the payload's size as capacity (2x for UTF-16, 4x for
     * UTF-32) or std::string's growth (1.5x or 2x) for texts larger than their input. The
     * one-block cases fail that way; the 64 KiB-block cases cover a representative first block,
     * a regrowth after an ASCII first block, and shrinking after a first block that over-projects.
     */
    constexpr std::size_t small = 64 * 1024;
    const std::string sentence = "caf\xE9 is a word in this line of a windows-1252 text file, 100 bytes long....\r\n";
    const std::string sentenceUtf8 =
        "caf\xC3\xA9 is a word in this line of a windows-1252 text file, 100 bytes long....\r\n";
    struct Case {
        const char* name;
        std::string bytes;
        LoadOptions options;
        std::string text;
        bool decodedAgain = false; // Nothing valid: the first lossy candidate is decoded again.
    };
    const std::string cjk = fromU8(u8"日");
    const Case cases[] = {
        {"UTF-16LE ASCII", fromHex("ff fe") + repeat("a\0"sv, 128 * 1024), options(1252), std::string(128 * 1024, 'a')},
        {"UTF-32LE ASCII", fromHex("ff fe 00 00") + repeat("a\0\0\0"sv, 64 * 1024), options(1252),
         std::string(64 * 1024, 'a')},
        {"UTF-16LE CJK", repeat("\xE5\x65"sv, 128 * 1024 + 2048), options(1252, kUtf16Le, small),
         repeat(cjk, 128 * 1024 + 2048)},
        {"UTF-16LE ASCII, then CJK", repeat("a\0"sv, 32 * 1024) + repeat("\xE5\x65"sv, 64 * 1024),
         options(1252, kUtf16Le, small), std::string(32 * 1024, 'a') + repeat(cjk, 64 * 1024)},
        // The first block projects about 360 KiB for a 160 KiB text; the accepted text is shrunk.
        {"UTF-16LE CJK, then ASCII", repeat("\xE5\x65"sv, 32 * 1024) + repeat("a\0"sv, 64 * 1024),
         options(1252, kUtf16Le, small), repeat(cjk, 32 * 1024) + std::string(64 * 1024, 'a')},
        // The old reservation (the payload's size) had to grow for these two.
        {"windows-1252, 1% non-ASCII", repeat(sentence, kBlock / 400), options(1252, kCp1252),
         repeat(sentenceUtf8, kBlock / 400)},
        {"lossy UTF-8", repeat(std::string(999, 'x') + "\xFF", 256), options(1252, kUtf8),
         repeat(std::string(999, 'x') + replacementText(), 256)},
        /*
         * After FF FE 00 00 the last unit, 00 D8 00 00, is a surrogate as UTF-32LE and a lone high
         * surrogate as UTF-16LE: both candidates are lossy, so UTF-32LE is decoded again (step 3)
         * into the released buffer. Its first block over-projects the text, which is shrunk too.
         */
        {"UTF-32LE, CJK then ASCII, decoded again",
         fromHex("ff fe 00 00") + repeat("\xE5\x65\0\0"sv, 16 * 1024) + repeat("a\0\0\0"sv, 32 * 1024) +
             fromHex("00 d8 00 00"),
         options(1252, std::nullopt, small), repeat(cjk, 16 * 1024) + std::string(32 * 1024, 'a') + replacementText(),
         true},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        auto d = decodeBytes(c.bytes, c.options);
        ASSERT_TRUE(d.has_value()) << d.error().describe();
        ASSERT_FALSE(d->payloadIsText);
        EXPECT_TRUE(sameText(d->text, c.text));
        EXPECT_TRUE(boundedCapacity(d->text));
        if (c.decodedAgain) {
            ASSERT_EQ(d->tried.size(), 2u);
            EXPECT_EQ(d->tried[0].guess.encoding, Encoding::unicode(EncodingForm::Utf32Le));
            EXPECT_EQ(d->tried[0].outcome, AttemptOutcome::Accepted);
            EXPECT_EQ(d->tried[1].outcome, AttemptOutcome::Lossy);
            EXPECT_EQ(d->encoding.fidelity.replacements, 1u);
        }
    }
    // decodeTextFile moves the same text into the buffer.
    auto loaded = frost::text::decodeTextFile(cases[0].bytes, cases[0].options);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(sameText(loaded->buffer.current().text(), cases[0].text));
}

// --- The block size never changes the result (D16) ------------------------------------------

TEST(TextFileBlocks, SmallBlocksMatchOneBlock) {
    // Every fixture in 16-byte blocks, then random inputs (one override per seed, sometimes a
    // 256-byte buffer limit) in blocks of 16 and of 17..63 bytes; the fuzz target's check (d) runs
    // the same comparison on fuzzed inputs.
    for (const auto& f : frost::text::test::encodingFixtures()) {
        const auto ok = frost::text::test::checkBlockInvariance(f.bytes, frost::text::test::fixtureOptions(f),
                                                                frost::text::platformCodecs(), 16);
        EXPECT_TRUE(ok.has_value()) << f.name << ": " << ok.error().describe();
    }
    const auto seeds = frost::text::test::encodingSeeds(48);
    for (std::uint64_t seed = seeds.first; seed < seeds.last; ++seed) {
        SplitMix64 rng(seed * 131 + 5);
        const std::string bytes = frost::text::test::randomEncodingInput(rng, 1024);
        LoadOptions o =
            options(seed % 2 == 0 ? 1252 : 932, frost::text::test::overrideByIndex(static_cast<unsigned>(seed % 8)));
        if (seed % 5 == 0) {
            o.buffer.maxBufferBytes = 256;
        }
        for (const std::size_t block : {std::size_t{16}, std::size_t{17 + seed % 47}}) {
            const auto ok = frost::text::test::checkBlockInvariance(bytes, o, frost::text::platformCodecs(), block);
            ASSERT_TRUE(ok.has_value()) << "block " << block << ": " << ok.error().describe() << "\ninput "
                                        << toHex(bytes.substr(0, 256)) << "\n"
                                        << frost::text::test::reproduceSeed(
                                               "text_encoding_tests", "TextFileBlocks.SmallBlocksMatchOneBlock", seed);
        }
    }
}

// --- The round-trip check past its first 64 KiB chunk (D17) ---------------------------------

TEST(TextFileBlocks, NotRoundTripOffsetsPastFirstChunk) {
    // 87 90 (U+2252) re-encodes as 81 E0. Chunks are 64 KiB of text, so these mismatches sit in
    // the second chunk; the kana case has different file and text offsets (2 bytes vs 3).
    struct Case {
        std::string bytes;
        std::uint64_t file;
        std::uint64_t text;
    };
    const Case cases[] = {
        {std::string(65536 + 10, 'a') + fromHex("87 90") + "z", 65546, 65546},
        {repeat(fromHex("82 a0"), 21846) + fromHex("87 90") + "z", 43692, 65538},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.file);
        auto d = decodeBytes(c.bytes, options(932, kCp932));
        ASSERT_TRUE(d.has_value()) << d.error().describe();
        const frost::text::LoadFidelity& f = d->encoding.fidelity;
        EXPECT_EQ(f.loss, LossKind::NotRoundTrip);
        EXPECT_EQ(f.firstLossFileOffset, c.file);
        EXPECT_EQ(f.firstLossTextOffset, ByteOffset{c.text});
    }
}

// --- The codec error contract at the loader (review of 22cbc80) -----------------------------

std::shared_ptr<FakeCodePage> sbcs(std::uint64_t seed) {
    SplitMix64 rng(seed);
    return std::make_shared<FakeCodePage>(frost::text::test::randomSbcs(rng));
}

TEST(TextFileBlocks, CodecErrorsAreInternal) {
    // A codec that returns InvalidArgument (an OS mapping passed through) must not read as "no
    // reversible mapping" on save, nor as anything but Internal on load.
    FakeCodecProvider fake;
    fake.serve(sbcs(9), 1252);
    auto loaded = frost::text::decodeTextFile("abc", options(1252, kCp1252), fake);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    fake.failEncodeAfter(0, ErrorCode::InvalidArgument);
    auto saved = frost::text::encodeForSave(loaded->buffer.current(), loaded->encoding, SaveConsent::None, fake);
    ASSERT_FALSE(saved.has_value());
    EXPECT_EQ(saved.error().code(), ErrorCode::Internal) << saved.error().describe();
    auto at = frost::text::firstUnencodable(loaded->buffer.current(), kCp1252, fake);
    ASSERT_FALSE(at.has_value());
    EXPECT_EQ(at.error().code(), ErrorCode::Internal);

    FakeCodecProvider failing;
    failing.serve(sbcs(9), 1252);
    failing.failDecodeAfter(0, ErrorCode::InvalidArgument);
    auto decoded = decodeBytes("abc", options(1252, kCp1252), failing);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code(), ErrorCode::Internal) << decoded.error().describe();
}

TEST(TextFileBlocks, CodecOpenFailureReported) {
    // Detection keeps a page whose open failed with anything but Unsupported, and the loader
    // reports that failure when it reaches the page instead of skipping it (TextFile's lossy
    // fallback would otherwise hide it).
    const auto page = sbcs(3);
    std::optional<std::uint8_t> undefined;
    for (unsigned b = 0x80; b <= 0xFF && !undefined; ++b) {
        const auto byte = static_cast<std::uint8_t>(b);
        const auto& c1 = page->info().windowsOnlyC1;
        if (!page->single(byte) && std::find(c1.begin(), c1.end(), byte) == c1.end()) {
            undefined = byte;
        }
    }
    ASSERT_TRUE(undefined.has_value()) << "the seed's page defines every byte";
    const std::string bytes = std::string("x") + static_cast<char>(*undefined) + "y"; // Lossy in UTF-8 and the page.
    {
        FakeCodecProvider fake;
        fake.serve(page, 932);
        fake.serve(page, 1252);
        fake.failOpen(932, ErrorCode::Busy);
        auto r = decodeBytes(bytes, options(932), fake);
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code(), ErrorCode::Busy);
        ASSERT_FALSE(r.error().context().empty());
        EXPECT_EQ(r.error().context().back(), "while decoding as Shift_JIS");
    }
    {
        // Unsupported is a missing converter: skipped, and the load takes the lossy fallback.
        FakeCodecProvider fake;
        fake.serve(page, 932);
        fake.serve(page, 1252);
        fake.failOpen(932, ErrorCode::Unsupported);
        auto r = decodeBytes(bytes, options(932), fake);
        ASSERT_TRUE(r.has_value()) << r.error().describe();
        EXPECT_EQ(r->encoding.fidelity.loss, LossKind::Replaced);
        for (const frost::text::CandidateAttempt& a : r->tried) {
            EXPECT_NE(a.guess.encoding, kCp932);
        }
    }
}

} // namespace
