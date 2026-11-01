// SPDX-License-Identifier: Apache-2.0
//
// frost::syntax languages (ADR 0023): the file-name table, the ABI check, compilation of every
// embedded query with its vocabulary and predicate validation, and our UTF-8 and UTF-16 decoders
// (D2) against an independent encoder.

#include "frost/syntax/language.hpp"

#include "c/frost_ts_unicode.h"
#include "language_state.hpp"
#include "syntax/syntax_test_support.hpp"
#include "ts.hpp"
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::syntax::languageForFileName;
using frost::syntax::LanguageId;
using frost::syntax::LanguageRegistry;

TEST(SyntaxLanguage, FileNameTable) {
    EXPECT_EQ(languageForFileName("main.c"), LanguageId::C);
    EXPECT_EQ(languageForFileName("widget.CPP"), LanguageId::Cpp);
    EXPECT_EQ(languageForFileName("src/x/header.h"), LanguageId::Cpp);
    EXPECT_EQ(languageForFileName("C:\\dev\\a.hpp"), LanguageId::Cpp);
    EXPECT_EQ(languageForFileName("lib.c++"), LanguageId::Cpp);
    EXPECT_EQ(languageForFileName("Program.cs"), LanguageId::CSharp);
    EXPECT_EQ(languageForFileName("CMakeLists.txt"), LanguageId::CMake);
    EXPECT_EQ(languageForFileName("cmakelists.TXT"), LanguageId::CMake);
    EXPECT_EQ(languageForFileName("FrostLayers.cmake"), LanguageId::CMake);
    EXPECT_EQ(languageForFileName("config.cmake.in"), LanguageId::CMake);
    EXPECT_EQ(languageForFileName("app.vcxproj"), LanguageId::Xml);
    EXPECT_EQ(languageForFileName("FrostIDE.slnx"), LanguageId::Xml);
    EXPECT_EQ(languageForFileName("frost.natvis"), LanguageId::Xml);
    EXPECT_EQ(languageForFileName("package.json"), LanguageId::Json);
    EXPECT_EQ(languageForFileName("settings.jsonc"), LanguageId::Json);
    EXPECT_EQ(languageForFileName("README.md"), LanguageId::Markdown);
    EXPECT_EQ(languageForFileName("notes.txt"), LanguageId::None);
    EXPECT_EQ(languageForFileName(".gitignore"), LanguageId::None);
    EXPECT_EQ(languageForFileName("Makefile"), LanguageId::None);
    EXPECT_EQ(languageForFileName("file.in"), LanguageId::None);
    EXPECT_EQ(languageForFileName(""), LanguageId::None);
    EXPECT_EQ(frost::syntax::toString(LanguageId::CSharp), "csharp");
    EXPECT_EQ(frost::syntax::toString(LanguageId::None), "none");
}

TEST(SyntaxLanguage, AbiInSupportedRange) {
    const LanguageRegistry registry;
    for (const LanguageId id : frost::syntax::kAllLanguages) {
        const frost::Result<std::uint32_t> abi = registry.abiVersion(id);
        ASSERT_TRUE(abi.has_value()) << frost::syntax::toString(id) << ": " << abi.error().describe();
        EXPECT_GE(*abi, static_cast<std::uint32_t>(TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION));
        EXPECT_LE(*abi, static_cast<std::uint32_t>(TREE_SITTER_LANGUAGE_VERSION));
    }
    const frost::Result<const frost::syntax::Language*> none = registry.get(LanguageId::None);
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().code(), ErrorCode::InvalidArgument);
}

TEST(SyntaxLanguage, AllQueriesCompile) {
    const LanguageRegistry registry;
    for (const LanguageId id : frost::syntax::kAllLanguages) {
        EXPECT_FALSE(registry.queriesReady(id));
        const frost::Result<void> prepared = registry.prepare(id);
        EXPECT_TRUE(prepared.has_value()) << frost::syntax::toString(id) << ": " << prepared.error().describe();
        EXPECT_TRUE(registry.queriesReady(id)) << frost::syntax::toString(id);
        // A second call returns the first result without compiling again.
        EXPECT_TRUE(registry.prepare(id).has_value());
    }
}

