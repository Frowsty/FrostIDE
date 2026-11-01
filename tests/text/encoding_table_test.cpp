// SPDX-License-Identifier: Apache-2.0
//
// EncodingTable: names, aliases, the M1 code-page table, BOMs and code page ids (ADR 0018, D1,
// D2 and D4), and EncodingPlatform: what the platform codecs and the OS report about the same
// table (probe F1 and F5).

#include "frost/text/encoding.hpp"
#include "frost/text/encoding_detection.hpp"
#include "frost/text/line_separator.hpp"
#include "frost/text/text_codec.hpp"
#include "frost/text/text_file.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>

#include <iconv.h>
#endif

namespace {

using frost::text::bomBytes;
using frost::text::CodePageInfo;
using frost::text::DetectionSource;
using frost::text::Encoding;
using frost::text::EncodingForm;
using frost::text::encodingName;
using frost::text::findCodePage;
using frost::text::findEncoding;
using frost::text::LineSeparator;
using frost::text::LossKind;
using frost::text::sniffBom;
using frost::text::supportedCodePages;

// The persisted enumerator values (append-only; m1/save-manager stores names, tests pin values).
static_assert(static_cast<int>(EncodingForm::Utf8) == 0 && static_cast<int>(EncodingForm::Utf16Le) == 1 &&
              static_cast<int>(EncodingForm::Utf16Be) == 2 && static_cast<int>(EncodingForm::Utf32Le) == 3 &&
              static_cast<int>(EncodingForm::Utf32Be) == 4 && static_cast<int>(EncodingForm::CodePage) == 5);
static_assert(static_cast<int>(LineSeparator::Lf) == 0 && static_cast<int>(LineSeparator::CrLf) == 1 &&
              static_cast<int>(LineSeparator::Cr) == 2);
static_assert(static_cast<int>(LossKind::None) == 0 && static_cast<int>(LossKind::Replaced) == 1 &&
              static_cast<int>(LossKind::NotRoundTrip) == 2);
static_assert(static_cast<int>(DetectionSource::Bom) == 0 && static_cast<int>(DetectionSource::UserOverride) == 1 &&
              static_cast<int>(DetectionSource::Empty) == 2 && static_cast<int>(DetectionSource::AsciiOnly) == 3 &&
              static_cast<int>(DetectionSource::NulPattern) == 4 && static_cast<int>(DetectionSource::Utf8Valid) == 5 &&
              static_cast<int>(DetectionSource::LegacyFallback) == 6 && static_cast<int>(DetectionSource::Lossy) == 7);
static_assert(Encoding::utf8().isValid() && !Encoding{EncodingForm::CodePage, 65001}.isValid() &&
              !Encoding{EncodingForm::Utf8, 1252}.isValid() && Encoding::legacy(1252).isValid());

constexpr EncodingForm kUnicodeForms[] = {EncodingForm::Utf8, EncodingForm::Utf16Le, EncodingForm::Utf16Be,
                                          EncodingForm::Utf32Le, EncodingForm::Utf32Be};

// Probe F1: the bytes Windows decodes to U+00bb and glibc rejects.
const std::map<std::uint16_t, std::vector<std::uint8_t>>& probeWindowsOnlyC1() {
    static const std::map<std::uint16_t, std::vector<std::uint8_t>> lists = {
        {874, {0x81, 0x82, 0x83, 0x84, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D,
               0x8E, 0x8F, 0x90, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F}},
        {932, {0x80}},
        {1250, {0x81, 0x83, 0x88, 0x90, 0x98}},
        {1251, {0x98}},
        {1252, {0x81, 0x8D, 0x8F, 0x90, 0x9D}},
        {1253, {0x81, 0x88, 0x8A, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x98, 0x9A, 0x9C, 0x9D, 0x9E, 0x9F}},
        {1254, {0x81, 0x8D, 0x8E, 0x8F, 0x90, 0x9D, 0x9E}},
        {1257, {0x81, 0x83, 0x88, 0x8A, 0x8C, 0x90, 0x98, 0x9A, 0x9C, 0x9F}},
    };
    return lists;
}

TEST(EncodingTable, NamesRoundTrip) {
    for (const EncodingForm form : kUnicodeForms) {
        const Encoding e = form == EncodingForm::Utf8 ? Encoding::utf8() : Encoding::unicode(form);
        const std::string_view name = encodingName(e);
        ASSERT_FALSE(name.empty());
        EXPECT_EQ(findEncoding(name), e) << name;
    }
    EXPECT_EQ(encodingName(Encoding::utf8()), "UTF-8");
    EXPECT_EQ(encodingName(Encoding::unicode(EncodingForm::Utf16Le)), "UTF-16LE");
    EXPECT_EQ(encodingName(Encoding::unicode(EncodingForm::Utf32Be)), "UTF-32BE");
    for (const CodePageInfo& info : supportedCodePages()) {
        const Encoding e = Encoding::legacy(info.codePage);
        EXPECT_EQ(encodingName(e), info.name);
        EXPECT_EQ(findEncoding(info.name), e) << info.name;
        for (const std::string_view alias : info.aliases) {
            EXPECT_EQ(findEncoding(alias), e) << alias;
            EXPECT_EQ(alias, [&] {
                std::string lower(alias);
                std::ranges::transform(lower, lower.begin(), [](char c) {
                    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
                });
                return lower;
            }()) << "aliases are lower case";
        }
        EXPECT_EQ(findEncoding("cp" + std::to_string(info.codePage)), e);
    }
    EXPECT_EQ(encodingName(Encoding::legacy(936)), "");
    EXPECT_EQ(encodingName(Encoding::legacy(1252)), "windows-1252");
    EXPECT_EQ(encodingName(Encoding::legacy(932)), "Shift_JIS");
    EXPECT_EQ(encodingName(Encoding::legacy(28591)), "ISO-8859-1");
}

TEST(EncodingTable, AliasesCaseInsensitive) {
    EXPECT_EQ(findEncoding("SJIS"), Encoding::legacy(932));
    EXPECT_EQ(findEncoding("shift_jis"), Encoding::legacy(932));
    EXPECT_EQ(findEncoding("Windows-31J"), Encoding::legacy(932));
    EXPECT_EQ(findEncoding("Cp1252"), Encoding::legacy(1252));
    EXPECT_EQ(findEncoding("UTF8"), Encoding::utf8());
    EXPECT_EQ(findEncoding("utf-8"), Encoding::utf8());
    EXPECT_EQ(findEncoding("utf-16le"), Encoding::unicode(EncodingForm::Utf16Le));
    EXPECT_EQ(findEncoding("LATIN2"), Encoding::legacy(28592));
    EXPECT_EQ(findEncoding("latin9"), Encoding::legacy(28605));
    EXPECT_EQ(findEncoding("koi8-r"), Encoding::legacy(20866));
}

TEST(EncodingTable, UnknownAndAmbiguousRejected) {
    for (const std::string_view name : {"latin1", "utf-16", "utf-32", "cp936", "", "cp", "cp65001", "unicode",
                                        "windows-936", "ISO-8859-5", " utf-8", "utf-8 "}) {
        EXPECT_EQ(findEncoding(name), std::nullopt) << "'" << name << "'";
    }
}

TEST(EncodingTable, TableSortedUnique) {
    const auto table = supportedCodePages();
    ASSERT_EQ(table.size(), 16u);
    const std::vector<std::uint16_t> expected = {437,  850,  866,  874,  932,   1250,  1251,  1252,
                                                 1253, 1254, 1256, 1257, 20866, 28591, 28592, 28605};
    std::set<std::string> names;
    for (std::size_t i = 0; i < table.size(); ++i) {
        EXPECT_EQ(table[i].codePage, expected[i]);
        EXPECT_FALSE(table[i].iconvName.empty());
        EXPECT_TRUE(names.insert(std::string(table[i].name)).second) << table[i].name;
        for (const std::string_view alias : table[i].aliases) {
            EXPECT_TRUE(names.insert(std::string(alias)).second) << alias;
        }
        EXPECT_TRUE(std::ranges::is_sorted(table[i].windowsOnlyC1));
        EXPECT_EQ(findCodePage(table[i].codePage), &table[i]);
    }
    EXPECT_EQ(findCodePage(936), nullptr);
    EXPECT_EQ(findCodePage(0), nullptr);
}

TEST(EncodingTable, LeadBytesOnlyFor932) {
    for (const CodePageInfo& info : supportedCodePages()) {
        EXPECT_EQ(info.isDoubleByte(), info.codePage == 932) << info.codePage;
    }
    const CodePageInfo& sjis = *findCodePage(932);
    for (unsigned b = 0; b < 256; ++b) {
        const bool lead = (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
        EXPECT_EQ(frost::text::isLeadByte(sjis, static_cast<std::uint8_t>(b)), lead) << b;
        EXPECT_FALSE(frost::text::isLeadByte(*findCodePage(1252), static_cast<std::uint8_t>(b)));
    }
}

TEST(EncodingTable, WindowsOnlyC1MatchesProbe) {
    for (const CodePageInfo& info : supportedCodePages()) {
        const auto& lists = probeWindowsOnlyC1();
        const auto it = lists.find(info.codePage);
        const std::vector<std::uint8_t> expected = it != lists.end() ? it->second : std::vector<std::uint8_t>{};
        EXPECT_EQ(std::vector<std::uint8_t>(info.windowsOnlyC1.begin(), info.windowsOnlyC1.end()), expected)
            << info.codePage;
    }
}

TEST(EncodingTable, BomBytesAndSniff) {
    EXPECT_EQ(bomBytes(EncodingForm::Utf8), "\xEF\xBB\xBF");
    EXPECT_EQ(bomBytes(EncodingForm::Utf16Le), "\xFF\xFE");
    EXPECT_EQ(bomBytes(EncodingForm::Utf16Be), "\xFE\xFF");
    EXPECT_EQ(bomBytes(EncodingForm::Utf32Le), std::string_view("\xFF\xFE\x00\x00", 4));
    EXPECT_EQ(bomBytes(EncodingForm::Utf32Be), std::string_view("\x00\x00\xFE\xFF", 4));
    EXPECT_EQ(bomBytes(EncodingForm::CodePage), "");

    using F = EncodingForm;
    EXPECT_EQ(sniffBom("\xEF\xBB\xBFx"), F::Utf8);
    EXPECT_EQ(sniffBom("\xEF\xBB\xBF"), F::Utf8);
    EXPECT_EQ(sniffBom("\xEF\xBB"), std::nullopt);
    EXPECT_EQ(sniffBom("\xFE"), std::nullopt);
    EXPECT_EQ(sniffBom("\xFF"), std::nullopt);
    EXPECT_EQ(sniffBom(""), std::nullopt);
    EXPECT_EQ(sniffBom("\xFE\xFF"), F::Utf16Be);
    EXPECT_EQ(sniffBom("\xFF\xFE"), F::Utf16Le);
    EXPECT_EQ(sniffBom("\xFF\xFE"
                       "A"),
              F::Utf16Le);
    EXPECT_EQ(sniffBom(std::string_view("\x00\x00\xFE\xFF", 4)), F::Utf32Be);
    EXPECT_EQ(sniffBom(std::string_view("\x00\x00\xFE", 3)), std::nullopt);
    // FF FE 00 00: UTF-32LE only when the size is a multiple of 4 (D8).
    EXPECT_EQ(sniffBom(std::string_view("\xFF\xFE\x00\x00", 4)), F::Utf32Le);
    EXPECT_EQ(sniffBom(std::string_view("\xFF\xFE\x00\x00\x41\x00", 6)), F::Utf16Le);
    EXPECT_EQ(sniffBom(std::string_view("\xFF\xFE\x00\x00\x41\x00\x00\x00", 8)), F::Utf32Le);
    EXPECT_EQ(sniffBom(std::string_view("\xFF\xFE\x00\x00\x41", 5)), F::Utf16Le);
    EXPECT_EQ(sniffBom("abc"), std::nullopt);
}

TEST(EncodingTable, FromCodePageId) {
    EXPECT_EQ(Encoding::fromCodePageId(65001), Encoding::utf8());
    EXPECT_EQ(Encoding::fromCodePageId(1200), Encoding::unicode(EncodingForm::Utf16Le));
    EXPECT_EQ(Encoding::fromCodePageId(1201), Encoding::unicode(EncodingForm::Utf16Be));
    EXPECT_EQ(Encoding::fromCodePageId(12000), Encoding::unicode(EncodingForm::Utf32Le));
    EXPECT_EQ(Encoding::fromCodePageId(12001), Encoding::unicode(EncodingForm::Utf32Be));
    EXPECT_EQ(Encoding::fromCodePageId(1252), Encoding::legacy(1252));
    EXPECT_EQ(Encoding::fromCodePageId(936), Encoding::legacy(936)); // Valid, but unsupported.
    EXPECT_EQ(Encoding::fromCodePageId(4), Encoding::legacy(4));
    EXPECT_EQ(Encoding::fromCodePageId(65535), Encoding::legacy(65535));
    for (const std::uint32_t id : {0u, 1u, 2u, 3u, 65000u, 65536u, 70000u, 0xFFFF'FFFFu}) {
        EXPECT_EQ(Encoding::fromCodePageId(id), std::nullopt) << id;
    }
    for (std::uint32_t id = 0; id <= 0x1'0000; ++id) {
        if (const auto e = Encoding::fromCodePageId(id)) {
            EXPECT_TRUE(e->isValid()) << id;
        }
    }
}

TEST(EncodingTable, AsciiCompatible) {
    EXPECT_TRUE(frost::text::isAsciiCompatible(Encoding::utf8()));
    for (const EncodingForm form : kUnicodeForms) {
        if (form != EncodingForm::Utf8) {
            EXPECT_FALSE(frost::text::isAsciiCompatible(Encoding::unicode(form)));
        }
    }
    for (const CodePageInfo& info : supportedCodePages()) {
        EXPECT_TRUE(frost::text::isAsciiCompatible(Encoding::legacy(info.codePage))) << info.codePage;
    }
    EXPECT_FALSE(frost::text::isAsciiCompatible(Encoding::legacy(936)));
}

TEST(EncodingTable, EnumNamesRoundTrip) {
    using frost::text::detectionSourceName;
    using frost::text::findDetectionSource;
    using frost::text::findLossKind;
    using frost::text::findSeparator;
    using frost::text::lossKindName;
    using frost::text::separatorName;
    for (const LossKind k : {LossKind::None, LossKind::Replaced, LossKind::NotRoundTrip}) {
        EXPECT_EQ(findLossKind(lossKindName(k)), k);
    }
    EXPECT_EQ(lossKindName(LossKind::NotRoundTrip), "not-round-trip");
    EXPECT_EQ(lossKindName(static_cast<LossKind>(9)), "");
    EXPECT_EQ(findLossKind("Replaced"), std::nullopt);
    EXPECT_EQ(findLossKind(""), std::nullopt);
    for (int v = 0; v <= 7; ++v) {
        const auto s = static_cast<DetectionSource>(v);
        EXPECT_EQ(findDetectionSource(detectionSourceName(s)), s) << v;
    }
    EXPECT_EQ(detectionSourceName(DetectionSource::UserOverride), "user-override");
    EXPECT_EQ(detectionSourceName(static_cast<DetectionSource>(8)), "");
    EXPECT_EQ(findDetectionSource("BOM"), std::nullopt);
    EXPECT_EQ(findDetectionSource(""), std::nullopt);
    for (const LineSeparator s : {LineSeparator::Lf, LineSeparator::CrLf, LineSeparator::Cr}) {
        EXPECT_EQ(findSeparator(separatorName(s)), s);
    }
}

TEST(EncodingPlatform, EveryCodePageOpens) {
    for (const CodePageInfo& info : supportedCodePages()) {
        auto codec = frost::text::platformCodecs().open(info.codePage);
        ASSERT_TRUE(codec.has_value()) << info.codePage << ": " << codec.error().describe();
        EXPECT_EQ((*codec)->info().codePage, info.codePage);
        EXPECT_EQ((*codec)->info().name, info.name);
    }
    auto missing = frost::text::platformCodecs().open(936);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), frost::ErrorCode::Unsupported);
}

#if defined(_WIN32)

TEST(EncodingPlatform, LeadBytesMatchGetCPInfoExW) {
    for (const CodePageInfo& info : supportedCodePages()) {
        CPINFOEXW cp{};
        ASSERT_TRUE(GetCPInfoExW(info.codePage, 0, &cp)) << info.codePage;
        std::vector<std::pair<unsigned, unsigned>> os;
        for (unsigned k = 0; k + 1 < MAX_LEADBYTES && (cp.LeadByte[k] != 0 || cp.LeadByte[k + 1] != 0); k += 2) {
            os.emplace_back(cp.LeadByte[k], cp.LeadByte[k + 1]);
        }
        std::vector<std::pair<unsigned, unsigned>> table;
        for (const auto& range : info.leadBytes) {
            table.emplace_back(range.first, range.last);
        }
        EXPECT_EQ(os, table) << info.codePage;
    }
}

TEST(EncodingPlatform, WindowsAcceptsWindowsOnlyC1) {
    for (const CodePageInfo& info : supportedCodePages()) {
        for (const std::uint8_t b : info.windowsOnlyC1) {
            const char in = static_cast<char>(b);
            wchar_t out[2] = {};
            const int n = MultiByteToWideChar(info.codePage, MB_ERR_INVALID_CHARS, &in, 1, out, 2);
            ASSERT_EQ(n, 1) << info.codePage << " byte " << unsigned{b};
            EXPECT_EQ(static_cast<unsigned>(out[0]), unsigned{b}) << info.codePage;
        }
    }
}

TEST(EncodingPlatform, DefaultLegacyCodePageIsSystemAnsi) {
    DWORD value = 0;
    const int got = GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                                    reinterpret_cast<LPWSTR>(&value), sizeof(value) / sizeof(WCHAR));
    const std::uint16_t chosen = frost::text::defaultLegacyCodePage();
    EXPECT_NE(chosen, 65001);
    EXPECT_NE(findCodePage(chosen), nullptr);
    if (got != 0 && value <= 0xFFFF && findCodePage(static_cast<std::uint16_t>(value)) != nullptr) {
        EXPECT_EQ(chosen, value);
    } else {
        EXPECT_EQ(chosen, 1252);
    }
    EXPECT_EQ(frost::text::platformDefaultSeparator(), LineSeparator::CrLf);
}

