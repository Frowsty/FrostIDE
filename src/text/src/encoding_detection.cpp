// SPDX-License-Identifier: Apache-2.0
//
// The encoding-detection heuristic (frost/text/encoding_detection.hpp; ADR 0018, D7 and D8).
/*
 * Notation: sample = bytes[0, min(n, sampleBytes)); cut = the sample is shorter than the file;
 * tail = More when cut (a sequence the cut splits is not counted as invalid), else Final.
 *
 * Every confidence value is a constant of the block below, each explained in ADR 0018 (D7). They
 * rank candidates and tell the user how sure the guess is; they are not probabilities.
 */

#include "frost/text/encoding_detection.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/utf.hpp"

#include "unicode_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace frost::text {
namespace {

// Confidence constants (ADR 0018, D7).
constexpr std::uint8_t kBomConfidence = 100;
constexpr std::uint8_t kUtf16AfterUtf32BomConfidence = 90; // FF FE 00 00: UTF-16LE + U+0000 is possible.
constexpr std::uint8_t kEmptyConfidence = 100;
constexpr std::uint8_t kAsciiConfidence = 100;
constexpr std::uint8_t kAsciiCutConfidence = 90; // Bytes after the sample may be non-ASCII.
constexpr std::uint8_t kNulPatternBase = 60;     // UTF-16 by zero bytes: 60 + 35 * zHi / h, at most 95.
constexpr std::uint8_t kNulPatternSpan = 35;
constexpr std::uint8_t kNulPatternMax = 95;
constexpr std::uint8_t kUtf8Base = 80; // Valid UTF-8: 80 + 5 * min(m, 3) for m multi-byte sequences.
constexpr std::uint8_t kUtf8PerSequence = 5;
constexpr std::uint32_t kUtf8MaxSequences = 3;
constexpr std::uint8_t kFallbackConfidence = 30;  // The chain after a valid sample.
constexpr std::uint8_t kCleanSbcsConfidence = 40; // A single-byte page decodes an invalid-UTF-8 sample cleanly.
constexpr std::uint8_t kCleanDbcsBase = 50;       // A double-byte page: 50 + min(20, d) for d pairs.
constexpr std::uint32_t kCleanDbcsMaxPairs = 20;
constexpr std::uint8_t kLossyConfidence = 10; // Decodes the sample only with replacements.
// NUL pattern thresholds: zHi >= ceil(0.4 h) and zLo <= floor(0.05 h), in integer arithmetic.
constexpr std::size_t kNulHiNumerator = 2; // 0.4 = 2/5
constexpr std::size_t kNulHiDenominator = 5;
constexpr std::size_t kNulLoDenominator = 20; // 0.05 = 1/20

constexpr std::uint16_t kWindows1252 = 1252;

bool canOpen(ITextCodecProvider& codecs, std::uint16_t codePage) noexcept {
    return findCodePage(codePage) != nullptr && codecs.open(codePage).has_value();
}

bool isUtf16Or32(Encoding encoding) noexcept {
    return encoding.form == EncodingForm::Utf16Le || encoding.form == EncodingForm::Utf16Be ||
           encoding.form == EncodingForm::Utf32Le || encoding.form == EncodingForm::Utf32Be;
}

void add(std::vector<EncodingGuess>& candidates, EncodingGuess guess) {
    const bool present = std::any_of(candidates.begin(), candidates.end(),
                                     [&](const EncodingGuess& g) { return g.encoding == guess.encoding; });
    if (!present) {
        candidates.push_back(guess);
    }
}

// The NUL-pattern test for one byte order; the confidence, or 0 when the sample is not UTF-16.
std::uint8_t nulPattern(std::string_view sample, Endian order, InputEnd tail) noexcept {
    const std::size_t h = sample.size() / 2;
    std::size_t zHi = 0; // Units whose high byte is 0 (ASCII and Latin-1 text).
    std::size_t zLo = 0; // Units whose low byte is 0.
    for (std::size_t u = 0; u < h; ++u) {
        const bool even = sample[2 * u] == '\0';
        const bool odd = sample[2 * u + 1] == '\0';
        const bool high = order == Endian::Little ? odd : even;
        const bool low = order == Endian::Little ? even : odd;
        zHi += high ? 1 : 0;
        zLo += low ? 1 : 0;
    }
    const std::size_t needHi = (kNulHiNumerator * h + kNulHiDenominator - 1) / kNulHiDenominator; // ceil(0.4 h)
    const std::size_t allowLo = h / kNulLoDenominator;                                            // floor(0.05 h)
    if (h == 0 || zHi < needHi || zLo > allowLo) {
        return 0;
    }
    std::string scratch;
    scratch.reserve(sample.size() * 3 / 2 + 4);
    DecodeStats stats;
    (void)appendUtf16Lossy(sample, order, scratch, stats, tail);
    if (stats.replacements != 0) {
        return 0;
    }
    const std::size_t confidence = kNulPatternBase + kNulPatternSpan * zHi / h;
    return static_cast<std::uint8_t>(std::min<std::size_t>(confidence, kNulPatternMax));
}

// The number of characters that start with a lead byte in the consumed part of a clean decode.
std::uint32_t countPairs(const CodePageInfo& info, std::string_view consumed) noexcept {
    std::uint32_t pairs = 0;
    for (std::size_t i = 0; i < consumed.size();) {
        if (isLeadByte(info, detail::octetAt(consumed, i)) && i + 1 < consumed.size()) {
            ++pairs;
            i += 2;
        } else {
            i += 1;
        }
    }
    return pairs;
}

} // namespace

