// SPDX-License-Identifier: Apache-2.0
//
// Tests for the strict UTF-8 <-> UTF-16 codec: round trips over every scalar
// value, boundary code points, and rejection of every class of ill-formed
// input (Unicode Standard table 3-7; unpaired surrogates).

#include "frost/io/utf.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using frost::ErrorCode;
using frost::io::isValidUtf8;
using frost::io::utf16ToUtf8;
using frost::io::utf8ToUtf16;

// Builds a std::string from raw bytes (string literals cannot hold every byte portably).
std::string bytes(std::initializer_list<unsigned char> list) {
    std::string out;
    for (const unsigned char b : list) {
        out += static_cast<char>(b);
    }
    return out;
}

// Encodes one scalar value with the textbook bit layout, as an oracle for the codec.
std::string encode(char32_t cp) {
    if (cp < 0x80) {
        return bytes({static_cast<unsigned char>(cp)});
    }
    if (cp < 0x800) {
        return bytes({static_cast<unsigned char>(0xC0 | (cp >> 6)), static_cast<unsigned char>(0x80 | (cp & 0x3F))});
    }
    if (cp < 0x10000) {
        return bytes({static_cast<unsigned char>(0xE0 | (cp >> 12)),
                      static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F)),
                      static_cast<unsigned char>(0x80 | (cp & 0x3F))});
    }
    return bytes({static_cast<unsigned char>(0xF0 | (cp >> 18)), static_cast<unsigned char>(0x80 | ((cp >> 12) & 0x3F)),
                  static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F)),
                  static_cast<unsigned char>(0x80 | (cp & 0x3F))});
}

TEST(Utf, AsciiAndEmptyRoundTrip) {
    EXPECT_EQ(utf8ToUtf16("").value(), u"");
    EXPECT_EQ(utf8ToUtf16("hello, world").value(), u"hello, world");
    EXPECT_EQ(utf16ToUtf8(u"hello, world").value(), "hello, world");
    // U+0000 is a valid scalar value and is converted, not treated as a terminator.
    const std::string withNul("a\0b", 3);
    EXPECT_EQ(utf8ToUtf16(withNul).value(), std::u16string(u"a\0b", 3));
}

TEST(Utf, MixedScriptsRoundTrip) {
    const std::string text =
        "ünïcødé 测试 😀 \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"; // Includes an emoji with a skin-tone modifier.
    const auto utf16 = utf8ToUtf16(text);
    ASSERT_TRUE(utf16.has_value());
    EXPECT_EQ(utf16ToUtf8(*utf16).value(), text);
    // U+1F600 is a surrogate pair in UTF-16.
    EXPECT_EQ(utf8ToUtf16("😀").value(), (std::u16string{char16_t{0xD83D}, char16_t{0xDE00}}));
}

TEST(Utf, BoundaryCodePoints) {
    for (const char32_t cp :
         {0x0u, 0x7Fu, 0x80u, 0x7FFu, 0x800u, 0xD7FFu, 0xE000u, 0xFFFDu, 0xFFFFu, 0x10000u, 0x10FFFFu}) {
        const std::string utf8 = encode(cp);
        const auto utf16 = utf8ToUtf16(utf8);
        ASSERT_TRUE(utf16.has_value()) << std::hex << static_cast<unsigned>(cp);
        EXPECT_EQ(utf16->size(), cp >= 0x10000 ? 2u : 1u);
        EXPECT_EQ(utf16ToUtf8(*utf16).value(), utf8) << std::hex << static_cast<unsigned>(cp);
    }
}

TEST(Utf, EveryScalarValueRoundTrips) {
    // All 1,112,064 scalar values in one string (about 4.3 MB of UTF-8).
    std::string all;
    for (char32_t cp = 0; cp <= 0x10FFFF; ++cp) {
        if (cp < 0xD800 || cp > 0xDFFF) {
            all += encode(cp);
        }
    }
    ASSERT_TRUE(isValidUtf8(all));
    const auto utf16 = utf8ToUtf16(all);
    ASSERT_TRUE(utf16.has_value());
    const auto back = utf16ToUtf8(*utf16);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, all);
}

