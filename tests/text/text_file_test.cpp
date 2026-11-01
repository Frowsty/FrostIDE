// SPDX-License-Identifier: Apache-2.0
//
// TextFile: decodeBytes, decodeTextFile, loadTextFile, the save guard and encodeForSave(To)
// (text_file.hpp; ADR 0018, D6, D7, D10, D15, D17 and D18), in memory and over MemFs; and
// FormatStatus, the status-bar values (row 242's inputs).

#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/text_file.hpp"

#include "support/crash_dialogs.hpp"
#include "text/support/encoding_checks.hpp"
#include "text/support/encoding_fixtures.hpp"
#include "text/support/encoding_oracle.hpp"
#include "text/support/fake_codec.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using frost::ByteOffset;
using frost::ErrorCode;
using frost::sched::detail::SplitMix64;
using frost::text::AttemptOutcome;
using frost::text::decodeBytes;
using frost::text::decodeTextFile;
using frost::text::DetectionSource;
using frost::text::DocumentEncoding;
using frost::text::encodeForSave;
using frost::text::encodeForSaveTo;
using frost::text::Encoding;
using frost::text::EncodingForm;
using frost::text::LineSeparator;
using frost::text::LoadedText;
using frost::text::LoadOptions;
using frost::text::LossKind;
using frost::text::SaveConsent;
using frost::text::test::fixture;
using frost::text::test::fromHex;
using frost::text::test::fromU8;
using frost::text::test::toHex;

const Encoding kUtf8 = Encoding::utf8();
const Encoding kUtf16Le = Encoding::unicode(EncodingForm::Utf16Le);
const Encoding kCp1252 = Encoding::legacy(1252);
const Encoding kCp932 = Encoding::legacy(932);

LoadOptions options(std::uint16_t legacy, std::optional<Encoding> override = std::nullopt) {
    LoadOptions o;
    o.detect.legacyCodePage = legacy;
    o.detect.defaultEncoding = kUtf8;
    o.defaultSeparator = LineSeparator::Lf;
    o.encoding = override;
    return o;
}

LoadedText load(std::string bytes, const LoadOptions& o) {
    auto loaded = decodeTextFile(std::move(bytes), o);
    EXPECT_TRUE(loaded.has_value()) << loaded.error().describe();
    if (!loaded) {
        auto empty = decodeTextFile(std::string(), options(1252));
        return std::move(*empty);
    }
    return std::move(*loaded);
}

std::string textOf(const LoadedText& l) { return l.buffer.current().text(); }

std::string saved(const LoadedText& l, SaveConsent consent = SaveConsent::None) {
    auto bytes = encodeForSave(l.buffer.current(), l.encoding, consent);
    EXPECT_TRUE(bytes.has_value()) << bytes.error().describe();
    return bytes.value_or(std::string("<error>"));
}

TEST(TextFile, OverrideWins) {
    const auto& f = fixture("cp1252");
    const LoadedText l = load(f.bytes, options(1252, kUtf8));
    EXPECT_EQ(l.encoding.format.encoding, kUtf8);
    EXPECT_EQ(l.encoding.detection.source, DetectionSource::UserOverride);
    EXPECT_EQ(l.encoding.detection.confidence, 100);
    EXPECT_EQ(l.encoding.fidelity.loss, LossKind::Replaced);
    ASSERT_EQ(l.tried.size(), 1u);
    EXPECT_EQ(l.tried[0].outcome, AttemptOutcome::Accepted);
    const LoadedText as932 = load(f.bytes, options(1252, kCp932));
    EXPECT_EQ(as932.encoding.format.encoding, kCp932);
}