TEST(SyntaxLanguage, UnknownCapturesAndPredicatesAreRejected) {
    using frost::syntax::detail::compileQuery;
    using frost::syntax::detail::QueryKind;
    const TSLanguage* json = tree_sitter_json();
    EXPECT_TRUE(compileQuery(json, "(number) @number", QueryKind::Highlights, "test").has_value());
    // A capture outside the vocabulary.
    const auto badCapture = compileQuery(json, "(number) @numeral", QueryKind::Highlights, "test");
    ASSERT_FALSE(badCapture.has_value());
    EXPECT_EQ(badCapture.error().code(), ErrorCode::Corrupt);
    // "_" captures feed predicates only.
    EXPECT_TRUE(compileQuery(json, "((number) @_n (#eq? @_n \"1\"))", QueryKind::Highlights, "test").has_value());
    // #match? (regular expressions) is not supported.
    const auto regex = compileQuery(json, "((number) @number (#match? @number \"^1\"))", QueryKind::Highlights, "test");
    ASSERT_FALSE(regex.has_value());
    EXPECT_EQ(regex.error().code(), ErrorCode::Corrupt);
    // A node type the grammar does not have.
    const auto badNode = compileQuery(json, "(numbr) @number", QueryKind::Highlights, "test");
    ASSERT_FALSE(badNode.has_value());
    EXPECT_NE(badNode.error().message().find("TSQueryErrorNodeType"), std::string::npos);
    // Outline captures need a known kind.
    EXPECT_TRUE(compileQuery(json, "(pair) @outline.key", QueryKind::Outline, "test").has_value());
    EXPECT_FALSE(compileQuery(json, "(pair) @outline.thing", QueryKind::Outline, "test").has_value());
}

// --- Our decoders (ADR 0023 D2), checked against a trivial encoder ---

std::vector<std::uint8_t> encodeUtf8(std::uint32_t c) {
    if (c < 0x80) {
        return {static_cast<std::uint8_t>(c)};
    }
    if (c < 0x800) {
        return {static_cast<std::uint8_t>(0xC0 | (c >> 6)), static_cast<std::uint8_t>(0x80 | (c & 0x3F))};
    }
    if (c < 0x10000) {
        return {static_cast<std::uint8_t>(0xE0 | (c >> 12)), static_cast<std::uint8_t>(0x80 | ((c >> 6) & 0x3F)),
                static_cast<std::uint8_t>(0x80 | (c & 0x3F))};
    }
    return {static_cast<std::uint8_t>(0xF0 | (c >> 18)), static_cast<std::uint8_t>(0x80 | ((c >> 12) & 0x3F)),
            static_cast<std::uint8_t>(0x80 | ((c >> 6) & 0x3F)), static_cast<std::uint8_t>(0x80 | (c & 0x3F))};
}

TEST(SyntaxUnicode, EveryScalarValueRoundTrips) {
    for (std::uint32_t c = 0; c <= 0x10FFFF; ++c) {
        if (c >= 0xD800 && c <= 0xDFFF) {
            continue;
        }
        const std::vector<std::uint8_t> bytes = encodeUtf8(c);
        std::int32_t decoded = 0;
        const std::uint32_t used = ts_decode_utf8(bytes.data(), static_cast<std::uint32_t>(bytes.size()), &decoded);
        ASSERT_EQ(used, bytes.size()) << c;
        ASSERT_EQ(decoded, static_cast<std::int32_t>(c));
    }
}

