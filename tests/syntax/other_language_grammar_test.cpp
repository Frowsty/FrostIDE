// SPDX-License-Identifier: Apache-2.0
// Authored actual tree-sitter parse/query regressions for the audited generated grammar recipes.
#include "frost/syntax/language.hpp"
#include "frost/syntax/syntax_tree.hpp"

#include "syntax/syntax_test_support.hpp"
#include <gtest/gtest.h>
namespace frost::syntax {
TEST(OtherLanguageGrammars, ExplicitFileProfilesSelectActualGrammar) {
    EXPECT_EQ(languageForFileName("main.py"), LanguageId::Python);
    EXPECT_EQ(languageForFileName("types.pyi"), LanguageId::Python);
    EXPECT_EQ(languageForFileName("main.mjs"), LanguageId::JavaScript);
    EXPECT_EQ(languageForFileName("main.ts"), LanguageId::TypeScript);
    EXPECT_EQ(languageForFileName("view.tsx"), LanguageId::Tsx);
    EXPECT_EQ(languageForFileName("data.YAML"), LanguageId::Yaml);
    EXPECT_EQ(languageForFileName("settings.toml"), LanguageId::Toml);
    EXPECT_EQ(languageForFileName("module.psm1"), LanguageId::PowerShell);
}
TEST(OtherLanguageGrammars, UpstreamNodesDriveCommentsStringsAndSourceOutline) {
    const LanguageRegistry registry;
    for (auto language : {LanguageId::Python, LanguageId::JavaScript, LanguageId::TypeScript, LanguageId::Tsx,
                          LanguageId::Yaml, LanguageId::Toml, LanguageId::PowerShell}) {
        ASSERT_TRUE(registry.prepare(language)) << toString(language);
        auto tree = testing::parseNow(language, testing::sample(language));
        ASSERT_FALSE(tree.empty()) << toString(language);
        auto spans =
            tree.highlights({ByteOffset{0}, ByteOffset{testing::sample(language).size()}}, testing::unlimited());
        EXPECT_TRUE(spans.complete);
        EXPECT_FALSE(spans.spans.empty()) << toString(language);
    }
    auto python = testing::parseNow(LanguageId::Python, "def greet(name):\n    return \"hello\"\n");
    EXPECT_EQ(testing::kindAt(python, "\"hello\""), "string");
    EXPECT_NE(testing::outlineText(python).find("function greet"), std::string::npos);
    auto powershell = testing::parseNow(LanguageId::PowerShell, "function Get-Value { return 42 }\n");
    EXPECT_NE(testing::outlineText(powershell).find("function Get-Value"), std::string::npos);
}
} // namespace frost::syntax
