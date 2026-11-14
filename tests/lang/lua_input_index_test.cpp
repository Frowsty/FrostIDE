// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/lua_input_index.hpp"
#include <gtest/gtest.h>
#include <vector>
using namespace frost;
TEST(LuaInputIndex, ExactDescendantAncestorAndBoundarySemanticsSurviveReset) {
    lang::LuaInputIndex index;
    std::vector<std::string> inputs{"/work/defs","/work/config/.luarc.json","C:\\work\\meta"}; index.reset(inputs);
    for(const auto* path:{"/work/defs","/work/defs/a.lua","/work","/work/config","C:\\work\\meta\\a.lua"}) EXPECT_TRUE(index.touches(path));
    for(const auto* path:{"/work/defs2","/work/config2","/other"}) EXPECT_FALSE(index.touches(path));
    index.reset({}); EXPECT_FALSE(index.touches("/work"));
}
TEST(LuaInputIndex, LargeTreeInvalidationDoesNotRequirePublishedDirectoryEnumeration) {
    std::vector<std::string> inputs; inputs.reserve(100000);
    for(unsigned i=0;i<100000;++i) inputs.push_back("/workspace/packages/p"+std::to_string(i)+"/definitions");
    lang::LuaInputIndex index; index.reset(inputs);
    EXPECT_TRUE(index.touches("/workspace/packages/p99999/definitions/new.lua"));
    EXPECT_TRUE(index.touches("/workspace/packages"));
    EXPECT_FALSE(index.touches("/workspace/packages/p99999x/definitions/new.lua"));
}
