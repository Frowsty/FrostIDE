// SPDX-License-Identifier: Apache-2.0
//
// Lossy Unicode decoders (UTF-8, UTF-16, UTF-32 to well-formed UTF-8) and exact encoders (UTF-8
// to UTF-16 and UTF-32) for frost::text's load and save pipeline (ADR 0018, D5 and D16).
// Portable code with no OS calls; every function is pure and thread-safe.
/*
 * Decoding never fails on ill-formed input: each ill-formed piece becomes one U+FFFD and is
 * counted in a DecodeStats, which the loader turns into a "lossy" mark on the document so that
 * saving cannot silently corrupt the file (text_file.hpp). The UTF-8 rule is the Unicode
 * Standard's "U+FFFD Substitution of Maximal Subparts" (Unicode 16.0, chapter 3, section 3.9,
 * tables 3-8 to 3-11), the same practice as the WHATWG Encoding Standard's UTF-8 decoder.
 *
 * Streaming (D16): every decoder takes an InputEnd. With More, an incomplete sequence at the end
 * of the input that could still become valid is left unconsumed and the call returns the bytes it
 * consumed; the caller passes the rest again at the start of the next call. Decoding a stream in
 * pieces with More and a last Final call gives exactly the output and stats of one Final call over
 * the whole stream (split invariance). Blockwise file loads, detection's cut sample and, in M4,
 * child-process output use this.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace frost::text {

// The byte order of UTF-16 and UTF-32 units.
enum class Endian : std::uint8_t { Little, Big };

// Whether more input follows the bytes of a decode call (D16).
enum class InputEnd : std::uint8_t {
    Final, // The input ends here: an incomplete trailing sequence is ill-formed and replaced.
    More,  // More input follows: an incomplete trailing sequence that could still become valid is left unconsumed.
};

// U+FFFD REPLACEMENT CHARACTER in UTF-8.
inline constexpr std::string_view kReplacementUtf8 = "\xEF\xBF\xBD";

/*
 * What a lossy decode of one input stream consumed and replaced. Each decode call adds to it, so
 * one object follows one stream (a file decoded in blocks, a child process's output):
 *   - inputBytes and outputBytes add the bytes each call consumed and appended;
 *   - replacements adds up;
 *   - firstInvalidInput: set once, to the stream offset of the first ill-formed subsequence
 *     (inputBytes before the call plus its offset in that call's input);
 *   - firstReplacementOutput: set at the same time, to the stream offset of that U+FFFD in the
 *     concatenated output (outputBytes before the call plus its offset among the bytes the call
 *     appended). It does not depend on what `out` held before.
 * Valid U+FFFD characters in the input are not counted. Use one object per stream. A plain value.
 */
struct DecodeStats {
    std::uint64_t inputBytes = 0;
    std::uint64_t outputBytes = 0;
    std::uint64_t replacements = 0;
    std::optional<std::uint64_t> firstInvalidInput;
    std::optional<std::uint64_t> firstReplacementOutput;

    friend constexpr bool operator==(const DecodeStats&, const DecodeStats&) noexcept = default;
};

/*
 * Appends the UTF-8 decoding of `bytes` to `out`, replacing each maximal subpart of an ill-formed
 * subsequence with one U+FFFD (Unicode Standard, section 3.9, "U+FFFD Substitution of Maximal
 * Subparts", tables 3-8 to 3-11). Well-formed input is appended unchanged, byte for byte. The
 * output is always well-formed.
 * Returns the bytes consumed. With Final that is bytes.size(). With More it may be up to 3 less,
 * when the input ends in a proper prefix of a well-formed sequence; the caller passes those bytes
 * again at the start of the next call. Split invariance holds (see above).
 * O(n), with an ASCII fast path: valid runs are found with io::validUtf8Prefix and appended in one
 * append. Budget: B15 (the valid-run path).
 */
[[nodiscard]] std::size_t appendUtf8Lossy(std::string_view bytes, std::string& out, DecodeStats& stats,
                                          InputEnd end = InputEnd::Final) noexcept;

/*
 * Appends the UTF-8 of UTF-16 `bytes` in the given byte order. Each unpaired surrogate (a high
 * surrogate not followed by a low one, or a lone low surrogate) gives one U+FFFD and consumes one
 * unit; with Final, an odd trailing byte and a trailing high surrogate each give one U+FFFD. With
 * More, a trailing odd byte or a trailing high surrogate (it may pair with the next unit) is left
 * unconsumed. The same return value and split invariance as appendUtf8Lossy. No BOM handling (the
 * caller strips it). O(n).
 */
[[nodiscard]] std::size_t appendUtf16Lossy(std::string_view bytes, Endian order, std::string& out, DecodeStats& stats,
                                           InputEnd end = InputEnd::Final) noexcept;
/*
 * Appends the UTF-8 of UTF-32 `bytes` in the given byte order. Each unit above U+10FFFF or in
 * D800..DFFF gives one U+FFFD; with Final, 1-3 trailing bytes give one U+FFFD, and with More they
 * are left unconsumed. The same return value and split invariance as appendUtf8Lossy. No BOM
 * handling. O(n).
 */
[[nodiscard]] std::size_t appendUtf32Lossy(std::string_view bytes, Endian order, std::string& out, DecodeStats& stats,
                                           InputEnd end = InputEnd::Final) noexcept;

// Appends the UTF-16 encoding of `utf8` in the given byte order. Precondition: well-formed UTF-8
// (snapshot chunks are); a decode failure fails a FROST_ASSERT. Cannot fail. O(n).
void appendUtf16(std::string_view utf8, Endian order, std::string& out) noexcept;
// Appends the UTF-32 encoding of `utf8`. Same precondition and cost as appendUtf16.
void appendUtf32(std::string_view utf8, Endian order, std::string& out) noexcept;

} // namespace frost::text
