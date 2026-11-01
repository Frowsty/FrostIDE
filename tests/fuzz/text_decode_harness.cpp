// SPDX-License-Identifier: Apache-2.0
//
// Fuzz harness for frost::text's encodings (ADR 0018): the lossy Unicode decoders against their
// independent oracles, and the whole load/save pipeline (detection, decodeBytes, decodeTextFile,
// the save guard and encodeForSave(To)) through tests/text/support/encoding_checks.hpp.
/*
 * Input format:
 *   byte 0   mode: bits 0-2 the override (0 none, 1 UTF-8, 2 UTF-16LE, 3 UTF-16BE, 4 UTF-32LE,
 *            5 UTF-32BE, 6 windows-1252, 7 Shift_JIS); bit 3 the codecs (set: a FakeCodecProvider
 *            serving one random DBCS page, seeded from byte 1, under each of 1252, 932, 1251 and
 *            437; clear: platformCodecs()); bits 4-5 the legacy code page (1252, 932, 1251, 437);
 *            bits 6-7 == 3: a buffer limit of 256 bytes, where OutOfRange is allowed;
 *   byte 1   the fake page's seed;
 *   byte 2   the detection sample: 16 + 4 * byte bytes for 0..254, kDetectSampleBytes for 255
 *            (with -max_len=16384 the sample is otherwise never cut);
 *   the rest the file; defaultSeparator is LF.
 * Checks, each failure an Internal error with a description:
 *   (a) appendUtf8Lossy equals oracleUtf8Lossy (text and stats) with well-formed output, and
 *       splitting the stream after 1 byte or at half the length (More, then Final) changes nothing;
 *   (b) appendUtf16Lossy and appendUtf32Lossy equal their oracles in both byte orders with the
 *       same split invariance, and encoding a decode without replacements gives the bytes back;
 *   (c) checkLoadAndSave (decodeTextFile succeeds, or fails with OutOfRange only in limit mode;
 *       the invariants, eol, decodeBytes agreement, the attempts and detection, the lossless
 *       implications, the sink, and AcceptLoss against firstUnencodable).
 * Shorter inputs than 3 bytes run with zero header bytes.
 */

#include "frost/io/utf.hpp"
#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/encoding_detection.hpp"
#include "frost/text/text_codec.hpp"
#include "frost/text/unicode_codec.hpp"

