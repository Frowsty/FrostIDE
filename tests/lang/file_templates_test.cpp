// SPDX-License-Identifier: Apache-2.0
// File/project sets use actual supplied names and authors through the shared renderer.
#include "frost/lang/file_templates.hpp"

#include <gtest/gtest.h>
using namespace frost;
TEST(FileTemplates, QualifiedClassHasMatchingDeclarationImplementationAndLiteralAuthor) {
    auto files = lang::renderFileTemplate(
        {lang::FileTemplateKind::Class, "sample::Widget", "Ada ${1:literal}", "Application widget"});
    ASSERT_TRUE(files);
    ASSERT_EQ(files->size(), 2);
    EXPECT_EQ((*files)[0].name, "Widget.hpp");
    EXPECT_EQ((*files)[1].name, "Widget.cpp");
    EXPECT_NE((*files)[0].contents.find("namespace sample {\n/**"), std::string::npos);
    EXPECT_NE((*files)[0].contents.find("@author Ada ${1:literal}"), std::string::npos);
    EXPECT_NE((*files)[1].contents.find("sample::Widget::Widget() = default;"), std::string::npos);
    EXPECT_NE((*files)[1].contents.find("#include \"Widget.hpp\""), std::string::npos);
}
TEST(FileTemplates, InvalidIdentifiersPathsAndCommentBreakoutsRefuseBeforeFileEffects) {
    for (const auto* name : {"class", "../Widget", "a/b", "9Widget", "a::::b", "Widget;bad"})
        EXPECT_FALSE(lang::renderFileTemplate({lang::FileTemplateKind::Class, name, "", ""}));
    EXPECT_FALSE(lang::renderFileTemplate({lang::FileTemplateKind::Class, "Widget", "*/ bad", ""}));
    EXPECT_FALSE(lang::renderFileTemplate({lang::FileTemplateKind::Class, "Widget", "", "line\ncode"}));
}
TEST(FileTemplates, EveryFileKindRendersUtf8AndExecutableContainsARealEntryPoint) {
    for (const auto kind : {lang::FileTemplateKind::Source, lang::FileTemplateKind::Header,
                            lang::FileTemplateKind::CMakeScript, lang::FileTemplateKind::Executable}) {
        auto files = lang::renderFileTemplate({kind, "Sample", "Author", "Description"});
        ASSERT_TRUE(files);
        ASSERT_EQ(files->size(), 1);
        EXPECT_NE(files->front().contents.find("Author"), std::string::npos);
        if (kind == lang::FileTemplateKind::Executable)
            EXPECT_NE(files->front().contents.find("int main()"), std::string::npos);
    }
}
TEST(FileTemplatesLua, ModuleCreatesOneLuaFileAndRefusesReservedBindings) {
    auto files = lang::renderFileTemplate({lang::FileTemplateKind::LuaModule, "Module", "Author", "Description"});
    ASSERT_TRUE(files);
    ASSERT_EQ(files->size(), 1);
    EXPECT_EQ(files->front().name, "Module.lua");
    EXPECT_NE(files->front().contents.find("local Module = {}"), std::string::npos);
    for (const auto* name : {"class", "namespace", "template"}) {
        auto lua = lang::renderFileTemplate({lang::FileTemplateKind::LuaModule, name, "", ""});
        ASSERT_TRUE(lua);
        EXPECT_EQ(lua->front().name, std::string(name) + ".lua");
    }
    EXPECT_FALSE(lang::renderFileTemplate({lang::FileTemplateKind::LuaModule, "end", "", ""}));
}
