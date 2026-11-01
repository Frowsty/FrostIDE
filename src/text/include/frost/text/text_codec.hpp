// SPDX-License-Identifier: Apache-2.0
//
// The ITextCodec seam between frost::text and the platform's legacy code-page converters
// (MultiByteToWideChar/WideCharToMultiByte on Windows, glibc iconv on Linux), the process-wide
// platform provider, and the shared driver that gives every platform the same replacement and
// round-trip policy (ADR 0018, D3 and D4).
/*
 * The codecs are deliberately strict and narrow: they convert the longest prefix they can and
 * stop. Everything that decides what a user sees lives once, in decodeLegacy and encodeLegacy:
 * one U+FFFD per invalid unit with the WHATWG Shift_JIS rule for ASCII trails, the C1
 * normalisation to Windows' tables (CodePageInfo::windowsOnlyC1), the reversible-encode rule that
 * filters both platforms' one-way mappings, validation of codec output and error offsets. With
 * that normalisation, per-unit decoding, BMP encoding and valid-stream decoding agree exactly on
 * Windows and glibc for the 16 M1 pages (ADR 0018, probe F1-F3; the differential test pins it).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/text/encoding.hpp"
#include "frost/text/unicode_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace frost::text {

/*
 * ITextCodec: a strict converter between one legacy code page and UTF-8 (ADR 0018, D3).
 * Implementations:
 *   - Windows: MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS) and WideCharToMultiByte(cp,
 *     WC_NO_BEST_FIT_CHARS, ..., &usedDefault);
 *   - Linux: glibc iconv, without //TRANSLIT or //IGNORE;
 *   - tests: a table-driven double (tests/text/support/fake_codec.hpp).
 * The contract is deliberately narrow. The replacement policy, the C1 normalisation, the
 * reversible-encode rule, output validation and error offsets live once, in decodeLegacy and
 * encodeLegacy, so every platform behaves the same.
 *
 * decodePrefix converts the longest prefix of `bytes` made of complete characters the code page
 * defines, appends their UTF-8 to `utf8` and returns the prefix's length, which is a character
 * boundary. A result below bytes.size() means bytes[result] starts an undefined character, or is a
 * lead byte whose trail is missing at the end. It never substitutes a default character, never
 * applies best fit and never skips input.
 *
 * encodePrefix converts the longest prefix of `utf8` (well-formed) whose characters all have a
 * mapping, appends the bytes and returns the UTF-8 bytes consumed (a character boundary). The
 * mapping may be one-way; encodeLegacy filters those out.
 *
 * Both calls accept any input size: implementations chunk internally, and the Windows codec keeps
 * every API call at or below 64 KiB, inside int. Both start from the initial state (M1's pages are
 * stateless) and append to their output. On error the appended part is unspecified and the caller
 * discards it. Errors are platform failures only, never invalid input: Internal or Unsupported,
 * built with io::lastWin32Error or io::errnoError.
 *
 * Lifetime: info() refers to static storage for platform codecs and to the provider's storage for
 * test doubles, so a codec must not outlive its provider. Thread safety: an ITextCodec is
 * single-threaded; open one per thread. Ownership: std::unique_ptr from a provider.
 * Complexity: O(n) per call. On Windows a failing multi-character chunk is bisected, which costs
 * O(log gap) extra calls per invalid character, where gap is the distance since the previous one,
 * and one failing call per byte in a run of invalid bytes (ADR 0018, D17).
 */
class ITextCodec {
public:
    virtual ~ITextCodec() = default;

    // The code page this codec converts. O(1).
    [[nodiscard]] virtual const CodePageInfo& info() const noexcept = 0;
    // See above. O(n).
    [[nodiscard]] virtual Result<std::size_t> decodePrefix(std::string_view bytes, std::string& utf8) noexcept = 0;
    // See above. Precondition: `utf8` is well-formed. O(n).
    [[nodiscard]] virtual Result<std::size_t> encodePrefix(std::string_view utf8, std::string& bytes) noexcept = 0;

protected:
    ITextCodec() = default;
    ITextCodec(const ITextCodec&) = default;
    ITextCodec& operator=(const ITextCodec&) = default;
};