TEST(TextFile, OverrideStripsOnlyOwnBom) {
    const LoadedText utf8 = load("\xEF\xBB\xBF"
                                 "a",
                                 options(1252, kUtf8));
    EXPECT_TRUE(utf8.encoding.format.bom);
    EXPECT_EQ(textOf(utf8), "a");
    const LoadedText utf16 = load("\xEF\xBB\xBF"
                                  "a",
                                  options(1252, kUtf16Le));
    EXPECT_FALSE(utf16.encoding.format.bom) << "a UTF-8 BOM is not UTF-16LE's";
    EXPECT_EQ(textOf(utf16), fromU8(u8"\ubbef\u61bf"));
    const LoadedText le = load(fromHex("ff fe 41 00"), options(1252, kUtf16Le));
    EXPECT_TRUE(le.encoding.format.bom);
    EXPECT_EQ(textOf(le), "A");
    const LoadedText be = load(fromHex("ff fe 41 00"), options(1252, Encoding::unicode(EncodingForm::Utf16Be)));
    EXPECT_FALSE(be.encoding.format.bom);
    EXPECT_EQ(textOf(be), fromU8(u8"\ufffe\u4100"));
    EXPECT_EQ(saved(be), fromHex("ff fe 41 00"));
}

TEST(TextFile, OverrideUnsupportedCodePage) {
    auto r = decodeTextFile("abc", options(1252, Encoding::legacy(936)));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::Unsupported);
    auto invalid = decodeTextFile("abc", options(1252, Encoding{EncodingForm::Utf8, 1252}));
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code(), ErrorCode::Unsupported);
}

TEST(TextFile, FallbackWhenLaterBytesInvalid) {
    std::string bytes;
    while (bytes.size() < 60 * 1024) {
        bytes += fromU8(u8"// caf\u00e9 \u65e5\u672c \U0001F600\r\n");
    }
    bytes.resize(102400, 'x'); // ASCII after the first 60 KiB, so 102400 is a char boundary.
    bytes += "\xE9"
             "tail\r\n";
    const LoadedText l = load(bytes, options(1252));
    EXPECT_EQ(l.encoding.format.encoding, kCp1252);
    EXPECT_TRUE(l.encoding.fidelity.lossless());
    EXPECT_EQ(l.encoding.detection, (frost::text::EncodingGuess{kCp1252, false, 30, DetectionSource::LegacyFallback}));
    ASSERT_EQ(l.tried.size(), 2u);
    EXPECT_EQ(l.tried[0].guess.encoding, kUtf8);
    EXPECT_EQ(l.tried[0].outcome, AttemptOutcome::Lossy);
    EXPECT_EQ(l.tried[0].fidelity.loss, LossKind::Replaced);
    EXPECT_EQ(l.tried[0].fidelity.firstLossFileOffset, 102400u);
    EXPECT_EQ(l.tried[1].outcome, AttemptOutcome::Accepted);
    EXPECT_EQ(saved(l), bytes);
}

TEST(TextFile, NotRoundTripIsAccepted) {
    const auto& f = fixture("cp932-nec-dup");
    const LoadedText l = load(f.bytes, options(932));
    EXPECT_EQ(l.encoding.format.encoding, kCp932) << "valid-first: not 1252";
    EXPECT_EQ(l.encoding.fidelity.loss, LossKind::NotRoundTrip);
    EXPECT_EQ(l.encoding.fidelity.firstLossFileOffset, 0u);
    EXPECT_EQ(l.encoding.fidelity.firstLossTextOffset, ByteOffset{0});
    ASSERT_FALSE(l.tried.empty());
    EXPECT_EQ(l.tried[0].outcome, AttemptOutcome::Accepted);
    EXPECT_EQ(textOf(l), fromU8(u8"\u2252\u2252"));
    // A later mismatch: the offsets point at the duplicate, not at the start.
    const LoadedText later = load(fromHex("41 81 e0 42 87 90"), options(932, kCp932));
    EXPECT_EQ(later.encoding.fidelity.loss, LossKind::NotRoundTrip);
    EXPECT_EQ(later.encoding.fidelity.firstLossFileOffset, 4u);
    EXPECT_EQ(later.encoding.fidelity.firstLossTextOffset, ByteOffset{5});
}

