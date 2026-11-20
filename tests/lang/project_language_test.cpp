// SPDX-License-Identifier: Apache-2.0
// R16 pure source/protocol cases; synthetic models are not real configure/evaluation evidence.
#include "frost/lang/msbuild_language.hpp"
#include "frost/lang/cmake_refactoring.hpp"
#include "frost/text/text_buffer.hpp"
#include <gtest/gtest.h>
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
namespace frost::lang {
namespace {
std::string projectPath(std::string_view leaf) {
    return io::joinPath(io::kNativePathStyle==io::PathStyle::Windows ? "C:/project" : "/project",leaf).value();
}
Json position(unsigned line,unsigned character) { return Json::object().set("position",toJson(LspPosition{line,character})); }
TEST(MSBuildLanguage, ConditionsAndDynamicImportsRemainSourceOnly) {
    const std::string source="<Project><PropertyGroup Condition=\"'$(Configuration)'=='Debug'\"><Feature>yes</Feature></PropertyGroup>"
        "<Import Project=\"$(Unknown)/missing.props\" /></Project>";
    auto parsed=project::parseMSBuildSource(source,projectPath("test.props"));
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->properties.size(),1U);
    EXPECT_FALSE(parsed->properties[0].condition.empty());
    ASSERT_EQ(parsed->imports.size(),1U);
    EXPECT_FALSE(parsed->imports[0].literal);
    MSBuildLanguageInput input;
    input.path=projectPath("test.props"); input.source=text::TextBuffer::fromUtf8(source).value().snapshot();
    auto completion=queryMSBuildLanguage(input,"textDocument/completion",position(0,0));
    ASSERT_TRUE(completion);
    bool sourceOnly=false;
    for (const auto& row:completion->items()) if (row.stringAt("label")=="Feature")
        sourceOnly=row.stringAt("detail").find("not evaluated")!=std::string_view::npos;
    EXPECT_TRUE(sourceOnly);
}
TEST(MSBuildLanguage, EngineOriginWinsOverInactiveSourceAssignments) {
    MSBuildLanguageInput input;
    input.path=projectPath("main.props");
    input.source=text::TextBuffer::fromUtf8("<Project>\n<PropertyGroup><Chosen>$(Flag)</Chosen></PropertyGroup>\n</Project>\n").value().snapshot();
    input.inputs={{projectPath("import.targets"),"<Project>\n<PropertyGroup><Flag>engine</Flag></PropertyGroup>\n</Project>"}};
    project::EvaluatedProjectItems evaluated;
    evaluated.projectPath=projectPath("main.props"); evaluated.configuration="Debug"; evaluated.platform="x64";
    evaluated.propertyDefinitions.push_back({"Flag","engine",projectPath("import.targets"),2,17});
    input.evaluations.push_back(evaluated);
    auto definition=queryMSBuildLanguage(input,"textDocument/definition",position(1,28));
    ASSERT_TRUE(definition);
    ASSERT_EQ(definition->size(),1U);
    EXPECT_EQ(definition->items()[0].stringAt("uri"),pathToUri(projectPath("import.targets")).value());
    input.evaluations[0].propertyDefinitions[0].line=999;
    auto stale=queryMSBuildLanguage(input,"textDocument/definition",position(1,28));
    ASSERT_TRUE(stale);
    // Current source declaration may be offered, but the stale engine origin is discarded.
    for (const auto& row:stale->items()) EXPECT_NE(row.find("range")->find("start")->integerAt("line"),998);
}
CMakeLanguageInput cmakeInput(std::string source) {
    CMakeLanguageInput input;
    input.path=projectPath("CMakeLists.txt");
    input.source=text::TextBuffer::fromUtf8(std::move(source)).value().snapshot();
    auto model=std::make_shared<project::CMakeProject>();
    model->sourceDirectory=projectPath(""); model->inputsAvailable=true;
    model->inputs.push_back({input.path});
    project::CMakeTarget target; target.name="alpha";
    target.backtrace.push_back({input.path,1,"add_custom_target"});
    model->targets.push_back(target); input.model=model;
    return input;
}
TEST(CMakeRefactoring, RewritesOnlyTargetSlotsAndPreservesQuotedSpelling) {
    auto input=cmakeInput("add_custom_target(alpha)\nadd_custom_target(consumer)\nadd_dependencies(consumer \"alpha\")\n");
    auto plan=planCMakeTargetRename(input,{0,19},"bravo");
    ASSERT_TRUE(plan) << plan.error().describe();
    ASSERT_EQ(plan->edit.files.size(),1U);
    auto changed=applyTextEdits(input.source.text(),plan->edit.files[0].edits,PositionEncoding::Utf16);
    ASSERT_TRUE(changed);
    EXPECT_NE(changed->find("add_custom_target(bravo)"),std::string::npos);
    EXPECT_NE(changed->find("consumer \"bravo\""),std::string::npos);
}
TEST(CMakeRefactoring, DynamicAndUnsupportedTargetUsesRefuseTheEntirePlan) {
    for (const auto* extra:{"set(NAME alpha)\n", "target_link_libraries(alpha ${OTHER})\n", "custom_command(alpha)\n"}) {
        auto input=cmakeInput(std::string("add_custom_target(alpha)\n")+extra);
        EXPECT_FALSE(planCMakeTargetRename(input,{0,19},"bravo"));
    }
}
TEST(CMakeRefactoring, SearchOptionalAndConditionalImportsRefuseWithoutGuessing) {
    for (const auto* text:{"include(extra.cmake)\n","include(/outside/absent.cmake OPTIONAL)\n",
                          "if(ON)\ninclude(/outside/file.cmake)\nendif()\n"}) {
        auto input=cmakeInput(std::string("add_custom_target(alpha)\n")+text);
        EXPECT_FALSE(planCMakeTargetRename(input,{0,19},"bravo"));
    }
}
TEST(CMakeRefactoring, LinkGrammarKeywordsAreRefusedAsOldOrNewTargetNames) {
    auto input=cmakeInput("add_custom_target(alpha)\ntarget_link_libraries(consumer PUBLIC alpha)\n");
    for (const auto* name:{"PUBLIC","PRIVATE","INTERFACE","debug","optimized","general"})
        EXPECT_FALSE(planCMakeTargetRename(input,{0,19},name));
    auto old=cmakeInput("add_custom_target(PUBLIC)\ntarget_link_libraries(consumer PUBLIC PUBLIC)\n");
    auto model=std::make_shared<project::CMakeProject>(*old.model);
    model->targets[0].name="PUBLIC"; old.model=model;
    EXPECT_FALSE(planCMakeTargetRename(old,{0,19},"bravo"));
}
TEST(CMakeRefactoring, GeneratedDynamicMetadataIsReadOnlyCapturedEvidence) {
    auto input=cmakeInput("add_custom_target(alpha)\n");
    auto model=std::make_shared<project::CMakeProject>(*input.model);
    model->inputs.push_back({projectPath("../build/CMakeSystem.cmake"),true,false,false}); input.model=model;
    input.inputs.emplace_back(projectPath("../build/CMakeSystem.cmake"),"set(CMAKE_HOST_SYSTEM_NAME \"${HOST}\")\n");
    auto plan=planCMakeTargetRename(input,{0,19},"bravo");
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->inputs.size(),2U);
    EXPECT_EQ(plan->edit.files.size(),1U);
}
TEST(CMakeRefactoring, ParserInspectionsReportConcreteSourceFacts) {
    auto source=text::TextBuffer::fromUtf8("add_custom_target(alpha)\nadd_custom_target(alpha)\n").value();
    auto diagnostics=inspectCMakeSource(projectPath("CMakeLists.txt"),source.snapshot());
    ASSERT_EQ(diagnostics.size(),1U);
    EXPECT_EQ(diagnostics[0].code,"cmake.duplicate-target");
    EXPECT_EQ(diagnostics[0].line,2);
}
} // namespace
} // namespace frost::lang
