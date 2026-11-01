// SPDX-License-Identifier: Apache-2.0
//
// The Windows text codecs (frost/text/text_codec.hpp; ADR 0018, D3 and D17): strict
// MultiByteToWideChar/WideCharToMultiByte conversions for the M1 code pages, the platform
// provider, and the system ANSI code page for detection (D14). The only file of frost::text that
// calls Win32 for encodings; wide APIs only.
/*
 * Strictness. Decoding passes exactly MB_ERR_INVALID_CHARS (never MB_USEGLYPHCHARS, MB_COMPOSITE or
 * MB_PRECOMPOSED). Without it, 932 silently turns invalid input into U+30FB (probe F1), and the
 * single-byte pages map undefined bytes to best-fit characters. Encoding passes
 * WC_NO_BEST_FIT_CHARS with lpUsedDefaultChar and never lpDefaultChar or WC_COMPOSITECHECK, so a
 * character without a mapping is detected rather than replaced. None of the M1 pages is among
 * those that reject these flags (50220 and similar).
 *
 * Chunking (D17). Both directions keep every API call at or below 64 KiB of input. Decoding
 * starts each call with a one-character chunk and doubles the chunk after every success, never
 * splitting a lead byte from its trail, so a run of invalid bytes costs one failing API call per
 * byte (each decodeLegacy step starts a new call), and an invalid character after g valid bytes
 * costs O(log g) extra calls: a failing multi-character chunk is bisected at character boundaries
 * until the failing character is isolated. The bytes converted are at most twice the input.
 *
 * Code pages are passed as explicit numbers, never CP_ACP, CP_OEMCP or CP_THREAD_ACP: the product
 * and test manifests set activeCodePage=UTF-8, so GetACP() is 65001 in our processes.
 */

#include "frost/core/assert.hpp"
#include "frost/io/os_error.hpp"
#include "frost/text/text_codec.hpp"

#include "unicode_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include <windows.h>

namespace frost::text {
namespace {

// The largest input of one API call (bytes for MultiByteToWideChar, UTF-16 units for
// WideCharToMultiByte).
constexpr std::size_t kMaxCallInput = 64 * 1024;
// Chunks up to this size convert into a stack buffer (no allocation per call).
constexpr std::size_t kStackUnits = 512;

// A failure of a conversion API other than an invalid character (ITextCodec: platform failures
// are Internal): io's mapping of the code would read as invalid input (ERROR_INVALID_PARAMETER is
// InvalidArgument), so only its message is kept.
std::unexpected<Error> platformFailure(const Error& os) {
    return fail(ErrorCode::Internal, std::format("the Windows code page converter failed: {}", os.message()));
}

class WinCodec final : public ITextCodec {
public:
    explicit WinCodec(const CodePageInfo& info) noexcept : m_info(&info) {}

    [[nodiscard]] const CodePageInfo& info() const noexcept override { return *m_info; }

    [[nodiscard]] Result<std::size_t> decodePrefix(std::string_view bytes, std::string& utf8) noexcept override {
        const std::size_t n = bytes.size();
        std::size_t pos = 0;
        std::size_t target = 1; // The chunk size in bytes: one character first, then doubling to 64 KiB.
        while (pos < n) {
            // The chunk [pos, end): whole characters, at least `target` bytes (or the rest).
            std::size_t end = pos;
            while (end < n && end - pos < target) {
                const std::size_t length = charLength(bytes, end);
                if (length == 0) {
                    break; // A lead byte without its trail at the very end.
                }
                end += length;
            }
            if (end == pos) {
                return pos; // The lead byte at the end: its trail is missing.
            }
            auto converted = convert(bytes.substr(pos, end - pos), utf8);
            if (!converted) {
                return std::unexpected(std::move(converted).error());
            }
            if (*converted) {
                pos = end;
                target = std::min(target * 2, kMaxCallInput);
                continue;
            }
            // Bisect [pos, end) until one failing character is isolated; convert what precedes it.
            std::size_t lo = pos;
            std::size_t hi = end;
            while (charLength(bytes, lo) < hi - lo) {
                const std::size_t mid = boundaryNear(bytes, lo, hi);
                auto half = convert(bytes.substr(lo, mid - lo), utf8);
                if (!half) {
                    return std::unexpected(std::move(half).error());
                }
                if (*half) {
                    lo = mid; // The failing character is in [mid, hi).
                } else {
                    hi = mid;
                }
            }
            return lo;
        }
        return pos;
    }