TEST(TextFile, LossyMarksDocument) {
    const auto& f = fixture("invalid-utf8");
    const LoadedText l = load(f.bytes, frost::text::test::fixtureOptions(f));
    EXPECT_EQ(l.encoding.fidelity.loss, LossKind::Replaced);
    EXPECT_EQ(l.encoding.fidelity.replacements, 33u);
    EXPECT_EQ(l.encoding.fidelity.firstLossFileOffset, 0u);
    EXPECT_EQ(l.encoding.fidelity.firstLossTextOffset, ByteOffset{0});
    const LoadedText later = load("ab\xFF"
                                  "cd\xFF",
                                  options(1252, kUtf8));
    EXPECT_EQ(later.encoding.fidelity.replacements, 2u);
    EXPECT_EQ(later.encoding.fidelity.firstLossFileOffset, 2u);
    EXPECT_EQ(later.encoding.fidelity.firstLossTextOffset, ByteOffset{2});
    const LoadedText bom = load(fromHex("ef bb bf 61 ff"), options(1252, kUtf8));
    EXPECT_EQ(bom.encoding.fidelity.firstLossFileOffset, 4u) << "file offsets include the BOM";
    EXPECT_EQ(bom.encoding.fidelity.firstLossTextOffset, ByteOffset{1});
}

TEST(TextFile, SaveGuardIsCorrupt) {
    const LoadedText l = load("a\xFF", options(1252, kUtf8));
    auto r = encodeForSave(l.buffer.current(), l.encoding);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::Corrupt);
    EXPECT_NE(r.error().message().find("saving would not restore the original bytes"), std::string::npos);
    EXPECT_NE(r.error().message().find("replaced (1 replacements"), std::string::npos) << r.error().message();
}

TEST(TextFile, SaveWithConsent) {
    const LoadedText l = load("a\xFF", options(1252, kUtf8));
    EXPECT_EQ(saved(l, SaveConsent::AcceptLoss), "a" + std::string(frost::text::kReplacementUtf8));
}

TEST(TextFile, AfterSaveClearsGuard) {
    LoadedText l = load("a\xFF", options(1252, kUtf8));
    const DocumentEncoding after = l.encoding.afterSave();
    EXPECT_TRUE(after.fidelity.lossless());
    EXPECT_EQ(after.format, l.encoding.format);
    EXPECT_EQ(after.detection, l.encoding.detection);
    auto r = encodeForSave(l.buffer.current(), after);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "a" + std::string(frost::text::kReplacementUtf8));
}

TEST(TextFile, ConvertedToKeepsGuard) {
    const LoadedText l = load("a\xFF", options(1252, kUtf8));
    const DocumentEncoding converted = l.encoding.convertedTo(kUtf16Le, true);
    EXPECT_EQ(converted.format.encoding, kUtf16Le);
    EXPECT_TRUE(converted.format.bom);
    EXPECT_EQ(converted.fidelity, l.encoding.fidelity);
    EXPECT_EQ(converted.detection, (frost::text::EncodingGuess{kUtf16Le, true, 100, DetectionSource::UserOverride}));
    auto guarded = encodeForSave(l.buffer.current(), converted);
    ASSERT_FALSE(guarded.has_value());
    EXPECT_EQ(guarded.error().code(), ErrorCode::Corrupt);
    auto accepted = encodeForSave(l.buffer.current(), converted, SaveConsent::AcceptLoss);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(toHex(*accepted), "ff fe 61 00 fd ff");
}

TEST(TextFile, LossyLegacyNeedsConversion) {
    // 1253 leaves AA undefined on both OSes (F1): a Replaced legacy load.
    const LoadedText greek = load("a\xAA"
                                  "b",
                                  options(1252, Encoding::legacy(1253)));
    ASSERT_EQ(greek.encoding.fidelity.loss, LossKind::Replaced);
    auto refused = encodeForSave(greek.buffer.current(), greek.encoding, SaveConsent::AcceptLoss);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(refused.error().message().find("U+FFFD at byte 1"), std::string::npos) << refused.error().message();
    // A lossy UTF-8 document converted to windows-1252 meets its U+FFFD the same way.
    const LoadedText l = load("x\xFF"
                              "y",
                              options(1252, kUtf8));
    const DocumentEncoding as1252 = l.encoding.convertedTo(kCp1252, false);
    auto r = encodeForSave(l.buffer.current(), as1252, SaveConsent::AcceptLoss);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(r.error().message().find("U+FFFD at byte 1 has no reversible mapping in windows-1252"), std::string::npos)
        << r.error().message();
    const DocumentEncoding back = as1252.convertedTo(kUtf8, false);
    auto ok = encodeForSave(l.buffer.current(), back, SaveConsent::AcceptLoss);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, "x" + std::string(frost::text::kReplacementUtf8) + "y");
    const DocumentEncoding greekUtf8 = greek.encoding.convertedTo(kUtf8, false);
    EXPECT_EQ(encodeForSave(greek.buffer.current(), greekUtf8, SaveConsent::AcceptLoss).value_or(""),
              "a" + std::string(frost::text::kReplacementUtf8) + "b");
}

