// SPDX-License-Identifier: Apache-2.0
//
// Encoding identity for frost::text's load and save pipeline (ADR 0018): the Unicode forms and
// the legacy code pages FrostIDE supports in M1, their canonical names and aliases, and the
// byte-order marks. Plain values and static constexpr tables; every function is pure and
// thread-safe.
/*
 * A TextBuffer always holds well-formed UTF-8 (ADR 0013, D1). A file's bytes are something else:
 * UTF-8, UTF-16 or UTF-32 in either byte order, or a legacy code page. Encoding names that form;
 * text_codec.hpp converts between it and UTF-8, and text_file.hpp loads and saves whole files.
 *
 * Legacy code pages are named by their Windows code page identifier (the number
 * MultiByteToWideChar takes), on every platform: the iconv codec maps the identifier to a glibc
 * converter name through the table below (CodePageInfo::iconvName). The M1 table holds the 16
 * pages on which Windows and glibc agree once normalised (ADR 0018, D2 and D4); CJK pages other
 * than Shift_JIS, the composing pages 1255/1258, KOI8-U and every stateful encoding are excluded
 * with measured evidence (ADR 0018, probe F4).
 *
 * Names follow the WHATWG Encoding Standard (https://encoding.spec.whatwg.org/, CC BY 4.0) where
 * it has the encoding, with one deliberate divergence: "ISO-8859-1" names the real ISO-8859-1
 * (28591), not WHATWG's label for windows-1252. "latin1" is not accepted, because EditorConfig
 * reads it as ISO-8859-1 and WHATWG as windows-1252; m1/settings maps EditorConfig itself.
 */
#pragma once

#include "frost/core/assert.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace frost::text {

// The byte form of a file on disk (ADR 0018, D2). The buffer always holds UTF-8. The enumerator
// values are stable and append-only (m1/save-manager persists encodings by name; tests pin them).
enum class EncodingForm : std::uint8_t { Utf8, Utf16Le, Utf16Be, Utf32Le, Utf32Be, CodePage };

namespace detail {
// True for the code page ids that name Unicode forms (65001, 1200, 1201, 12000, 12001).
[[nodiscard]] constexpr bool isUnicodeCodePageId(std::uint32_t id) noexcept {
    return id == 65001 || id == 1200 || id == 1201 || id == 12000 || id == 12001;
}
} // namespace detail

/*
 * An encoding: a Unicode form, or a legacy code page named by its Windows code page identifier
 * (the number MultiByteToWideChar takes; the iconv codec maps it to a glibc name). A plain,
 * trivially copyable value. A CodePage value may name an unsupported page; functions that need a
 * codec report Unsupported for it.
 * Invariant (kept by every factory; callers that build values by hand keep it too): codePage is 0
 * exactly when form != CodePage, and a CodePage value never carries a Unicode id (65001, 1200,
 * 1201, 12000, 12001), which fromCodePageId maps to its form. Values read from settings or a
 * journal are validated with findEncoding (names) or isValid() before use.
 */
struct Encoding {
    EncodingForm form = EncodingForm::Utf8;
    std::uint16_t codePage = 0;

    // UTF-8. O(1).
    [[nodiscard]] static constexpr Encoding utf8() noexcept { return {}; }
    // A Unicode form. Precondition (FROST_PRE): f != CodePage. O(1).
    [[nodiscard]] static constexpr Encoding unicode(EncodingForm f) noexcept {
        FROST_PRE(f != EncodingForm::CodePage);
        return Encoding{f, 0};
    }
    // A legacy code page, for constants in code. Precondition (FROST_PRE): cp >= 4 and cp is not
    // a Unicode id. External ids (settings, consoles) go through fromCodePageId instead. O(1).
    [[nodiscard]] static constexpr Encoding legacy(std::uint16_t cp) noexcept {
        FROST_PRE(cp >= 4 && !detail::isUnicodeCodePageId(cp));
        return Encoding{EncodingForm::CodePage, cp};
    }
    /*
     * The encoding of a code page id from outside: a console's GetConsoleOutputCP (M4), a project
     * or settings value.
     *   - 65001 -> utf8(); 1200/1201 -> UTF-16LE/BE; 12000/12001 -> UTF-32LE/BE;
     *   - other ids from 4 to 65535, except 65000 (UTF-7) -> legacy(id), which may be unsupported;
     *   - 0..3 (CP_ACP, CP_OEMCP, CP_MACCP, CP_THREAD_ACP), 65000 and ids above 65535 -> nullopt.
     * O(1).
     */
    [[nodiscard]] static constexpr std::optional<Encoding> fromCodePageId(std::uint32_t id) noexcept {
        switch (id) {
        case 65001: return utf8();
        case 1200: return unicode(EncodingForm::Utf16Le);
        case 1201: return unicode(EncodingForm::Utf16Be);
        case 12000: return unicode(EncodingForm::Utf32Le);
        case 12001: return unicode(EncodingForm::Utf32Be);
        default: break;
        }
        if (id < 4 || id == 65000 || id > 65535) {
            return std::nullopt;
        }
        return legacy(static_cast<std::uint16_t>(id));
    }