#else

TEST(EncodingPlatform, GlibcRejectsWindowsOnlyC1) {
    for (const CodePageInfo& info : supportedCodePages()) {
        if (info.windowsOnlyC1.empty()) {
            continue;
        }
        const std::string from(info.iconvName);
        iconv_t cd = iconv_open("UTF-8", from.c_str());
        ASSERT_NE(cd, reinterpret_cast<iconv_t>(-1)) << from;
        for (const std::uint8_t b : info.windowsOnlyC1) {
            char in[1] = {static_cast<char>(b)};
            char out[8] = {};
            char* inPtr = in;
            char* outPtr = out;
            std::size_t inLeft = 1;
            std::size_t outLeft = sizeof(out);
            errno = 0;
            const std::size_t r = iconv(cd, &inPtr, &inLeft, &outPtr, &outLeft);
            EXPECT_EQ(r, static_cast<std::size_t>(-1))
                << from << " byte " << unsigned{b}
                << " now decodes in glibc: prune it from windowsOnlyC1 (ADR 0018, D4) after checking both OSes";
            EXPECT_EQ(errno, EILSEQ) << from << " byte " << unsigned{b};
            static_cast<void>(iconv(cd, nullptr, nullptr, nullptr, nullptr));
        }
        iconv_close(cd);
    }
}

TEST(EncodingPlatform, DefaultLegacyCodePageIs1252) {
    EXPECT_EQ(frost::text::defaultLegacyCodePage(), 1252);
    EXPECT_EQ(frost::text::platformDefaultSeparator(), LineSeparator::Lf);
}

#endif

} // namespace