TEST(TextFile, UnencodableOnSave) {
    LoadedText l = load("ab", options(1252, kCp1252));
    ASSERT_TRUE(l.buffer.insert(ByteOffset{1}, fromU8(u8"\U0001F600")).has_value());
    auto r = encodeForSave(l.buffer.current(), l.encoding);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(r.error().message().find("U+1F600 at byte 1 has no reversible mapping in windows-1252"),
              std::string::npos)
        << r.error().message();
    auto at = frost::text::firstUnencodable(l.buffer.current(), kCp1252);
    ASSERT_TRUE(at.has_value());
    EXPECT_EQ(*at, ByteOffset{1});
    EXPECT_EQ(frost::text::firstUnencodable(l.buffer.current(), kUtf16Le).value(), std::nullopt);
    auto unsupported = frost::text::firstUnencodable(l.buffer.current(), Encoding::legacy(936));
    ASSERT_FALSE(unsupported.has_value());
    EXPECT_EQ(unsupported.error().code(), ErrorCode::Unsupported);
    EXPECT_FALSE(frost::text::firstUnencodable(l.buffer.current(), Encoding{EncodingForm::CodePage, 1200}).has_value());
}

TEST(TextFileDeathTest, BomOnCodePageIsPrecondition) {
    const LoadedText l = load("ab", options(1252, kCp1252));
    DocumentEncoding bad = l.encoding;
    bad.format.bom = true;
    EXPECT_DEATH(
        {
            frost::test::disableCrashDialogs();
            (void)encodeForSave(l.buffer.current(), bad);
        },
        "precondition violated: state.format.isValid");
    EXPECT_DEATH(
        {
            frost::test::disableCrashDialogs();
            (void)l.encoding.convertedTo(kCp1252, true);
        },
        "precondition violated");
}

TEST(TextFile, FormatIsValid) {
    using frost::text::TextFileFormat;
    EXPECT_TRUE((TextFileFormat{kUtf8, true, LineSeparator::CrLf}).isValid());
    EXPECT_TRUE((TextFileFormat{kCp1252, false, LineSeparator::Lf}).isValid());
    EXPECT_FALSE((TextFileFormat{kCp1252, true, LineSeparator::Lf}).isValid());
    EXPECT_FALSE((TextFileFormat{kUtf8, false, static_cast<LineSeparator>(3)}).isValid());
    EXPECT_FALSE((TextFileFormat{Encoding{EncodingForm::CodePage, 65001}, false, LineSeparator::Lf}).isValid());
    EXPECT_FALSE((TextFileFormat{Encoding{static_cast<EncodingForm>(6), 0}, false, LineSeparator::Lf}).isValid());
    EXPECT_FALSE((TextFileFormat{Encoding{EncodingForm::Utf16Le, 5}, false, LineSeparator::Lf}).isValid());
}

TEST(TextFile, BufferLimitIsOutOfRange) {
    LoadOptions o = options(1252);
    o.buffer.maxBufferBytes = 64;
    auto r = decodeTextFile(std::string(100, 'a'), o);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
    auto fits = decodeTextFile(std::string(64, 'a'), o);
    EXPECT_TRUE(fits.has_value());
}

