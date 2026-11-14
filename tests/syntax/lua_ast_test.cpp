// SPDX-License-Identifier: Apache-2.0
#include "syntax_test_support.hpp"
#include <gtest/gtest.h>
#include <algorithm>
using namespace frost;
TEST(LuaAst, OwnsActualGrammarNodesFieldsAndAnonymousLocalToken) {
    auto tree=syntax::testing::parseNow(syntax::LanguageId::Lua,"local value = 4\nprint(value)\n");
    auto ast=tree.luaAstProjection(); ASSERT_TRUE(ast); ASSERT_FALSE(ast->nodes.empty());
    EXPECT_EQ(ast->nodes.front().kind,"chunk");
    bool declaration=false,local=false,reference=false;
    for(std::size_t i=0;i<ast->nodes.size();++i) {
        const auto& node=ast->nodes[i];
        declaration|=node.kind=="variable_declaration";
        reference|=node.kind=="identifier" && node.field=="name";
        for(const auto& token:node.tokens) local|=token.kind=="local";
        for(auto child:node.children) { ASSERT_LT(child,ast->nodes.size()); EXPECT_EQ(ast->nodes[child].parent,i); }
    }
    EXPECT_TRUE(declaration); EXPECT_TRUE(local); EXPECT_TRUE(reference);
    tree={}; EXPECT_EQ(ast->snapshot.size().value(),std::string_view("local value = 4\nprint(value)\n").size());
}
TEST(LuaAst, RefusesWrongLanguageErrorsCancellationAndNodeBudget) {
    EXPECT_FALSE(syntax::testing::parseNow(syntax::LanguageId::Cpp,"int x;").luaAstProjection());
    EXPECT_FALSE(syntax::testing::parseNow(syntax::LanguageId::Lua,"local =").luaAstProjection());
    auto tree=syntax::testing::parseNow(syntax::LanguageId::Lua,"local x = 1\nprint(x)\n");
    syntax::LuaAstLimits limits; limits.maxNodes=1; EXPECT_FALSE(tree.luaAstProjection(limits));
    std::stop_source stopped; stopped.request_stop(); limits={}; limits.stop=stopped.get_token();
    auto result=tree.luaAstProjection(limits); ASSERT_FALSE(result); EXPECT_EQ(result.error().code(),ErrorCode::Cancelled);
}