    // True for the five Unicode forms. O(1).
    [[nodiscard]] constexpr bool isUnicode() const noexcept { return form != EncodingForm::CodePage; }
    // The invariant above: the form is an enumerator, and codePage is 0 exactly for Unicode forms
    // and neither 0..3 nor a Unicode id for CodePage. For values from outside. O(1).
    [[nodiscard]] constexpr bool isValid() const noexcept {
        if (form > EncodingForm::CodePage) {
            return false;
        }
        if (form != EncodingForm::CodePage) {
            return codePage == 0;
        }
        return codePage >= 4 && !detail::isUnicodeCodePageId(codePage);
    }

    friend constexpr bool operator==(const Encoding&, const Encoding&) noexcept = default;
};

// An inclusive range of DBCS lead bytes.
struct LeadByteRange {
    std::uint8_t first;
    std::uint8_t last;
};

/*
 * One supported legacy code page: a static constexpr table entry with no global constructors.
 * windowsOnlyC1 lists the bytes b that Windows decodes to U+00bb and glibc rejects (ADR 0018,
 * probe F1). The shared driver (text_codec.hpp) accepts them on every platform, in both
 * directions (D4). Lifetime: entries of supportedCodePages() live in static storage; test doubles
 * own theirs.
 */
struct CodePageInfo {
    std::uint16_t codePage = 0;
    std::string_view name;                     // Canonical display and persistence name: "windows-1252", "Shift_JIS".
    std::span<const std::string_view> aliases; // Lower-case names that findEncoding also accepts.
    std::string_view
        iconvName; // The glibc iconv name ("CP1252", "CP932", "ISO-8859-15"); the only strings passed to iconv_open.
    std::span<const LeadByteRange> leadBytes;    // DBCS lead-byte ranges; empty for single-byte pages.
    std::span<const std::uint8_t> windowsOnlyC1; // Sorted ascending.

    // True for double-byte pages (932 in M1). O(1).
    [[nodiscard]] constexpr bool isDoubleByte() const noexcept { return !leadBytes.empty(); }
};

/*
 * The M1 table, sorted by codePage (16 entries):
 *   437 IBM437 | 850 IBM850 | 866 IBM866 | 874 windows-874 | 932 Shift_JIS (lead bytes 81-9F and
 *   E0-FC) | 1250..1254, 1256, 1257 windows-125x | 20866 KOI8-R | 28591 ISO-8859-1 |
 *   28592 ISO-8859-2 | 28605 ISO-8859-15.
 * Static storage. O(1).
 */
[[nodiscard]] std::span<const CodePageInfo> supportedCodePages() noexcept;
// The table entry for `codePage`, or nullptr when it is not in the table. O(log 16).
[[nodiscard]] const CodePageInfo* findCodePage(std::uint16_t codePage) noexcept;
// "UTF-8", "UTF-16LE", "UTF-16BE", "UTF-32LE", "UTF-32BE", or CodePageInfo::name; "" for an
// unsupported code page or an invalid value. Static storage. O(log 16).
[[nodiscard]] std::string_view encodingName(Encoding encoding) noexcept;
// The inverse of encodingName, ASCII case-insensitive. Accepts the names, the aliases, "utf8" and
// "cp<N>" for table entries. nullopt otherwise ("latin1", "utf-16", "cp936", ""). O(table).
[[nodiscard]] std::optional<Encoding> findEncoding(std::string_view name) noexcept;
// True for UTF-8 and every table code page: their bytes 00..7F are ASCII. Detection's default
// encoding must be one of these. O(log 16).
[[nodiscard]] bool isAsciiCompatible(Encoding encoding) noexcept;
// The BOM bytes of a Unicode form: EF BB BF; FF FE; FE FF; FF FE 00 00; 00 00 FE FF. Empty for
// CodePage. Static storage. O(1).
[[nodiscard]] std::string_view bomBytes(EncodingForm form) noexcept;
/*
 * The form whose BOM starts `bytes`, testing UTF-32 before UTF-16. FF FE 00 00 is UTF-32LE only
 * when bytes.size() % 4 == 0; otherwise it is UTF-16LE (a BOM followed by U+0000). detectEncoding
 * adds UTF-16LE as the second candidate in the UTF-32LE case (D8). It depends on the total size,
 * so pass a view of the whole file (a whole mapped view), not a prefix. O(1).
 */
[[nodiscard]] std::optional<EncodingForm> sniffBom(std::string_view bytes) noexcept;
// True when `byte` lies in one of info's lead-byte ranges. O(ranges).
[[nodiscard]] bool isLeadByte(const CodePageInfo& info, std::uint8_t byte) noexcept;

} // namespace frost::text
