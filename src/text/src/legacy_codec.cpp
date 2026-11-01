// SPDX-License-Identifier: Apache-2.0
//
// The shared legacy code-page driver (frost/text/text_codec.hpp; ADR 0018, D3, D4 and D17):
// decodeLegacy turns a strict codec's prefixes into a lossy decode with one policy for every
// platform, and encodeLegacy accepts only reversible mappings.
/*
 * Replacement policy (D3). At a byte the codec does not decode, the driver decides the unit from
 * CodePageInfo::leadBytes: a lead byte followed by a byte >= 0x80 is one invalid 2-byte unit; any
 * other byte is one invalid byte, so an ASCII byte after a lead byte is read again. This is the
 * ASCII-trail rule of the WHATWG Encoding Standard's Shift_JIS decoder
 * (https://encoding.spec.whatwg.org/#shift_jis-decoder, CC BY 4.0): "If byte is an ASCII byte,
 * restore byte to ioQueue". It keeps line breaks and ASCII syntax after a stray lead byte.
 *
 * C1 normalisation (D4). Windows decodes some undefined single bytes b to U+00bb, glibc rejects
 * them; CodePageInfo::windowsOnlyC1 lists them (probe F1) and the driver maps them on every
 * platform, in both directions.
 *
 * Reversible encoding (D4). Both platforms have one-way mappings whose bytes do not decode back
 * (Windows maps 932's U+F8F0 to A0, which it refuses to decode; glibc maps U+00A5 to 5C, which
 * decodes to U+005C). encodeLegacy re-decodes every chunk it encodes and stops at the first
 * character whose bytes do not decode back to it. The M1 code pages are prefix-free (a lead byte
 * never decodes alone), so the first character where the re-decoded text differs is the
 * unencodable one.
 *
 * Errors. Codec errors keep Unsupported and become Internal otherwise (withName), so the error
 * codes the drivers return mean the same thing whatever codec ran.
 */

#include "frost/core/assert.hpp"
#include "frost/io/utf.hpp"
#include "frost/text/text_codec.hpp"

#include "unicode_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace frost::text {
namespace {

// UTF-8 input per encodeLegacy chunk: one encodePrefix and one decodePrefix check each (D17).
constexpr std::size_t kEncodeChunkBytes = 4096;

bool isWindowsOnlyC1(const CodePageInfo& info, std::uint8_t byte) noexcept {
    return std::binary_search(info.windowsOnlyC1.begin(), info.windowsOnlyC1.end(), byte);
}

/*
 * A codec's error with the context "while <verb> <name>". ITextCodec's errors are Internal or
 * Unsupported; any other code (a codec that passes an OS mapping such as InvalidArgument through)
 * becomes Internal with the codec's message, so callers can rely on the contract: InvalidArgument
 * from encodeForSave only ever means an unencodable character (text_file.hpp).
 */
Error withName(Error error, const CodePageInfo& info, std::string_view verb) {
    if (error.code() != ErrorCode::Internal && error.code() != ErrorCode::Unsupported) {
        Error internal(ErrorCode::Internal,
                       std::format("the {} codec failed ({}): {}", info.name, toString(error.code()), error.message()));
        for (const std::string& note : error.context()) {
            internal.withContext(note);
        }
        error = std::move(internal);
    }
    return std::move(error).withContext(std::format("while {} {}", verb, info.name));
}

// Checks one decodePrefix result: the count and the appended UTF-8 (from `before` on).
Result<void> checkDecoded(const CodePageInfo& info, std::size_t consumed, std::size_t given, const std::string& utf8,
                          std::size_t before) noexcept {
    if (consumed > given) {
        return fail(ErrorCode::Internal,
                    std::format("the {} codec reported {} bytes decoded of {} given", info.name, consumed, given));
    }
    const std::string_view appended = std::string_view(utf8).substr(before);
    if (const std::size_t valid = io::validUtf8Prefix(appended); valid != appended.size()) {
        return fail(ErrorCode::Internal,
                    std::format("the {} codec produced ill-formed UTF-8 at byte {} of its output", info.name, valid));
    }
    return {};
}

// The end of the next encode chunk of `utf8` starting at `pos`: at most kEncodeChunkBytes, at a
// character boundary, and at least one character.
std::size_t chunkEnd(std::string_view utf8, std::size_t pos) noexcept {
    if (utf8.size() - pos <= kEncodeChunkBytes) {
        return utf8.size();
    }
    std::size_t end = detail::floorCharBoundary(utf8, pos + kEncodeChunkBytes);
    if (end == pos) {
        end = pos + detail::utf8SequenceLength(detail::octetAt(utf8, pos));
    }
    return end;
}

/*
 * Encodes chunk `c` (well-formed, whole characters) and checks that its bytes decode back.
 * Returns the bytes of c's longest reversibly encodable prefix in `out` and that prefix's length.
 * Codec calls: one encodePrefix and one decodePrefix, plus one more of each when the re-decode
 * differs (the one-way case).
 */
Result<std::size_t> encodeChecked(ITextCodec& codec, std::string_view c, std::string& out, std::string& scratch,
                                  std::string& check) noexcept {
    const CodePageInfo& info = codec.info();
    out.clear();
    auto encoded = codec.encodePrefix(c, out);
    if (!encoded) {
        return std::unexpected(withName(std::move(encoded).error(), info, "encoding"));
    }
    const std::size_t k = *encoded;
    if (k > c.size() || detail::floorCharBoundary(c, k) != k) {
        return fail(ErrorCode::Internal,
                    std::format("the {} codec reported {} UTF-8 bytes encoded of {}, not a character boundary",
                                info.name, k, c.size()));
    }
    check.clear();
    auto decoded = codec.decodePrefix(out, check);
    if (!decoded) {
        return std::unexpected(withName(std::move(decoded).error(), info, "decoding"));
    }
    if (*decoded > out.size()) {
        return fail(ErrorCode::Internal,
                    std::format("the {} codec reported {} bytes decoded of {} given", info.name, *decoded, out.size()));
    }
    const std::string_view wanted = c.substr(0, k);
    if (*decoded == out.size() && check == wanted) {
        return k;
    }
    // A one-way mapping: the first differing UTF-8 byte, floored to a character boundary, starts
    // the first character whose bytes do not decode back to it.
    const std::size_t common = std::min(check.size(), wanted.size());
    std::size_t d = static_cast<std::size_t>(
        std::mismatch(wanted.begin(), wanted.begin() + static_cast<std::ptrdiff_t>(common), check.begin()).first -
        wanted.begin());
    d = detail::floorCharBoundary(c, d);
    if (d >= k) {
        return fail(ErrorCode::Internal, std::format("the {} codec decoded its own encoding to extra text", info.name));
    }
    // Re-encode the reversible part [0, d) and check it again.
    scratch.clear();
    auto again = codec.encodePrefix(c.substr(0, d), scratch);
    if (!again) {
        return std::unexpected(withName(std::move(again).error(), info, "encoding"));
    }
    check.clear();
    auto redecoded = codec.decodePrefix(scratch, check);
    if (!redecoded) {
        return std::unexpected(withName(std::move(redecoded).error(), info, "decoding"));
    }
    if (*again != d || *redecoded != scratch.size() || check != c.substr(0, d)) {
        return fail(ErrorCode::Internal,
                    std::format("the {} codec did not reproduce a prefix it had encoded reversibly", info.name));
    }
    out.swap(scratch);
    return d;
}

} // namespace

