// SPDX-License-Identifier: Apache-2.0
// Shared authored templates use real snippet offsets, literal inputs and linked-placeholder edits.
#include "frost/lang/templates.hpp"

#include <gtest/gtest.h>
using namespace frost;
TEST(Templates, ContextValuesCannotBecomeNewSnippetInstructions) {
    auto result = lang::expandTemplate("// ${AUTHOR}\n${1:name} = $0", {{"AUTHOR", "${1:injected} \\ literal"}});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->parsed.text, "// ${1:injected} \\ literal\nname = ");
    ASSERT_EQ(result->parsed.tabStops.size(), 2);
    EXPECT_EQ(result->parsed.tabStops.front().index, 1);
    EXPECT_FALSE(lang::expandTemplate("${AUTHOR}", {}));
}
TEST(Templates, CMakeRuntimeVariablesRemainLiteralAndCatalogueUsesOneRenderer) {
    const auto* definition = lang::findTemplate("cmake.library");
    ASSERT_NE(definition, nullptr);
    auto rendered = lang::expandTemplate(definition->body, {});
    ASSERT_TRUE(rendered);
    EXPECT_NE(rendered->parsed.text.find("${CMAKE_CURRENT_SOURCE_DIR}"), std::string::npos);
    auto files = lang::expandTemplate(lang::findTemplate("file.cpp.class.header")->body,
                                      {{"CLASS", "Widget"}, {"AUTHOR", "Ada"}, {"DESCRIPTION", "Application widget"}});
    ASSERT_TRUE(files);
    EXPECT_NE(files->parsed.text.find("class Widget"), std::string::npos);
    EXPECT_NE(files->parsed.text.find("@author Ada"), std::string::npos);
}
TEST(Templates, LinkedPlaceholdersPropagateOnlyWithinTheirExactActiveBoundary) {
    lang::LiveTemplateSession session{lang::parseSnippet("for(${1:i}; ${1:i}<${2:n};++${1:i}){$0}"), 0};
    auto advanced = lang::advanceTemplate(session, "for(index; i<n;++i){}", 9);
    ASSERT_TRUE(advanced);
    EXPECT_EQ(advanced->next.expected.text, "for(index; index<n;++index){}");
    ASSERT_EQ(advanced->edits.size(), 2);
    ASSERT_TRUE(advanced->selection);
    EXPECT_EQ(advanced->selection->index, 2);
    EXPECT_FALSE(lang::advanceTemplate(session, "wrong(i; i<n;++i){}", 5));
    EXPECT_FALSE(lang::advanceTemplate(session, "for(index; i<n;++i){}", 19));
}
TEST(Templates, BoundedVariablesAndDeepPlaceholderInputsFailBeforeParsing) {
    EXPECT_FALSE(lang::expandTemplate("${AUTHOR}", {{"AUTHOR", std::string(65537, 'x')}}));
    std::string deep;
    for (int i = 0; i < 65; ++i)
        deep += "${1:";
    deep += "body";
    deep += std::string(65, '}');
    EXPECT_FALSE(lang::expandTemplate(deep, {}));
    std::string unfinished;
    for (int i = 0; i < 40; ++i)
        unfinished += "${1:";
    EXPECT_FALSE(lang::expandTemplate(unfinished, {}));
    auto parsed = lang::parseSnippet(unfinished);
    EXPECT_TRUE(parsed.budgetExceeded);
    EXPECT_EQ(parsed.text, unfinished);
    EXPECT_TRUE(parsed.tabStops.empty());
    EXPECT_FALSE(lang::expandTemplate("${A}${A}\\x", {{"A", std::string(65536, '$')}}));
}
TEST(Templates, MirrorAggregateOutputIsAdmittedBeforeCopyingLargeValues) {
    std::string raw;
    for (int i = 0; i < 4096; ++i)
        raw += "$1";
    lang::LiveTemplateSession session{lang::parseSnippet(raw), 0};
    EXPECT_FALSE(lang::advanceTemplate(session, std::string(65536, 'x'), 65536));
}
TEST(Templates, PostfixForAndIfHaveRealFinalCaretsAndDoNotReparseOperands) {
    for (const auto& id : {"cpp.postfix.for", "cpp.postfix.if", "cmake.postfix.for", "cmake.postfix.if"}) {
        const auto* definition = lang::findTemplate(id);
        ASSERT_NE(definition, nullptr);
        auto rendered = lang::expandTemplate(definition->body, {{"EXPR", "items"}});
        ASSERT_TRUE(rendered);
        EXPECT_NE(rendered->parsed.text.find("items"), std::string::npos);
        EXPECT_EQ(rendered->parsed.tabStops.back().index, 0);
    }
}
TEST(Templates, AdjacentEmptyPlaceholdersHaveRightInsertionAffinity) {
    lang::LiveTemplateSession session{lang::parseSnippet("$1$0"), 0};
    auto final = lang::advanceTemplate(session, "x", 1);
    ASSERT_TRUE(final);
    ASSERT_TRUE(final->selection);
    EXPECT_EQ(final->selection->begin, 1);
    EXPECT_EQ(final->selection->end, 1);
    session = {lang::parseSnippet("$1${2:value}"), 0};
    auto next = lang::advanceTemplate(session, "xvalue", 1);
    ASSERT_TRUE(next);
    ASSERT_TRUE(next->selection);
    EXPECT_EQ(next->selection->begin, 1);
    EXPECT_EQ(next->selection->end, 6);
    session = {lang::parseSnippet("$1$1$0"), 0};
    auto mirrored = lang::advanceTemplate(session, "x", 1);
    ASSERT_TRUE(mirrored);
    ASSERT_TRUE(mirrored->selection);
    EXPECT_EQ(mirrored->next.expected.text, "xx");
    EXPECT_EQ(mirrored->selection->begin, 2);
    EXPECT_EQ(mirrored->selection->end, 2);
}
TEST(TemplatesLua, CatalogueHasLuaStructuresAndFileModuleUsesActualAuthor) {
    for (const auto* id :
         {"lua.function", "lua.if", "lua.for", "lua.pairs", "lua.ipairs", "lua.repeat", "lua.module", "lua.pcall"}) {
        const auto* definition = lang::findTemplate(id);
        ASSERT_NE(definition, nullptr);
        EXPECT_EQ(definition->language, "lua");
        auto expanded = lang::expandTemplate(definition->body, {});
        ASSERT_TRUE(expanded);
        EXPECT_FALSE(expanded->parsed.tabStops.empty());
    }
    auto module =
        lang::expandTemplate(lang::findTemplate("file.lua.module")->body,
                             {{"CLASS", "Module"}, {"AUTHOR", "Actual Author"}, {"DESCRIPTION", "Actual module"}});
    ASSERT_TRUE(module);
    EXPECT_NE(module->parsed.text.find("@author Actual Author"), std::string::npos);
    EXPECT_NE(module->parsed.text.find("return Module"), std::string::npos);
}
