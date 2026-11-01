// SPDX-License-Identifier: Apache-2.0
//
// The M1 code-page table, encoding names, BOMs and the BOM sniffer (frost/text/encoding.hpp;
// ADR 0018, D2 and D8). Everything here is constexpr data or pure functions over it: no global
// constructors, no exit-time destructors.

#include "frost/text/encoding.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <iterator>

namespace frost::text {
namespace {

// Aliases (lower case; findEncoding compares ASCII case-insensitively). WHATWG labels where WHATWG
// has the encoding, minus the labels that would name a different page ("latin1", "ascii").
constexpr std::string_view kAliases437[] = {"csibm437"};
constexpr std::string_view kAliases850[] = {"csibm850"};
constexpr std::string_view kAliases866[] = {"csibm866", "866"};
constexpr std::string_view kAliases874[] = {"dos-874"};
constexpr std::string_view kAliases932[] = {"shift-jis", "sjis", "cp932", "ms932", "windows-31j"};
constexpr std::string_view kAliases1250[] = {"x-cp1250"};
constexpr std::string_view kAliases1251[] = {"x-cp1251"};
constexpr std::string_view kAliases1252[] = {"x-cp1252"};
constexpr std::string_view kAliases1253[] = {"x-cp1253"};
constexpr std::string_view kAliases1254[] = {"x-cp1254"};
constexpr std::string_view kAliases1256[] = {"x-cp1256"};
constexpr std::string_view kAliases1257[] = {"x-cp1257"};
constexpr std::string_view kAliases20866[] = {"koi8r", "koi8_r", "koi8", "koi", "cskoi8r"};
constexpr std::string_view kAliases28591[] = {"iso8859-1", "iso_8859-1", "iso88591"};
constexpr std::string_view kAliases28592[] = {"iso8859-2", "iso_8859-2", "iso88592", "latin2"};
constexpr std::string_view kAliases28605[] = {"iso8859-15", "iso_8859-15", "iso885915", "latin9"};

// Shift_JIS lead bytes (GetCPInfoExW's LeadByte for 932; EncodingPlatform.LeadBytesMatchGetCPInfoExW).
constexpr LeadByteRange kLead932[] = {{0x81, 0x9F}, {0xE0, 0xFC}};

// Bytes b that MultiByteToWideChar decodes to U+00bb and glibc rejects (ADR 0018, probe F1).
constexpr std::uint8_t kC1_874[] = {0x81, 0x82, 0x83, 0x84, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D,
                                    0x8E, 0x8F, 0x90, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F};
constexpr std::uint8_t kC1_932[] = {0x80};
constexpr std::uint8_t kC1_1250[] = {0x81, 0x83, 0x88, 0x90, 0x98};
constexpr std::uint8_t kC1_1251[] = {0x98};
constexpr std::uint8_t kC1_1252[] = {0x81, 0x8D, 0x8F, 0x90, 0x9D};
constexpr std::uint8_t kC1_1253[] = {0x81, 0x88, 0x8A, 0x8C, 0x8D, 0x8E, 0x8F,
                                     0x90, 0x98, 0x9A, 0x9C, 0x9D, 0x9E, 0x9F};
constexpr std::uint8_t kC1_1254[] = {0x81, 0x8D, 0x8E, 0x8F, 0x90, 0x9D, 0x9E};
constexpr std::uint8_t kC1_1257[] = {0x81, 0x83, 0x88, 0x8A, 0x8C, 0x90, 0x98, 0x9A, 0x9C, 0x9F};

constexpr CodePageInfo kTable[] = {
    {437, "IBM437", kAliases437, "CP437", {}, {}},
    {850, "IBM850", kAliases850, "CP850", {}, {}},
    {866, "IBM866", kAliases866, "CP866", {}, {}},
    {874, "windows-874", kAliases874, "CP874", {}, kC1_874},
    {932, "Shift_JIS", kAliases932, "CP932", kLead932, kC1_932},
    {1250, "windows-1250", kAliases1250, "CP1250", {}, kC1_1250},
    {1251, "windows-1251", kAliases1251, "CP1251", {}, kC1_1251},
    {1252, "windows-1252", kAliases1252, "CP1252", {}, kC1_1252},
    {1253, "windows-1253", kAliases1253, "CP1253", {}, kC1_1253},
    {1254, "windows-1254", kAliases1254, "CP1254", {}, kC1_1254},
    {1256, "windows-1256", kAliases1256, "CP1256", {}, {}},
    {1257, "windows-1257", kAliases1257, "CP1257", {}, kC1_1257},
    {20866, "KOI8-R", kAliases20866, "KOI8-R", {}, {}},
    {28591, "ISO-8859-1", kAliases28591, "ISO-8859-1", {}, {}},
    {28592, "ISO-8859-2", kAliases28592, "ISO-8859-2", {}, {}},
    {28605, "ISO-8859-15", kAliases28605, "ISO-8859-15", {}, {}},
};

constexpr bool tableIsSorted() noexcept {
    for (std::size_t i = 1; i < std::size(kTable); ++i) {
        if (kTable[i - 1].codePage >= kTable[i].codePage) {
            return false;
        }
    }
    return true;
}
static_assert(tableIsSorted(), "findCodePage binary-searches kTable by codePage");
static_assert(std::size(kTable) == 16, "the M1 table has 16 code pages (ADR 0018, D2)");

struct FormName {
    EncodingForm form;
    std::string_view name;
};
constexpr FormName kFormNames[] = {
    {EncodingForm::Utf8, "UTF-8"},       {EncodingForm::Utf16Le, "UTF-16LE"}, {EncodingForm::Utf16Be, "UTF-16BE"},
    {EncodingForm::Utf32Le, "UTF-32LE"}, {EncodingForm::Utf32Be, "UTF-32BE"},
};

constexpr char asciiLower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return asciiLower(x) == asciiLower(y); });
}

} // namespace