Result<std::size_t> decodeLegacy(ITextCodec& codec, std::string_view bytes, std::string& utf8, DecodeStats& stats,
                                 InputEnd end) noexcept {
    const CodePageInfo& info = codec.info();
    const std::size_t outStart = utf8.size();
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    const auto replace = [&](std::size_t at) {
        if (!stats.firstInvalidInput) {
            stats.firstInvalidInput = stats.inputBytes + at;
            stats.firstReplacementOutput = stats.outputBytes + (utf8.size() - outStart);
        }
        ++stats.replacements;
        utf8.append(kReplacementUtf8);
    };
    while (i < n) {
        const std::size_t before = utf8.size();
        auto decoded = codec.decodePrefix(bytes.substr(i), utf8);
        if (!decoded) {
            return std::unexpected(withName(std::move(decoded).error(), info, "decoding"));
        }
        if (auto checked = checkDecoded(info, *decoded, n - i, utf8, before); !checked) {
            return std::unexpected(std::move(checked).error());
        }
        i += *decoded;
        if (i == n) {
            break;
        }
        const std::uint8_t b = detail::octetAt(bytes, i);
        const bool lead = isLeadByte(info, b);
        if (!lead && isWindowsOnlyC1(info, b)) {
            detail::appendScalarUtf8(static_cast<char32_t>(b), utf8); // D4: Windows' U+00bb.
            i += 1;
            continue;
        }
        std::size_t skip = 1;
        if (lead) {
            if (i + 1 == n) {
                if (end == InputEnd::More) {
                    break; // Its trail may arrive with the next call.
                }
            } else if (detail::octetAt(bytes, i + 1) >= 0x80) {
                skip = 2; // An invalid pair; an ASCII trail is read again instead (WHATWG).
            }
        }
        replace(i);
        i += skip;
    }
    stats.inputBytes += i;
    stats.outputBytes += utf8.size() - outStart;
    return i;
}

Result<std::optional<std::uint64_t>> encodeLegacy(ITextCodec& codec, std::string_view utf8,
                                                  std::string& bytes) noexcept {
    const CodePageInfo& info = codec.info();
    std::string out;
    std::string scratch;
    std::string check;
    std::size_t pos = 0;
    while (pos < utf8.size()) {
        const std::string_view c = utf8.substr(pos, chunkEnd(utf8, pos) - pos);
        auto encoded = encodeChecked(codec, c, out, scratch, check);
        if (!encoded) {
            return std::unexpected(std::move(encoded).error());
        }
        bytes.append(out);
        pos += *encoded;
        if (*encoded == c.size()) {
            continue;
        }
        // The character at the stop has no reversible mapping from the codec. D4: U+0080..U+009F
        // is encodable as its byte when Windows decodes that byte to it.
        std::size_t length = 0;
        const char32_t x = detail::decodeValidUtf8Scalar(utf8, pos, length);
        if (x >= 0x80 && x <= 0x9F && isWindowsOnlyC1(info, static_cast<std::uint8_t>(x)) &&
            !isLeadByte(info, static_cast<std::uint8_t>(x))) {
            bytes.push_back(static_cast<char>(static_cast<std::uint8_t>(x)));
            pos += length;
            continue;
        }
        return std::optional<std::uint64_t>(pos);
    }
    return std::optional<std::uint64_t>();
}

} // namespace frost::text