/*
 * Opens codecs. Interfaces are passed by reference and never deleted through the base (the
 * IFileSystem pattern: a protected non-virtual destructor). open() is thread-safe for the platform
 * provider; test doubles document their own rules.
 * Errors: Unsupported when `codePage` is not in supportedCodePages(), or when the platform lacks it
 * ("iconv has no converter for CP932; install the glibc gconv modules", for example
 * glibc-gconv-extra on Fedora).
 */
class ITextCodecProvider {
public:
    [[nodiscard]] virtual Result<std::unique_ptr<ITextCodec>> open(std::uint16_t codePage) noexcept = 0;

protected:
    constexpr ITextCodecProvider() noexcept = default;
    constexpr ITextCodecProvider(const ITextCodecProvider&) noexcept = default;
    constexpr ITextCodecProvider& operator=(const ITextCodecProvider&) noexcept = default;
    ~ITextCodecProvider() = default;
};

// The process-wide platform provider: a stateless, constant-initialised, trivially destructible
// static (no global constructor or exit-time destructor). Thread-safe. O(1).
[[nodiscard]] ITextCodecProvider& platformCodecs() noexcept;

/*
 * Decodes `bytes` through `codec` with the shared policy (D3, D4), appending UTF-8 to `utf8`.
 * Returns the bytes consumed: bytes.size() with Final. With More, a trailing lead byte whose trail
 * has not arrived is left unconsumed. Steps:
 *   1. Append decodePrefix's prefix, after validating the newly appended bytes with
 *      io::validUtf8Prefix (ill-formed codec output is Internal).
 *   2. At an undefined byte b: if b is not a lead byte and is in info().windowsOnlyC1, append
 *      U+00bb.
 *   3. Otherwise append one U+FFFD and skip one unit:
 *      - a lead byte followed by a byte >= 0x80 skips 2 bytes;
 *      - anything else skips 1 byte, so an ASCII trail after a lead is read again (the WHATWG
 *        Shift_JIS decoder rule). Example: 85 41 gives U+FFFD 'A', and 81 0A keeps the LF.
 *      The driver decides the unit from info().leadBytes, never from where the codec stopped.
 *   4. Repeat. If decodePrefix returns 0 and nothing applies, the driver still advances by one
 *      byte.
 * Stats as in DecodeStats (unicode_codec.hpp). The split invariance of appendUtf8Lossy holds.
 * Errors:
 *   - the codec's own errors, with the context "while decoding <name>";
 *   - Internal if the codec reports more bytes consumed than it was given, or appends ill-formed
 *     UTF-8.
 * Thread safety: as the codec (single-threaded). O(n + calls); calls is linear in n (ADR 0018,
 * D17).
 */
[[nodiscard]] Result<std::size_t> decodeLegacy(ITextCodec& codec, std::string_view bytes, std::string& utf8,
                                               DecodeStats& stats, InputEnd end = InputEnd::Final) noexcept;

/*
 * Encodes well-formed `utf8` through `codec`, accepting only reversible mappings: a character is
 * encodable iff its bytes decode back to it. U+0080..U+009F are encodable as their byte when that
 * byte is in windowsOnlyC1. Never substitutes a character and never applies best fit (D10).
 * Returns:
 *   - nullopt when everything was encoded (appended to `bytes`);
 *   - otherwise the offset in `utf8` of the first unencodable character, with `bytes` holding the
 *     encoding of everything before it.
 * Errors: the codec's errors, and Internal for contract breaches. Precondition: `utf8` is
 * well-formed (FROST_PRE is not evaluated; a codec may report Internal).
 * O(n + stops): per chunk of at most 4 KiB, one encodePrefix and one decodePrefix check, plus O(1)
 * calls per stop.
 */
[[nodiscard]] Result<std::optional<std::uint64_t>> encodeLegacy(ITextCodec& codec, std::string_view utf8,
                                                                std::string& bytes) noexcept;

/*
 * The default legacy candidate for detection (ADR 0018, D14):
 *   - Windows: the system locale's ANSI code page (GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT,
 *     LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER)), when it is in the table;
 *   - otherwise (0 for Unicode-only locales, a failure, a page outside the table), and always on
 *     Linux: 1252.
 * The UTF-8 manifest does not change the system ANSI code page, so this is never 65001. Computed
 * once (a function-local static integer). Thread-safe. O(1) after the first call.
 */
[[nodiscard]] std::uint16_t defaultLegacyCodePage() noexcept;

} // namespace frost::text