TEST(SyntaxUnicode, IllFormedUtf8ConsumesMaximalSubparts) {
    // Unicode 16.0 Table 3-8's example: 61 F1 80 80 E1 80 C2 62 80 63 80 BF 64 decodes to
    // a, FFFD, FFFD, FFFD, b, FFFD, c, FFFD, FFFD, d.
    const std::array<std::uint8_t, 13> text = {0x61, 0xF1, 0x80, 0x80, 0xE1, 0x80, 0xC2,
                                               0x62, 0x80, 0x63, 0x80, 0xBF, 0x64};
    std::vector<std::int32_t> decoded;
    std::uint32_t at = 0;
    while (at < text.size()) {
        std::int32_t c = 0;
        const std::uint32_t used = ts_decode_utf8(text.data() + at, static_cast<std::uint32_t>(text.size() - at), &c);
        ASSERT_GE(used, 1u);
        decoded.push_back(c);
        at += used;
    }
    const std::vector<std::int32_t> expected = {0x61, -1, -1, -1, 0x62, -1, 0x63, -1, -1, 0x64};
    EXPECT_EQ(decoded, expected);
    // Surrogates, overlongs and values past U+10FFFF are ill-formed.
    for (const std::array<std::uint8_t, 3> bad :
         {std::array<std::uint8_t, 3>{0xED, 0xA0, 0x80}, std::array<std::uint8_t, 3>{0xE0, 0x80, 0x80},
          std::array<std::uint8_t, 3>{0xF4, 0x90, 0x80}}) {
        std::int32_t c = 0;
        EXPECT_EQ(ts_decode_utf8(bad.data(), 3, &c), 1u);
        EXPECT_EQ(c, TS_DECODE_ERROR);
    }
    std::int32_t c = 0;
    const std::array<std::uint8_t, 2> overlong = {0xC0, 0xAF};
    EXPECT_EQ(ts_decode_utf8(overlong.data(), 2, &c), 1u);
    EXPECT_EQ(c, TS_DECODE_ERROR);
}

TEST(SyntaxUnicode, TruncatedSequenceAsksForMoreInput) {
    const std::vector<std::uint8_t> euro = encodeUtf8(0x20AC); // E2 82 AC
    std::int32_t c = 0;
    // Cut after two bytes: an error that consumed fewer than four bytes (the lexer re-reads).
    EXPECT_EQ(ts_decode_utf8(euro.data(), 2, &c), 2u);
    EXPECT_EQ(c, TS_DECODE_ERROR);
    EXPECT_EQ(ts_decode_utf8(euro.data(), 0, &c), 0u);
}

TEST(SyntaxUnicode, Utf16BothByteOrders) {
    std::int32_t c = 0;
    // U+1F600 is D83D DE00.
    const std::array<std::uint8_t, 4> le = {0x3D, 0xD8, 0x00, 0xDE};
    const std::array<std::uint8_t, 4> be = {0xD8, 0x3D, 0xDE, 0x00};
    EXPECT_EQ(ts_decode_utf16_le(le.data(), 4, &c), 4u);
    EXPECT_EQ(c, 0x1F600);
    EXPECT_EQ(ts_decode_utf16_be(be.data(), 4, &c), 4u);
    EXPECT_EQ(c, 0x1F600);
    // A lone high surrogate, a lone low surrogate and an odd trailing byte are errors.
    EXPECT_EQ(ts_decode_utf16_be(be.data(), 2, &c), 2u);
    EXPECT_EQ(c, TS_DECODE_ERROR);
    EXPECT_EQ(ts_decode_utf16_be(be.data() + 2, 2, &c), 2u);
    EXPECT_EQ(c, TS_DECODE_ERROR);
    EXPECT_EQ(ts_decode_utf16_le(le.data(), 1, &c), 1u);
    EXPECT_EQ(c, TS_DECODE_ERROR);
    const std::array<std::uint8_t, 2> letter = {0x41, 0x00};
    EXPECT_EQ(ts_decode_utf16_le(letter.data(), 2, &c), 2u);
    EXPECT_EQ(c, 0x41);
}

} // namespace
