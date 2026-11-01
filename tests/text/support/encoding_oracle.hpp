// SPDX-License-Identifier: Apache-2.0
//
// Independent oracles for frost::text's encoding tests and the text_decode fuzz target (ADR 0018,
// section "Tests"). They share no code with src/text or frost::io beyond value types: each follows
// a definition directly, unit by unit, so that a test comparing the production codecs with them is
// a real differential and not a consistency check.
/*
 *   - oracleUtf8Lossy applies the definition of a maximal subpart (Unicode 16.0, section 3.9): at
 *     an offset that does not start a well-formed sequence, the longest subsequence that is an
 *     initial subsequence of some well-formed sequence, or one byte. Well-formed sequences are
 *     recognised with text::test::oracleDecode (bit patterns, not table 3-7), and the proper
 *     prefixes come from a set built once by encoding every scalar value with the oracle's own
 *     bit-pattern encoder (checked against oracleDecode).
 *   - oracleUtf16Lossy and oracleUtf32Lossy follow the surrogate and range definitions with a
 *     unit-by-unit state machine; oracleEncodeUtf16/32 are the inverse bit layouts.
 *   - oracleEolStats is a linear scan.
 *   - legacyOracle decodes a FakeCodePage by table lookup with the documented driver policy
 *     (ADR 0018, D3 and D4), reimplemented here; oracleEncodeLegacy is the reversible-encode rule.
 *   - fnv1a64 is FNV-1a (64-bit), from the public-domain description at
 *     http://www.isthe.com/chongo/tech/comp/fnv/ (offset basis 14695981039346656037, prime
 *     1099511628211).
 * Pure functions; thread-safe (the prefix set is a function-local static built on first use).
 */
#pragma once

#include "frost/text/line_separator.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace frost::text::test {

class FakeCodePage;

// The result of an oracle decode of a whole input (InputEnd::Final semantics).
struct OracleDecode {
    std::string utf8;
    std::uint64_t replacements = 0;
    std::optional<std::uint64_t> firstInvalid;           // Input offset of the first replaced unit.
    std::optional<std::uint64_t> firstReplacementOutput; // Output offset of the first U+FFFD.

    friend bool operator==(const OracleDecode&, const OracleDecode&) = default;
};

// Appends the UTF-8 bit pattern of `scalar` (any value up to 0x10FFFF, surrogates included).
void oracleAppendUtf8(char32_t scalar, std::string& out);
// True when `prefix` (1..3 bytes) is a proper prefix of some well-formed UTF-8 sequence.
[[nodiscard]] bool oracleIsProperPrefix(std::string_view prefix);

[[nodiscard]] OracleDecode oracleUtf8Lossy(std::string_view bytes);
[[nodiscard]] OracleDecode oracleUtf16Lossy(std::string_view bytes, bool bigEndian);
[[nodiscard]] OracleDecode oracleUtf32Lossy(std::string_view bytes, bool bigEndian);
// The UTF-16/UTF-32 bytes of well-formed `utf8`.
[[nodiscard]] std::string oracleEncodeUtf16(std::string_view utf8, bool bigEndian);
[[nodiscard]] std::string oracleEncodeUtf32(std::string_view utf8, bool bigEndian);

// Line-break counts by a linear scan (LSP model: CRLF once, lone CR, lone LF).
[[nodiscard]] EolStats oracleEolStats(std::string_view text);

// The documented driver policy over a table (D3, D4): table units, then the windowsOnlyC1 rule
// for undefined non-lead bytes, then one U+FFFD per invalid unit (a lead byte with a byte >= 0x80
// after it is a 2-byte unit; anything else one byte).
[[nodiscard]] OracleDecode legacyOracle(const FakeCodePage& page, std::string_view bytes);

/*
 * The reversible-encode rule (D4) over a table: each character's bytes must decode back to it as
 * one unit; U+0080..U+009F map to their byte when it is a windowsOnlyC1 non-lead byte the table
 * does not encode reversibly. Returns the bytes before the first unencodable character and that
 * character's UTF-8 offset (nullopt when everything encoded).
 */
struct OracleEncode {
    std::string bytes;
    std::optional<std::uint64_t> stop;
};
[[nodiscard]] OracleEncode oracleEncodeLegacy(const FakeCodePage& page, std::string_view utf8);

inline constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
// FNV-1a 64 of `bytes`, continuing from `hash`.
[[nodiscard]] std::uint64_t fnv1a64(std::string_view bytes, std::uint64_t hash = kFnvOffsetBasis);

// UTF-8 from a u8 literal (char8_t storage to char).
[[nodiscard]] std::string fromU8(std::u8string_view text);
// Bytes from a hex string such as "ef bb bf 41"; whitespace separates bytes.
[[nodiscard]] std::string fromHex(std::string_view hex);
// "ef bb bf" for bytes.
[[nodiscard]] std::string toHex(std::string_view bytes);

} // namespace frost::text::test