TEST(TextFile, OutputCapStopsEarly) {
    {
        LoadOptions o = options(1252, kUtf8);
        o.buffer.maxBufferBytes = 4096;
        auto r = decodeBytes(std::string(4096, '\xFF'), o);
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
    }
    {
        // B1 is U+FF71 in Shift_JIS (3 UTF-8 bytes) and U+00B1 in windows-1252 (2 bytes).
        LoadOptions o = options(932);
        o.buffer.maxBufferBytes = 2500;
        auto r = decodeBytes(std::string(1000, '\xB1'), o);
        ASSERT_TRUE(r.has_value()) << r.error().describe();
        EXPECT_EQ(r->encoding.format.encoding, kCp1252);
        ASSERT_GE(r->tried.size(), 2u);
        EXPECT_EQ(r->tried[0].guess.encoding, kCp932);
        EXPECT_EQ(r->tried[0].outcome, AttemptOutcome::TooLarge);
        EXPECT_EQ(r->tried[1].guess.encoding, kCp1252);
        EXPECT_EQ(r->tried[1].outcome, AttemptOutcome::Accepted);
    }
}

TEST(TextFile, CodecIllFormedOutputIsInternal) {
    frost::text::test::FakeCodecProvider fake;
    SplitMix64 rng(5);
    fake.serve(std::make_shared<frost::text::test::FakeCodePage>(frost::text::test::randomSbcs(rng)), 1252);
    fake.setIllFormedOutput(true);
    auto r = decodeTextFile("abc", options(1252, kCp1252), fake);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::Internal);
}

class TextFileFs : public ::testing::Test {
protected:
    frost::ManualClock clock;
    frost::io::MemFs fs{clock, frost::io::PathStyle::Posix};
    void SetUp() override { ASSERT_TRUE(fs.createDirectories("/proj").has_value()); }
};

TEST_F(TextFileFs, ReadFaultsPropagate) {
    ASSERT_TRUE(fs.writeFileAtomic("/proj/a.txt", "hello").has_value());
    struct Case {
        frost::io::FsOp op;
        ErrorCode code;
    };
    for (const Case c :
         {Case{frost::io::FsOp::Status, ErrorCode::Busy}, Case{frost::io::FsOp::Read, ErrorCode::PermissionDenied},
          Case{frost::io::FsOp::Read, ErrorCode::Busy}}) {
        fs.injectFault(c.op, "/proj/a.txt", c.code);
        auto r = frost::text::loadTextFile(fs, "/proj/a.txt", options(1252));
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code(), c.code);
        ASSERT_FALSE(r.error().context().empty());
        EXPECT_EQ(r.error().context().back(), "while loading /proj/a.txt");
    }
    auto missing = frost::text::loadTextFile(fs, "/proj/missing.txt", options(1252));
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(missing.error().context().back(), "while loading /proj/missing.txt");
    auto ok = frost::text::loadTextFile(fs, "/proj/a.txt", options(1252));
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->buffer.current().text(), "hello");
}

TEST_F(TextFileFs, FileTooLargeRefusedBeforeRead) {
    ASSERT_TRUE(fs.writeFileAtomic("/proj/big.txt", std::string(11, 'a')).has_value());
    fs.injectFault(frost::io::FsOp::Read, "/proj/big.txt", ErrorCode::IoError);
    LoadOptions o = options(1252);
    o.maxFileBytes = 10;
    auto r = frost::text::loadTextFile(fs, "/proj/big.txt", o);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
    EXPECT_NE(r.error().message().find("the file has 11 bytes, more than the limit 10"), std::string::npos);
    EXPECT_EQ(fs.pendingFaults(), 1u) << "the file was never read";
    o.maxFileBytes = 11;
    auto read = frost::text::loadTextFile(fs, "/proj/big.txt", o);
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().code(), ErrorCode::IoError) << "now it is read";
}