TEST(Utf, RejectsIllFormedUtf8) {
    const std::string cases[] = {
        bytes({0x80}),                   // Lone continuation byte.
        bytes({0xBF}),                   // Lone continuation byte.
        bytes({0xC0, 0x80}),             // Overlong NUL.
        bytes({0xC1, 0xBF}),             // Overlong 2-byte form.
        bytes({0xE0, 0x80, 0x80}),       // Overlong 3-byte form.
        bytes({0xE0, 0x9F, 0xBF}),       // Overlong 3-byte form (U+07FF).
        bytes({0xF0, 0x80, 0x80, 0x80}), // Overlong 4-byte form.
        bytes({0xF0, 0x8F, 0xBF, 0xBF}), // Overlong 4-byte form (U+FFFF).
        bytes({0xED, 0xA0, 0x80}),       // Encoded high surrogate U+D800 (CESU-8 / WTF-8).
        bytes({0xED, 0xBF, 0xBF}),       // Encoded low surrogate U+DFFF.
        bytes({0xF4, 0x90, 0x80, 0x80}), // U+110000, above the Unicode range.
        bytes({0xF5, 0x80, 0x80, 0x80}), // Lead byte F5 never occurs.
        bytes({0xFF}),                   // Byte FF never occurs.
        bytes({0xFE}),                   // Byte FE never occurs.
        bytes({0xE2, 0x82}),             // Truncated 3-byte sequence (U+20AC without its last byte).
        bytes({0xF0, 0x9F, 0x98}),       // Truncated 4-byte sequence.
        bytes({0xC3, 0x41}),             // Lead byte followed by ASCII.
        bytes({0xE2, 0x28, 0xA1}),       // Bad second byte.
        bytes({0xE2, 0x82, 0x28}),       // Bad third byte.
    };
    for (const std::string& bad : cases) {
        EXPECT_FALSE(isValidUtf8(bad));
        const auto result = utf8ToUtf16(bad);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(Utf, ErrorReportsTheOffendingByteOffset) {
    const auto result = utf8ToUtf16("abc" + bytes({0xC0, 0xAF}) + "def");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("byte offset 3"), std::string::npos) << result.error().message();
}

TEST(Utf, RejectsUnpairedSurrogates) {
    const std::u16string cases[] = {
        std::u16string(1, char16_t{0xD800}),                // Lone high surrogate.
        std::u16string(1, char16_t{0xDFFF}),                // Lone low surrogate.
        std::u16string{char16_t{0xDC00}, char16_t{0xD800}}, // Reversed pair.
        std::u16string{u'a', char16_t{0xD83D}, u'b'},       // High surrogate followed by a non-surrogate.
        std::u16string{char16_t{0xD83D}, char16_t{0xD83D}}, // Two highs.
        std::u16string{u'x', char16_t{0xD83D}, char16_t{0xDE00}, char16_t{0xDE00}}, // Pair, then a stray low.
    };
    for (const std::u16string& bad : cases) {
        const auto result = utf16ToUtf8(bad);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    }
    const auto located = utf16ToUtf8(std::u16string{u'a', u'b', char16_t{0xDC00}});
    ASSERT_FALSE(located.has_value());
    EXPECT_NE(located.error().message().find("offset 2"), std::string::npos) << located.error().message();
}

#ifdef _WIN32
TEST(Utf, WideWrappersMatchTheUtf16Functions) {
    const std::string text = "C:\\Users\\ünï\\测试 😀.txt";
    const auto wide = frost::io::toWide(text);
    ASSERT_TRUE(wide.has_value());
    EXPECT_EQ(*wide, L"C:\\Users\\ünï\\测试 😀.txt");
    EXPECT_EQ(frost::io::fromWide(*wide).value(), text);
    EXPECT_FALSE(frost::io::fromWide(std::wstring(1, wchar_t{0xD800})).has_value());
    EXPECT_FALSE(frost::io::toWide(bytes({0xED, 0xA0, 0x80})).has_value());
}
#endif

} // namespace
