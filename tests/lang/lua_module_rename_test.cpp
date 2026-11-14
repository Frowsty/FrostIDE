// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/lua_module_rename.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/text/document.hpp"
#include <gtest/gtest.h>
#include <algorithm>
using namespace frost;
namespace {
std::string root() {
#ifdef _WIN32
    return "C:/workspace";
#else
    return "/workspace";
#endif
}
lang::LuaModuleRenameRequest fixture(io::MemFs& fs,std::string users) {
    const auto base=root(); EXPECT_TRUE(fs.createDirectories(base));
    const auto from=io::joinPath(base,"old.lua").value(),to=io::joinPath(base,"new.lua").value(),user=io::joinPath(base,"main.lua").value();
    EXPECT_TRUE(fs.writeFileAtomic(from,"return {}\n"));EXPECT_TRUE(fs.writeFileAtomic(user,users));
    auto module=text::Document::fromUtf8("return {}\n"),main=text::Document::fromUtf8(users);
    return {base,from,to,{"?.lua","?/init.lua"},{{from,module->text()},{user,main->text()}}};
}
}
TEST(LuaModuleRename, ChangesOnlyActualResolvedRequireLiteralAndCapturesLookupAbsences) {
    io::MemFs fs(systemClock());auto request=fixture(fs,"local m=require('old')\nprint('old') -- require('old')\nobject.require('old')\n");
    auto plan=lang::planLuaModuleRename(fs,request);ASSERT_TRUE(plan)<<plan.error().describe();ASSERT_EQ(plan->files.size(),1);
    ASSERT_EQ(plan->files[0].edits.size(),1);EXPECT_EQ(plan->files[0].edits[0].replacement,"'new'");
    EXPECT_TRUE(std::ranges::any_of(plan->probes,[](const auto& probe){return !probe.bytes;}));
    EXPECT_EQ(fs.readFile(request.fromPath).value_or(""),"return {}\n");EXPECT_FALSE(fs.exists(request.toPath).value_or(true));
}
TEST(LuaModuleRename, RefusesShadowingAliasingDynamicRequireAndSearchPathMutation) {
    for(const auto* source:{"local require=function()end\nrequire('old')","local r=require\nr('old')","require(name)","package.path='other/?.lua'\nrequire('old')",
        "debug.getregistry()['_LOADED']['old']={}\nrequire('old')"}) {
        io::MemFs fs(systemClock());auto request=fixture(fs,source);EXPECT_FALSE(lang::planLuaModuleRename(fs,request))<<source;
    }
}
TEST(LuaModuleRename, RefusesAmbiguousPhysicalCandidatesAndDestinationMembership) {
    io::MemFs fs(systemClock());auto request=fixture(fs,"require('old')");
    ASSERT_TRUE(fs.createDirectories(io::joinPath(root(),"old").value()));
    ASSERT_TRUE(fs.writeFileAtomic(io::joinPath(root(),"old/init.lua").value(),"return {}"));
    EXPECT_FALSE(lang::planLuaModuleRename(fs,request));
    ASSERT_TRUE(fs.writeFileAtomic(request.toPath,"foreign"));EXPECT_FALSE(lang::planLuaModuleRename(fs,request));
}
TEST(LuaModuleRename, RefusesMakingAnUnchangedUnresolvedRequireResolveToTheNewModule) {
    io::MemFs fs(systemClock());auto request=fixture(fs,"require('old')\nrequire('new')\n");
    EXPECT_FALSE(lang::planLuaModuleRename(fs,request));
}