#include "fuzz/fuzz_harness.hpp"
#include "support/crash_dialogs.hpp"
#include "text/support/encoding_checks.hpp"
#include "text/support/encoding_oracle.hpp"
#include "text/support/fake_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace frost::fuzz {
namespace {

using text::DecodeStats;
using text::Endian;
using text::InputEnd;
using text::test::OracleDecode;

std::unexpected<Error> mismatch(std::string what) { return fail(ErrorCode::Internal, std::move(what)); }

enum class Form : std::uint8_t { Utf8, Utf16, Utf32 };

std::size_t decodeOnce(Form form, Endian order, std::string_view bytes, std::string& out, DecodeStats& stats,
                       InputEnd end) {
    switch (form) {
    case Form::Utf16: return text::appendUtf16Lossy(bytes, order, out, stats, end);
    case Form::Utf32: return text::appendUtf32Lossy(bytes, order, out, stats, end);
    case Form::Utf8: break;
    }
    return text::appendUtf8Lossy(bytes, out, stats, end);
}

struct Stream {
    std::string text;
    DecodeStats stats;
};

// Decodes `bytes` split at `cut` with More, then the rest with Final.
Result<Stream> decodeSplit(Form form, Endian order, std::string_view bytes, std::size_t cut) {
    Stream s;
    std::string pending(bytes.substr(0, cut));
    const std::size_t first = decodeOnce(form, order, pending, s.text, s.stats, InputEnd::More);
    if (first > pending.size() || pending.size() - first > 3) {
        return mismatch(std::format("More consumed {} of {} bytes", first, pending.size()));
    }
    pending.erase(0, first);
    pending.append(bytes.substr(cut));
    if (decodeOnce(form, order, pending, s.text, s.stats, InputEnd::Final) != pending.size()) {
        return mismatch("Final left bytes unconsumed");
    }
    return s;
}

Result<void> checkUnicode(Form form, Endian order, std::string_view bytes) {
    const char* name = form == Form::Utf8 ? "UTF-8" : form == Form::Utf16 ? "UTF-16" : "UTF-32";
    Stream whole;
    if (decodeOnce(form, order, bytes, whole.text, whole.stats, InputEnd::Final) != bytes.size()) {
        return mismatch(std::format("{}: Final left bytes unconsumed", name));
    }
    const bool big = order == Endian::Big;
    const OracleDecode want = form == Form::Utf8    ? text::test::oracleUtf8Lossy(bytes)
                              : form == Form::Utf16 ? text::test::oracleUtf16Lossy(bytes, big)
                                                    : text::test::oracleUtf32Lossy(bytes, big);
    if (whole.text != want.utf8 || whole.stats.replacements != want.replacements ||
        whole.stats.firstInvalidInput != want.firstInvalid ||
        whole.stats.firstReplacementOutput != want.firstReplacementOutput || whole.stats.inputBytes != bytes.size() ||
        whole.stats.outputBytes != whole.text.size()) {
        return mismatch(std::format("{} (big endian {}) differs from its oracle", name, big));
    }
    if (!io::isValidUtf8(whole.text)) {
        return mismatch(std::format("{} produced ill-formed UTF-8", name));
    }
    for (const std::size_t cut : {std::min<std::size_t>(1, bytes.size()), bytes.size() / 2}) {
        auto split = decodeSplit(form, order, bytes, cut);
        if (!split) {
            return std::unexpected(std::move(split).error());
        }
        if (split->text != whole.text || !(split->stats == whole.stats)) {
            return mismatch(std::format("{} split at {} differs from one call", name, cut));
        }
    }
    if (form != Form::Utf8 && whole.stats.replacements == 0) {
        std::string encoded;
        if (form == Form::Utf16) {
            text::appendUtf16(whole.text, order, encoded);
        } else {
            text::appendUtf32(whole.text, order, encoded);
        }
        if (encoded != bytes) {
            return mismatch(std::format("{} encode after a valid decode is not the identity", name));
        }
    }
    return {};
}

// One fake DBCS page per seed byte, built on first use.
std::shared_ptr<const text::test::FakeCodePage> fakePage(std::uint8_t seed) {
    static std::array<std::shared_ptr<const text::test::FakeCodePage>, 256> pages;
    if (!pages[seed]) {
        sched::detail::SplitMix64 rng(0xF00D + seed);
        pages[seed] = std::make_shared<const text::test::FakeCodePage>(text::test::randomDbcs(rng));
    }
    return pages[seed];
}

} // namespace

void initialize() {
    frost::test::disableCrashDialogs();
    (void)text::test::oracleIsProperPrefix("\xE0\xA0"); // Builds the oracle's prefix set up front.
}

Result<void> runInput(std::span<const std::uint8_t> data) {
    const std::uint8_t mode = !data.empty() ? data[0] : 0;
    const std::uint8_t seed = data.size() > 1 ? data[1] : 0;
    const std::uint8_t sample = data.size() > 2 ? data[2] : 0;
    const std::span<const std::uint8_t> rest = data.size() > 3 ? data.subspan(3) : std::span<const std::uint8_t>();
    const std::string file(rest.begin(), rest.end());

    // (a) and (b).
    if (auto ok = checkUnicode(Form::Utf8, Endian::Little, file); !ok) {
        return ok;
    }
    for (const Endian order : {Endian::Little, Endian::Big}) {
        if (auto ok = checkUnicode(Form::Utf16, order, file); !ok) {
            return ok;
        }
        if (auto ok = checkUnicode(Form::Utf32, order, file); !ok) {
            return ok;
        }
    }

    // (c).
    constexpr std::uint16_t kLegacy[] = {1252, 932, 1251, 437};
    text::LoadOptions options;
    options.encoding = text::test::overrideByIndex(mode & 0x7u);
    options.detect.legacyCodePage = kLegacy[(mode >> 4) & 0x3u];
    options.detect.defaultEncoding = text::Encoding::utf8();
    options.detect.sampleBytes = sample == 255 ? text::kDetectSampleBytes : 16u + 4u * sample;
    options.defaultSeparator = text::LineSeparator::Lf;
    const bool limitMode = (mode >> 6) == 3;
    if (limitMode) {
        options.buffer.maxBufferBytes = 256;
    }
    if ((mode & 0x8u) != 0) {
        text::test::FakeCodecProvider fake;
        const auto page = fakePage(seed);
        for (const std::uint16_t id : kLegacy) {
            fake.serve(page, id);
        }
        return text::test::checkLoadAndSave(file, options, fake, limitMode);
    }
    return text::test::checkLoadAndSave(file, options, text::platformCodecs(), limitMode);
}

} // namespace frost::fuzz