    [[nodiscard]] Result<std::size_t> encodePrefix(std::string_view utf8, std::string& bytes) noexcept override {
        std::size_t pos = 0;
        std::wstring wide;
        while (pos < utf8.size()) {
            std::size_t end = std::min(utf8.size(), pos + kMaxCallInput / 2);
            end = detail::floorCharBoundary(utf8, end);
            if (end == pos) {
                end = pos + detail::utf8SequenceLength(detail::octetAt(utf8, pos));
            }
            const std::string_view piece = utf8.substr(pos, end - pos);
            wide.clear();
            toUtf16(piece, wide);
            auto used = encodeUnits(wide, bytes);
            if (!used) {
                return std::unexpected(std::move(used).error());
            }
            if (!*used) {
                pos = end;
                continue;
            }
            // Bisect at UTF-16 character boundaries to the first character that needs the default.
            std::size_t lo = 0;
            std::size_t hi = wide.size();
            while (unitLength(wide, lo) < hi - lo) {
                std::size_t mid = lo + (hi - lo) / 2;
                if (mid > lo && isLowSurrogate(wide[mid]) && isHighSurrogate(wide[mid - 1])) {
                    --mid; // Never split a surrogate pair.
                }
                if (mid == lo) {
                    mid = lo + unitLength(wide, lo);
                }
                auto half = encodeUnits(std::wstring_view(wide).substr(lo, mid - lo), bytes);
                if (!half) {
                    return std::unexpected(std::move(half).error());
                }
                if (!*half) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            return pos + utf8LengthOfUnits(piece, lo);
        }
        return pos;
    }

private:
    // The length of the character at `i` by the lead-byte table: 2 for a lead byte with a byte
    // after it, 1 otherwise, 0 for a lead byte at the very end.
    [[nodiscard]] std::size_t charLength(std::string_view bytes, std::size_t i) const noexcept {
        if (!isLeadByte(*m_info, detail::octetAt(bytes, i))) {
            return 1;
        }
        return i + 1 < bytes.size() ? 2 : 0;
    }

    // A character boundary strictly inside (lo, hi), near the middle; [lo, hi) holds at least two
    // characters. O(hi - lo) by scanning from lo, so a whole bisection is linear in the chunk.
    [[nodiscard]] std::size_t boundaryNear(std::string_view bytes, std::size_t lo, std::size_t hi) const noexcept {
        const std::size_t half = lo + (hi - lo) / 2;
        std::size_t at = lo + charLength(bytes, lo);
        while (at < half) {
            const std::size_t next = at + charLength(bytes, at);
            if (next >= hi) {
                break;
            }
            at = next;
        }
        return at;
    }

    // Converts `chunk` strictly and appends its UTF-8. True when it converted, false when the chunk
    // holds an invalid character (ERROR_NO_UNICODE_TRANSLATION); other failures are errors.
    [[nodiscard]] Result<bool> convert(std::string_view chunk, std::string& utf8) const noexcept {
        // Each unit of these pages becomes at most one UTF-16 unit, so chunk.size() units suffice.
        wchar_t stack[kStackUnits];
        std::wstring heap;
        wchar_t* buffer = stack;
        if (chunk.size() > kStackUnits) {
            heap.resize(chunk.size());
            buffer = heap.data();
        }
        const int written = MultiByteToWideChar(m_info->codePage, MB_ERR_INVALID_CHARS, chunk.data(),
                                                static_cast<int>(chunk.size()), buffer, static_cast<int>(chunk.size()));
        if (written <= 0) {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_UNICODE_TRANSLATION) {
                return false;
            }
            if (error == ERROR_INSUFFICIENT_BUFFER) {
                return fail(
                    ErrorCode::Internal,
                    std::format("MultiByteToWideChar({}) needed more than one UTF-16 unit per byte", m_info->codePage));
            }
            return platformFailure(io::win32Error(error, "MultiByteToWideChar"));
        }
        detail::appendUtf16UnitsAsUtf8(std::wstring_view(buffer, static_cast<std::size_t>(written)), utf8);
        return true;
    }