TEST(TextFile, NewLineSeparatorIsDominant) {
    EXPECT_EQ(load("a\r\nb\r\nc\nd\re\r\n", options(1252)).encoding.format.lineSeparator, LineSeparator::CrLf);
    EXPECT_EQ(load("a\rb\rc", options(1252)).encoding.format.lineSeparator, LineSeparator::Cr);
    LoadOptions o = options(1252);
    o.defaultSeparator = LineSeparator::CrLf;
    EXPECT_EQ(load("no breaks", o).encoding.format.lineSeparator, LineSeparator::CrLf);
    EXPECT_EQ(load("a\nb\r\n", o).encoding.format.lineSeparator, LineSeparator::CrLf) << "a tie keeps the default";
    EXPECT_EQ(load("a\nb\r\n", options(1252)).encoding.format.lineSeparator, LineSeparator::Lf);
}

TEST(TextFile, DecodeBytesZeroCopy) {
    struct Case {
        std::string bytes;
        LoadOptions options;
        bool payloadIsText;
        std::size_t offset;
    };
    const Case cases[] = {
        {fromU8(u8"caf\u00e9"), options(1252), true, 0},
        {"\xEF\xBB\xBF"
         "abc",
         options(1252), true, 3},
        {"", options(1252), true, 0},
        {fromHex("ff fe 41 00"), options(1252), false, 2},
        {"caf\xE9", options(1252), false, 0},
        {"a\xFF", options(1252, kUtf8), false, 0},
        {"abc", options(1252, kCp1252), false, 0},
    };
    for (const Case& c : cases) {
        auto d = decodeBytes(c.bytes, c.options);
        ASSERT_TRUE(d.has_value());
        EXPECT_EQ(d->payloadIsText, c.payloadIsText) << toHex(c.bytes);
        EXPECT_EQ(d->payloadOffset, c.offset) << toHex(c.bytes);
        if (d->payloadIsText) {
            EXPECT_TRUE(d->text.empty());
        }
        const std::string_view text = d->payloadIsText ? std::string_view(c.bytes).substr(d->payloadOffset) : d->text;
        auto l = decodeTextFile(c.bytes, c.options);
        ASSERT_TRUE(l.has_value());
        EXPECT_EQ(l->buffer.current().text(), text);
        EXPECT_EQ(l->encoding, d->encoding);
        EXPECT_EQ(l->tried, d->tried);
        EXPECT_EQ(l->eol, d->eol);
    }
}

std::string bigText(std::size_t bytes) {
    std::string text;
    while (text.size() < bytes) {
        text += fromU8(u8"line \u65e5\u672c \uff76 // x\r\n"); // Encodable in every Unicode form and in 932.
    }
    return text;
}

TEST(TextFile, EncodeForSaveToMatchesString) {
    const std::string text = bigText(300 * 1024);
    for (const Encoding e : {kUtf8, kUtf16Le, Encoding::unicode(EncodingForm::Utf32Be), kCp932}) {
        for (const std::uint32_t piece : {4096u, 5u}) {
            LoadOptions o = options(1252, kUtf8);
            o.buffer.maxPieceBytes = piece;
            LoadedText l = load(text, o);
            const DocumentEncoding state = l.encoding.convertedTo(e, e.isUnicode());
            auto encoded = encodeForSave(l.buffer.current(), state);
            ASSERT_TRUE(encoded.has_value()) << encoded.error().describe();
            const std::string& whole = *encoded;
            std::string streamed;
            std::size_t pieces = 0;
            auto r = encodeForSaveTo(l.buffer.current(), state, SaveConsent::None,
                                     [&](std::string_view bytes) -> frost::Result<void> {
                                         EXPECT_LE(bytes.size(), 64u * 1024u);
                                         streamed.append(bytes);
                                         ++pieces;
                                         return {};
                                     });
            ASSERT_TRUE(r.has_value()) << r.error().describe();
            EXPECT_EQ(streamed, whole) << frost::text::encodingName(e) << " piece " << piece;
            EXPECT_GT(pieces, 1u);
        }
    }
}

