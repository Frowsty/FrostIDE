// SPDX-License-Identifier: Apache-2.0
// Actual pinned tree-sitter AST oracles, never a mirrored text parser.
#include "frost/lang/lua_refactoring.hpp"
#include "syntax/syntax_test_support.hpp"
#include <gtest/gtest.h>
using namespace frost;
namespace {
Result<lang::LuaRefactoringProposal> inlineAt(std::string_view source,std::size_t offset) {
    return lang::planLuaInlineVariable(syntax::testing::parseNow(syntax::LanguageId::Lua,source),ByteOffset{offset});
}
std::string apply(std::string source,const lang::LuaRefactoringProposal& plan) {
    for(auto it=plan.edits.rbegin();it!=plan.edits.rend();++it)
        source.replace(it->range.begin.value(),it->range.end.value()-it->range.begin.value(),it->replacement);
    return source;
}
}
TEST(LuaInline, ResolvesShadowingInitializerAndMemberNamesFromActualAst) {
    const std::string source="local value = 4\ndo\n    local value = value\n    print(value)\nend\nprint(value, object.value, {value = 7})\n";
    auto plan=inlineAt(source,source.find("value")); ASSERT_TRUE(plan);
    const auto after=apply(source,*plan);
    EXPECT_NE(after.find("local value = (4)"),after.npos);
    EXPECT_NE(after.find("print(value)"),after.npos);
    EXPECT_NE(after.find("print((4), object.value, {value = 7})"),after.npos);
    EXPECT_FALSE(syntax::testing::parseNow(syntax::LanguageId::Lua,after).hasError());
}
TEST(LuaInline, RefusesWritesAliasesAllocationsMultivalueAndExtendedBindingProfiles) {
    for(const auto* source:{"local x=1\nx=2\nprint(x)","local x=other\nprint(x)","local x={}\nprint(x)",
        "local x,y=1,2\nprint(x)","local x=call()\nprint(x)","local x <close> = 1\nprint(x)",
        "local x=1\nlocal function use() return x end","local x=1\n_ENV={}\nprint(x)",
        "repeat\nlocal x=1\nprint(x)\nuntil x==1"})
        EXPECT_FALSE(inlineAt(source,std::string_view(source).find('x'))) << source;
}
TEST(LuaExtract, CapturesExistingVariablesByReferenceAndReturnsEscapingLocals) {
    const std::string source="local total=1\nlocal value=total+2\ntotal=value*2\nprint(value,total)\n";
    const auto begin=source.find("local value"); const auto end=source.find("\nprint");
    auto plan=lang::planLuaExtractFunction(syntax::testing::parseNow(syntax::LanguageId::Lua,source),
        {ByteOffset{begin},ByteOffset{end}},"calculate"); ASSERT_TRUE(plan);
    const auto after=apply(source,*plan);
    EXPECT_NE(after.find("local function calculate()"),after.npos);
    EXPECT_NE(after.find("return value"),after.npos);
    EXPECT_NE(after.find("local value = calculate()"),after.npos);
    EXPECT_NE(after.find("total=value*2"),after.npos);
    EXPECT_FALSE(syntax::testing::parseNow(syntax::LanguageId::Lua,after).hasError());
}
TEST(LuaExtract, RefusesPartialStatementsControlTransferNameCaptureAndMultivalue) {
    for(const auto* source:{"return 42","if flag then print(1) end","local a,b=call()","print(debug.traceback())"}) {
        auto tree=syntax::testing::parseNow(syntax::LanguageId::Lua,source);
        EXPECT_FALSE(lang::planLuaExtractFunction(tree,{ByteOffset{0},ByteOffset{std::string_view(source).size()}},"extracted"));
    }
    const std::string source="local name=1\nprint(name)"; auto tree=syntax::testing::parseNow(syntax::LanguageId::Lua,source);
    EXPECT_FALSE(lang::planLuaExtractFunction(tree,{ByteOffset{13},ByteOffset{source.size()}},"name"));
    EXPECT_FALSE(lang::planLuaExtractFunction(tree,{ByteOffset{15},ByteOffset{source.size()}},"extract"));
}
TEST(LuaExtract, BoundsIndentAmplificationBeforeAllocatingTheExpandedBody) {
    const std::string indent(65536,' ');
    const std::string source=indent+"print(1)"+std::string(200,'\n')+"print(2)";
    auto result=lang::planLuaExtractFunction(syntax::testing::parseNow(syntax::LanguageId::Lua,source),
        {ByteOffset{indent.size()},ByteOffset{source.size()}},"extract");
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code(),ErrorCode::OutOfRange);
}