std::span<const CodePageInfo> supportedCodePages() noexcept { return kTable; }

const CodePageInfo* findCodePage(std::uint16_t codePage) noexcept {
    const auto* it = std::lower_bound(std::begin(kTable), std::end(kTable), codePage,
                                      [](const CodePageInfo& info, std::uint16_t cp) { return info.codePage < cp; });
    return it != std::end(kTable) && it->codePage == codePage ? it : nullptr;
}

std::string_view encodingName(Encoding encoding) noexcept {
    if (!encoding.isValid()) {
        return {};
    }
    if (encoding.isUnicode()) {
        return kFormNames[static_cast<std::size_t>(encoding.form)].name;
    }
    const CodePageInfo* info = findCodePage(encoding.codePage);
    return info != nullptr ? info->name : std::string_view{};
}

std::optional<Encoding> findEncoding(std::string_view name) noexcept {
    if (name.empty()) {
        return std::nullopt;
    }
    for (const FormName& entry : kFormNames) {
        if (equalsIgnoreCase(name, entry.name)) {
            return Encoding{entry.form, 0};
        }
    }
    if (equalsIgnoreCase(name, "utf8")) {
        return Encoding::utf8();
    }
    for (const CodePageInfo& info : kTable) {
        if (equalsIgnoreCase(name, info.name)) {
            return Encoding::legacy(info.codePage);
        }
        for (std::string_view alias : info.aliases) {
            if (equalsIgnoreCase(name, alias)) {
                return Encoding::legacy(info.codePage);
            }
        }
    }
    // "cp<N>" for table entries: digits only, no sign, no leading '+', at most 5 digits.
    if (name.size() > 2 && name.size() <= 7 && asciiLower(name[0]) == 'c' && asciiLower(name[1]) == 'p') {
        const std::string_view digits = name.substr(2);
        if (std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            std::uint32_t value = 0;
            const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
            if (error == std::errc{} && end == digits.data() + digits.size() && value <= 0xFFFF &&
                findCodePage(static_cast<std::uint16_t>(value)) != nullptr) {
                return Encoding::legacy(static_cast<std::uint16_t>(value));
            }
        }
    }
    return std::nullopt;
}

bool isAsciiCompatible(Encoding encoding) noexcept {
    if (!encoding.isValid()) {
        return false;
    }
    if (encoding.form == EncodingForm::Utf8) {
        return true;
    }
    return encoding.form == EncodingForm::CodePage && findCodePage(encoding.codePage) != nullptr;
}

std::string_view bomBytes(EncodingForm form) noexcept {
    switch (form) {
    case EncodingForm::Utf8: return std::string_view("\xEF\xBB\xBF", 3);
    case EncodingForm::Utf16Le: return std::string_view("\xFF\xFE", 2);
    case EncodingForm::Utf16Be: return std::string_view("\xFE\xFF", 2);
    case EncodingForm::Utf32Le: return std::string_view("\xFF\xFE\x00\x00", 4);
    case EncodingForm::Utf32Be: return std::string_view("\x00\x00\xFE\xFF", 4);
    case EncodingForm::CodePage: break;
    }
    return {};
}

std::optional<EncodingForm> sniffBom(std::string_view bytes) noexcept {
    if (bytes.starts_with(bomBytes(EncodingForm::Utf8))) {
        return EncodingForm::Utf8;
    }
    if (bytes.starts_with(bomBytes(EncodingForm::Utf32Be))) {
        return EncodingForm::Utf32Be;
    }
    if (bytes.starts_with(bomBytes(EncodingForm::Utf32Le)) && bytes.size() % 4 == 0) {
        return EncodingForm::Utf32Le;
    }
    if (bytes.starts_with(bomBytes(EncodingForm::Utf16Le))) {
        return EncodingForm::Utf16Le;
    }
    if (bytes.starts_with(bomBytes(EncodingForm::Utf16Be))) {
        return EncodingForm::Utf16Be;
    }
    return std::nullopt;
}

bool isLeadByte(const CodePageInfo& info, std::uint8_t byte) noexcept {
    return std::any_of(info.leadBytes.begin(), info.leadBytes.end(),
                       [byte](const LeadByteRange& range) { return byte >= range.first && byte <= range.last; });
}

} // namespace frost::text