TEST(TextFile, GuardBeforeFirstSinkCall) {
    const LoadedText lossy = load("a\xFF", options(1252, kUtf8));
    int calls = 0;
    const auto sink = [&](std::string_view) -> frost::Result<void> {
        ++calls;
        return {};
    };
    auto corrupt = encodeForSaveTo(lossy.buffer.current(), lossy.encoding, SaveConsent::None, sink);
    ASSERT_FALSE(corrupt.has_value());
    EXPECT_EQ(corrupt.error().code(), ErrorCode::Corrupt);
    const LoadedText fine = load("abc", options(1252));
    const DocumentEncoding unsupported = fine.encoding.convertedTo(Encoding::legacy(936), false);
    auto missing = encodeForSaveTo(fine.buffer.current(), unsupported, SaveConsent::None, sink);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(calls, 0);
}

TEST(TextFile, SinkErrorPropagates) {
    for (const Encoding e : {kUtf8, kUtf16Le, kCp1252}) {
        const LoadedText l = load("abc", options(1252));
        const DocumentEncoding state = l.encoding.convertedTo(e, e.isUnicode());
        auto r =
            encodeForSaveTo(l.buffer.current(), state, SaveConsent::None, [](std::string_view) -> frost::Result<void> {
                return frost::fail(ErrorCode::IoError, "disk full");
            });
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code(), ErrorCode::IoError);
        ASSERT_FALSE(r.error().context().empty());
        EXPECT_EQ(r.error().context().back(), "while saving");
    }
}

TEST(TextFile, LosslessIffRoundTrip) {
    const auto seeds = frost::text::test::encodingSeeds(200);
    for (std::uint64_t seed = seeds.first; seed < seeds.last; ++seed) {
        SplitMix64 rng(seed * 31 + 7);
        const std::string bytes = frost::text::test::randomEncodingInput(rng, 4096);
        const std::uint16_t legacy = seed % 2 == 0 ? 1252 : 932;
        for (unsigned index = 0; index < 8; ++index) {
            const LoadOptions o = options(legacy, frost::text::test::overrideByIndex(index));
            const auto checked = frost::text::test::checkLoadAndSave(bytes, o, frost::text::platformCodecs(), false);
            ASSERT_TRUE(checked.has_value())
                << "override " << index << ": " << checked.error().describe() << "\ninput "
                << toHex(bytes.substr(0, 256)) << "\n"
                << frost::text::test::reproduceSeed("text_encoding_tests", "TextFile.LosslessIffRoundTrip", seed);
        }
    }
}

TEST(TextFile, MovesValidUtf8WithoutCopy) {
    std::string text = bigText(1 << 20);
    const char* original = text.data();
    auto l = decodeTextFile(std::move(text), options(1252));
    ASSERT_TRUE(l.has_value());
    EXPECT_EQ(l->buffer.current().chunkAt(ByteOffset{0}).data(), original) << "the UTF-8 path must not copy (B15)";
    std::string withBom = "\xEF\xBB\xBF" + bigText(1 << 20);
    const char* bomOriginal = withBom.data();
    auto b = decodeTextFile(std::move(withBom), options(1252));
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->buffer.current().chunkAt(ByteOffset{0}).data(), bomOriginal) << "the BOM is erased in place";
}

// --- FormatStatus ----------------------------------------------------------------------------

