// SPDX-License-Identifier: Apache-2.0
//
// Encoding detection for frost::text's loader (ADR 0018, D7 and D8): a deterministic heuristic
// that ranks the encodings a file may be in, with a confidence value per candidate. The loader
// (text_file.hpp) tries the candidates in order; a user override skips detection.
/*
 * The heuristic, in order (constants and their reasons in ADR 0018, D7):
 *   1. a byte-order mark decides (UTF-32LE's FF FE 00 00 adds UTF-16LE as a second candidate);
 *   2. an empty file takes the default encoding;
 *   3. a sample whose zero bytes sit in one byte of most UTF-16 units is UTF-16 without a BOM;
 *   4. a sample that is valid UTF-8 is UTF-8, or the default encoding when it is pure ASCII, with
 *      a fallback chain [UTF-8, the legacy code page, windows-1252] for bytes after the sample;
 *   5. otherwise the legacy code page and windows-1252, ranked by whether they decode the sample
 *      without errors, around a lossy UTF-8 candidate.
 * Only the BOM and the first sampleBytes are read. Confidence values are documented heuristic
 * constants, not probabilities.
 */
#pragma once

#include "frost/text/encoding.hpp"
#include "frost/text/text_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace frost::text {

// The bytes inspected when there is no BOM. Budget: B15.
inline constexpr std::size_t kDetectSampleBytes = 64 * 1024;

// Why a candidate was proposed. The enumerator values are stable and append-only (persisted by
// name in m1/save-manager's journal; tests pin them).
enum class DetectionSource : std::uint8_t {
    Bom,            // A byte-order mark (confidence 100).
    UserOverride,   // LoadOptions::encoding or DocumentEncoding::convertedTo (100).
    Empty,          // No bytes: the default encoding (100).
    AsciiOnly,      // No byte >= 0x80 in the sample: the default encoding (100, or 90 when the sample was cut).
    NulPattern,     // UTF-16 without a BOM, from where the zero bytes are.
    Utf8Valid,      // The sample is valid UTF-8 with multi-byte sequences (or UTF-8 as a fallback).
    LegacyFallback, // A legacy code page, as a fallback or because the sample is not valid UTF-8.
    Lossy,          // No candidate decodes the sample without errors.
};

// Persistence names: "bom", "user-override", "empty", "ascii-only", "nul-pattern", "utf8-valid",
// "legacy-fallback", "lossy"; "" for a value outside the enumeration. Static storage. O(1).
[[nodiscard]] std::string_view detectionSourceName(DetectionSource source) noexcept;
// The exact (case-sensitive) inverse of detectionSourceName; nullopt for anything else. O(1).
[[nodiscard]] std::optional<DetectionSource> findDetectionSource(std::string_view name) noexcept;

// One candidate encoding. A plain value.
struct EncodingGuess {
    Encoding encoding;
    bool bom = false;
    std::uint8_t confidence = 0; // 0..100, documented heuristic constants (ADR 0018, D7), not a probability.
    DetectionSource source = DetectionSource::Empty;

    friend constexpr bool operator==(const EncodingGuess&, const EncodingGuess&) noexcept = default;
};

// Detection options. Defaults suit production; tests set legacyCodePage explicitly.
struct DetectOptions {
    // For empty and ASCII-only samples. It usually comes from settings, so it is not a
    // precondition: a value that is not ASCII-compatible (UTF-16/32), or a code page outside the
    // table or reported Unsupported by `codecs`, is replaced by UTF-8.
    Encoding defaultEncoding = Encoding::utf8();
    // The first legacy candidate. A value outside the table, or one `codecs` reports Unsupported
    // for, is skipped, and 1252 remains.
    std::uint16_t legacyCodePage = defaultLegacyCodePage();
    // Bytes inspected without a BOM. FROST_PRE: >= 16 (a test and fuzz knob).
    std::size_t sampleBytes = kDetectSampleBytes;
};

// The ranked candidates of one detection.
struct EncodingDetection {
    // Ranked, best first, never empty, no duplicate encodings, at most 4. UTF-8 appears in every
    // list except three: a BOM's (its form, plus UTF-16LE after FF FE 00 00), an empty file's
    // (only the default encoding, which may be a code page) and a NUL pattern's (only UTF-16).
    std::vector<EncodingGuess> candidates;
    // The sample holds a 0x00 byte and the top candidate is not UTF-16 or UTF-32.
    bool binaryLikely = false;
};

/*
 * Ranks the encodings `bytes` may be in (the algorithm above; ADR 0018, D7). It reads at most the
 * BOM and the first sampleBytes; it also uses bytes.size() (the BOM rule, and whether the sample
 * was cut), so pass a view of the whole file. That suits mapped views (m1/large-file). Code pages
 * outside the table, or that `codecs` reports Unsupported, are left out; a page whose open fails
 * otherwise stays a candidate (ranked lossy when the sample needed it), so the loader reports the
 * failure if it reaches the page. Deterministic. Precondition: options.sampleBytes >= 16.
 * Thread safety: as safe as `codecs` (platformCodecs() is thread-safe).
 * Cost: O(min(n, sampleBytes)) plus one strict decode of the sample per legacy candidate (at most
 * 2) and at most 3 codec opens, one per distinct page among the default encoding, the legacy page
 * and 1252 (none when a BOM decides). Budget: B15.
 */
[[nodiscard]] EncodingDetection detectEncoding(std::string_view bytes, const DetectOptions& options,
                                               ITextCodecProvider& codecs) noexcept;

} // namespace frost::text