std::string_view detectionSourceName(DetectionSource source) noexcept {
    switch (source) {
    case DetectionSource::Bom: return "bom";
    case DetectionSource::UserOverride: return "user-override";
    case DetectionSource::Empty: return "empty";
    case DetectionSource::AsciiOnly: return "ascii-only";
    case DetectionSource::NulPattern: return "nul-pattern";
    case DetectionSource::Utf8Valid: return "utf8-valid";
    case DetectionSource::LegacyFallback: return "legacy-fallback";
    case DetectionSource::Lossy: return "lossy";
    }
    return {};
}

std::optional<DetectionSource> findDetectionSource(std::string_view name) noexcept {
    for (std::uint8_t v = 0; v <= static_cast<std::uint8_t>(DetectionSource::Lossy); ++v) {
        const auto source = static_cast<DetectionSource>(v);
        if (!name.empty() && name == detectionSourceName(source)) {
            return source;
        }
    }
    return std::nullopt;
}

EncodingDetection detectEncoding(std::string_view bytes, const DetectOptions& options,
                                 ITextCodecProvider& codecs) noexcept {
    FROST_PRE(options.sampleBytes >= 16);
    EncodingDetection result;
    std::vector<EncodingGuess>& candidates = result.candidates;
    candidates.reserve(4);
    const std::string_view sample = bytes.substr(0, std::min(bytes.size(), options.sampleBytes));
    const bool cut = sample.size() < bytes.size();
    const InputEnd tail = cut ? InputEnd::More : InputEnd::Final;
    const auto finish = [&]() {
        result.binaryLikely = sample.find('\0') != std::string_view::npos && !isUtf16Or32(candidates.front().encoding);
        return result;
    };

    // 1. A byte-order mark decides (D8).
    if (const std::optional<EncodingForm> bom = sniffBom(bytes)) {
        candidates.push_back({Encoding::unicode(*bom), true, kBomConfidence, DetectionSource::Bom});
        if (*bom == EncodingForm::Utf32Le) {
            candidates.push_back(
                {Encoding::unicode(EncodingForm::Utf16Le), true, kUtf16AfterUtf32BomConfidence, DetectionSource::Bom});
        }
        return finish();
    }

    // 0. Effective defaults: values from settings are never preconditions (D7). Codecs are opened
    // only after the BOM check, so a BOM costs no codec open.
    Encoding def = options.defaultEncoding;
    if (!isAsciiCompatible(def) || (def.form == EncodingForm::CodePage && !canOpen(codecs, def.codePage))) {
        def = Encoding::utf8();
    }

    // 2. Empty.
    if (bytes.empty()) {
        candidates.push_back({def, false, kEmptyConfidence, DetectionSource::Empty});
        return finish();
    }

    // 3. UTF-16 without a BOM, from where the zero bytes are. No fallback chain, so a UTF-16 file
    // never becomes UTF-8 with NULs.
    if (sample.size() >= 4) {
        for (const Endian order : {Endian::Little, Endian::Big}) {
            if (const std::uint8_t confidence = nulPattern(sample, order, tail); confidence != 0) {
                const EncodingForm form = order == Endian::Little ? EncodingForm::Utf16Le : EncodingForm::Utf16Be;
                candidates.push_back({Encoding::unicode(form), false, confidence, DetectionSource::NulPattern});
                return finish();
            }
        }
    }

    const std::optional<std::uint16_t> legacy =
        canOpen(codecs, options.legacyCodePage) ? std::optional<std::uint16_t>(options.legacyCodePage) : std::nullopt;
    const bool have1252 = canOpen(codecs, kWindows1252);

    // 4. Valid UTF-8 (a sequence the cut splits counts as valid).
    const std::size_t valid = io::validUtf8Prefix(sample);
    const bool utf8Valid = valid == sample.size() || (cut && detail::isProperUtf8Prefix(sample.substr(valid)));
    if (utf8Valid) {
        std::uint32_t multiByte = 0;
        bool ascii = true;
        for (const char c : sample) {
            const auto b = static_cast<std::uint8_t>(c);
            ascii = ascii && b < 0x80;
            multiByte += b >= 0xC2 ? 1u : 0u;
        }
        if (ascii) {
            candidates.push_back(
                {def, false, cut ? kAsciiCutConfidence : kAsciiConfidence, DetectionSource::AsciiOnly});
        } else {
            const auto confidence =
                static_cast<std::uint8_t>(kUtf8Base + kUtf8PerSequence * std::min(multiByte, kUtf8MaxSequences));
            candidates.push_back({Encoding::utf8(), false, confidence, DetectionSource::Utf8Valid});
        }
        // The fallback chain lets the loader recover when bytes after the sample are not UTF-8.
        add(candidates, {Encoding::utf8(), false, kFallbackConfidence, DetectionSource::Utf8Valid});
        if (legacy) {
            add(candidates, {Encoding::legacy(*legacy), false, kFallbackConfidence, DetectionSource::LegacyFallback});
        }
        if (have1252) {
            add(candidates,
                {Encoding::legacy(kWindows1252), false, kFallbackConfidence, DetectionSource::LegacyFallback});
        }
        return finish();
    }

    // 5. Not UTF-8: rank the legacy candidates by whether they decode the sample cleanly.
    std::vector<EncodingGuess> clean;
    std::vector<EncodingGuess> dirty;
    std::uint16_t pages[2] = {0, 0};
    std::size_t pageCount = 0;
    if (legacy) {
        pages[pageCount++] = *legacy;
    }
    if (have1252 && (!legacy || *legacy != kWindows1252)) {
        pages[pageCount++] = kWindows1252;
    }
    std::string scratch;
    for (std::size_t p = 0; p < pageCount; ++p) {
        auto codec = codecs.open(pages[p]);
        if (!codec) {
            continue; // canOpen succeeded a moment ago; a provider that fails now is skipped too.
        }
        scratch.clear();
        DecodeStats stats;
        const Result<std::size_t> consumed = decodeLegacy(**codec, sample, scratch, stats, tail);
        const Encoding encoding = Encoding::legacy(pages[p]);
        if (consumed && stats.replacements == 0) {
            const CodePageInfo& info = (*codec)->info();
            const std::uint8_t confidence =
                info.isDoubleByte()
                    ? static_cast<std::uint8_t>(
                          kCleanDbcsBase + std::min(countPairs(info, sample.substr(0, *consumed)), kCleanDbcsMaxPairs))
                    : kCleanSbcsConfidence;
            clean.push_back({encoding, false, confidence, DetectionSource::LegacyFallback});
        } else {
            // A codec failure ranks the page as lossy; the loader reports the error if it gets there.
            dirty.push_back({encoding, false, kLossyConfidence, DetectionSource::Lossy});
        }
    }
    candidates.insert(candidates.end(), clean.begin(), clean.end());
    candidates.push_back({Encoding::utf8(), false, kLossyConfidence, DetectionSource::Lossy});
    candidates.insert(candidates.end(), dirty.begin(), dirty.end());
    return finish();
}

} // namespace frost::text