TEST(FormatStatus, PerFixture) {
    using frost::text::FormatStatus;
    const std::map<std::string, FormatStatus> expected = {
        {"lf-utf8", {"UTF-8", false, LossKind::None, DetectionSource::Utf8Valid, 90, "LF", false}},
        {"crlf-utf8", {"UTF-8", false, LossKind::None, DetectionSource::Utf8Valid, 90, "CRLF", false}},
        {"cr-only", {"UTF-8", false, LossKind::None, DetectionSource::AsciiOnly, 100, "CR", false}},
        {"mixed-eol", {"UTF-8", false, LossKind::None, DetectionSource::AsciiOnly, 100, "CRLF", true}},
        {"utf8-bom-crlf", {"UTF-8", true, LossKind::None, DetectionSource::Bom, 100, "CRLF", false}},
        {"utf8-bom-only", {"UTF-8", true, LossKind::None, DetectionSource::Bom, 100, "LF", false}},
        {"empty", {"UTF-8", false, LossKind::None, DetectionSource::Empty, 100, "LF", false}},
        {"utf16le-bom", {"UTF-16LE", true, LossKind::None, DetectionSource::Bom, 100, "CRLF", false}},
        {"utf16be-bom", {"UTF-16BE", true, LossKind::None, DetectionSource::Bom, 100, "CRLF", false}},
        {"utf16le-nobom", {"UTF-16LE", false, LossKind::None, DetectionSource::NulPattern, 95, "CRLF", false}},
        {"utf32le-bom", {"UTF-32LE", true, LossKind::None, DetectionSource::Bom, 100, "LF", false}},
        {"utf16le-bom-nul", {"UTF-16LE", true, LossKind::None, DetectionSource::Bom, 100, "LF", false}},
        {"cp1252", {"windows-1252", false, LossKind::None, DetectionSource::LegacyFallback, 40, "CRLF", false}},
        {"cp1252-c1", {"windows-1252", false, LossKind::None, DetectionSource::LegacyFallback, 40, "LF", false}},
        {"cp932", {"Shift_JIS", false, LossKind::None, DetectionSource::LegacyFallback, 64, "CRLF", false}},
        {"cp932-nec-dup",
         {"Shift_JIS", false, LossKind::NotRoundTrip, DetectionSource::LegacyFallback, 52, "LF", false}},
        {"invalid-utf8", {"UTF-8", false, LossKind::Replaced, DetectionSource::UserOverride, 100, "LF", false}},
        {"invalid-utf8-as-1252",
         {"windows-1252", false, LossKind::None, DetectionSource::LegacyFallback, 40, "LF", false}},
        {"utf16le-lone-surrogate", {"UTF-16LE", true, LossKind::Replaced, DetectionSource::Bom, 100, "LF", false}},
        {"utf16be-odd", {"UTF-16BE", true, LossKind::Replaced, DetectionSource::Bom, 100, "LF", false}},
        {"cp1253-undefined",
         {"windows-1253", false, LossKind::Replaced, DetectionSource::UserOverride, 100, "LF", false}},
    };
    ASSERT_EQ(expected.size(), frost::text::test::encodingFixtures().size());
    for (const auto& f : frost::text::test::encodingFixtures()) {
        const LoadedText l = load(f.bytes, frost::text::test::fixtureOptions(f));
        const auto it = expected.find(f.name);
        ASSERT_NE(it, expected.end()) << f.name;
        EXPECT_EQ(frost::text::formatStatus(l.encoding, l.eol), it->second) << f.name;
    }
}

TEST(FormatStatus, MixedFromStats) {
    DocumentEncoding state;
    EXPECT_FALSE(frost::text::formatStatus(state, {3, 0, 0}).mixedLineSeparators);
    EXPECT_TRUE(frost::text::formatStatus(state, {3, 1, 0}).mixedLineSeparators);
    EXPECT_EQ(frost::text::formatStatus(state, {}).lineSeparatorName, "LF");
}

TEST(FormatStatus, OverrideSource) {
    const LoadedText l = load("abc", options(1252, kCp932));
    const auto status = frost::text::formatStatus(l.encoding, l.eol);
    EXPECT_EQ(status.encodingName, "Shift_JIS");
    EXPECT_EQ(status.source, DetectionSource::UserOverride);
    EXPECT_EQ(status.confidence, 100);
    const auto converted = frost::text::formatStatus(l.encoding.convertedTo(kUtf8, true), l.eol);
    EXPECT_EQ(converted.encodingName, "UTF-8");
    EXPECT_TRUE(converted.bom);
    EXPECT_EQ(converted.source, DetectionSource::UserOverride);
}

TEST(FormatStatus, LossKindMarker) {
    const LoadedText replaced = load("a\xFF", options(1252, kUtf8));
    EXPECT_EQ(frost::text::formatStatus(replaced.encoding, replaced.eol).loss, LossKind::Replaced);
    const LoadedText nort = load(fromHex("87 90"), options(932, kCp932));
    EXPECT_EQ(frost::text::formatStatus(nort.encoding, nort.eol).loss, LossKind::NotRoundTrip);
    EXPECT_EQ(frost::text::formatStatus(nort.encoding.afterSave(), nort.eol).loss, LossKind::None);
}

} // namespace