    // Encodes `units` (whole characters) and appends the bytes. True when some character needed
    // the default character (nothing is appended then); false when everything converted.
    [[nodiscard]] Result<bool> encodeUnits(std::wstring_view units, std::string& bytes) const noexcept {
        if (units.empty()) {
            return false;
        }
        const std::size_t capacity = 2 * units.size(); // At most 2 bytes per UTF-16 unit for these pages.
        const std::size_t start = bytes.size();
        bytes.resize(start + capacity);
        BOOL usedDefault = FALSE;
        const int written =
            WideCharToMultiByte(m_info->codePage, WC_NO_BEST_FIT_CHARS, units.data(), static_cast<int>(units.size()),
                                bytes.data() + start, static_cast<int>(capacity), nullptr, &usedDefault);
        if (written <= 0) {
            const Error error = io::lastWin32Error("WideCharToMultiByte");
            bytes.resize(start);
            return platformFailure(error);
        }
        if (usedDefault != FALSE) {
            bytes.resize(start);
            return true;
        }
        bytes.resize(start + static_cast<std::size_t>(written));
        return false;
    }

    static bool isHighSurrogate(wchar_t u) noexcept { return u >= 0xD800 && u <= 0xDBFF; }
    static bool isLowSurrogate(wchar_t u) noexcept { return u >= 0xDC00 && u <= 0xDFFF; }

    // The UTF-16 units of the character at `i` (1, or 2 for a surrogate pair).
    static std::size_t unitLength(std::wstring_view units, std::size_t i) noexcept {
        return i + 1 < units.size() && isHighSurrogate(units[i]) && isLowSurrogate(units[i + 1]) ? 2 : 1;
    }

    // Appends the UTF-16 of well-formed `utf8`.
    static void toUtf16(std::string_view utf8, std::wstring& out) noexcept {
        for (std::size_t i = 0; i < utf8.size();) {
            std::size_t length = 0;
            const auto c = static_cast<std::uint32_t>(detail::decodeValidUtf8Scalar(utf8, i, length));
            i += length;
            if (c >= 0x10000) {
                out.push_back(static_cast<wchar_t>(0xD800 + ((c - 0x10000) >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
            } else {
                out.push_back(static_cast<wchar_t>(c));
            }
        }
    }

    // The UTF-8 bytes of the first `units` UTF-16 units of well-formed `utf8`.
    static std::size_t utf8LengthOfUnits(std::string_view utf8, std::size_t units) noexcept {
        std::size_t i = 0;
        std::size_t counted = 0;
        while (counted < units && i < utf8.size()) {
            const std::size_t length = detail::utf8SequenceLength(detail::octetAt(utf8, i));
            counted += length == 4 ? 2 : 1;
            i += length;
        }
        return i;
    }

    const CodePageInfo* m_info;
};

class WinProvider final : public ITextCodecProvider {
public:
    constexpr WinProvider() noexcept = default;

    [[nodiscard]] Result<std::unique_ptr<ITextCodec>> open(std::uint16_t codePage) noexcept override {
        const CodePageInfo* info = findCodePage(codePage);
        if (info == nullptr) {
            return fail(ErrorCode::Unsupported, std::format("code page {} is not supported", codePage));
        }
        if (IsValidCodePage(codePage) == FALSE) {
            return fail(ErrorCode::Unsupported,
                        std::format("Windows has no converter for code page {} ({})", codePage, info->name));
        }
        return std::unique_ptr<ITextCodec>(std::make_unique<WinCodec>(*info));
    }
};

// Constant-initialised and trivially destructible: no global constructor, no exit-time destructor.
constinit WinProvider gProvider;

} // namespace

ITextCodecProvider& platformCodecs() noexcept { return gProvider; }

std::uint16_t defaultLegacyCodePage() noexcept {
    // Computed once; a function-local static integer (thread-safe initialisation).
    static const std::uint16_t value = [] {
        DWORD codePage = 0;
        const int read = GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                                         reinterpret_cast<LPWSTR>(&codePage), sizeof(DWORD) / sizeof(WCHAR));
        if (read == 0 || codePage == 0 || codePage > 0xFFFF ||
            findCodePage(static_cast<std::uint16_t>(codePage)) == nullptr) {
            return std::uint16_t{1252};
        }
        return static_cast<std::uint16_t>(codePage);
    }();
    return value;
}

} // namespace frost::text
