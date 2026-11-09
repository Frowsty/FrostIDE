// SPDX-License-Identifier: Apache-2.0
//
// Tests for the Lua host extension points (frost/lang/lua_host.hpp, LUA-13) on in-memory trees:
// the provider registry (order, replacement by name, removal), composing contributions into the
// LuaLS client configuration, the FrostIDE-owned meta folders (written, kept, pruned, unsafe names
// refused), FrostEngine's provider on that API (an engine checkout's projects as their own
// folders, the inputs a change must re-read), and that the Lua module's own sources hold no
// FrostEngine code (LUA-13's acceptance: FrostEngine is one provider among any).

#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/frost_engine_lua.hpp"
#include "frost/lang/lua_host.hpp"
#include "frost/lang/lua_service.hpp"

#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using frost::lang::Json;
using frost::lang::LuaHostContribution;

#ifdef _WIN32
const std::string kRoot = "C:\\work";
#else
const std::string kRoot = "/work";
#endif

std::string join(std::string_view relative) { return frost::io::joinPath(kRoot, relative).value(); }

void write(frost::io::MemFs& fs, std::string_view relative, std::string_view text) {
    const std::string path = join(relative);
    ASSERT_TRUE(fs.createDirectories(std::string(frost::io::parentPath(path))).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(path, text).has_value());
}

std::vector<std::string> strings(const Json& list) {
    std::vector<std::string> out;
    for (const Json& item : list.items())
        out.emplace_back(item.asString() ? *item.asString() : "<not a string>");
    return out;
}

LuaHostContribution withGlobals(std::vector<std::string> globals) {
    LuaHostContribution contribution;
    contribution.globals = std::move(globals);
    return contribution;
}

// A provider that contributes `contribution` to folders whose name is `folderName`.
class FixedProvider final : public frost::lang::ILuaHostProvider {
public:
    FixedProvider(std::string name, std::string folderName, LuaHostContribution contribution)
        : m_name(std::move(name)), m_folder(std::move(folderName)), m_contribution(std::move(contribution)) {}
    std::string_view name() const noexcept override { return m_name; }
    std::optional<LuaHostContribution> contribute(frost::io::IFileSystem&, std::string_view folder) override {
        if (frost::io::fileName(folder) != m_folder)
            return std::nullopt;
        return m_contribution;
    }

private:
    std::string m_name;
    std::string m_folder;
    LuaHostContribution m_contribution;
};

TEST(LuaHost, TheRegistryAsksProvidersInOrderReplacesByNameAndRemoves) {
    frost::io::MemFs fs(frost::systemClock());
    frost::lang::LuaHostRegistry registry;
    registry.add(std::make_shared<FixedProvider>("A", "work", withGlobals({"a"})));
    registry.add(std::make_shared<FixedProvider>("B", "work", withGlobals({"b"})));
    registry.add(std::make_shared<FixedProvider>("C", "elsewhere", withGlobals({"c"})));
    registry.add(nullptr);
    EXPECT_EQ(registry.names(), (std::vector<std::string>{"A", "B", "C"}));
    auto got = registry.contributions(fs, kRoot);
    ASSERT_EQ(got.size(), 2U) << "C does not claim this folder";
    EXPECT_EQ(got[0].provider, "A") << "the registry fills in the provider's name";
    EXPECT_EQ(got[1].globals, std::vector<std::string>{"b"});
    registry.add(std::make_shared<FixedProvider>("A", "work", withGlobals({"a2"})));
    EXPECT_EQ(registry.names(), (std::vector<std::string>{"B", "C", "A"})) << "a replacement goes last";
    EXPECT_TRUE(registry.remove("B"));
    EXPECT_FALSE(registry.remove("B"));
    got = registry.contributions(fs, kRoot);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].globals, std::vector<std::string>{"a2"});
}

TEST(LuaHost, CompositionMergesContributionsOverTheUsersDefaults) {
    EXPECT_EQ(frost::lang::composeLuaConfiguration("Lua 5.3", {}).dump(), frost::lang::luaDefaults("Lua 5.3").dump())
        << "no host: the client's own defaults";
    std::array<LuaHostContribution, 3> contributions;
    contributions[0].provider = "first";
    contributions[0].runtimePath = {"Scripts/?.lua"};
    contributions[0].builtins = {{"io", "disable"}, {"os", "disable"}};
    contributions[0].library = {"lib", "shared"};
    contributions[0].globals = {"Engine", "Log"};
    contributions[1].provider = "second";
    contributions[1].runtimeVersion = "Lua 5.1";
    contributions[1].runtimePath = {"Scripts/?.lua", "Mods/?.lua"};
    contributions[1].builtins = {{"os", "enable"}};
    contributions[1].library = {"shared", "more"};
    contributions[1].globals = {"Log", "Mod"};
    contributions[2].provider = "third";
    contributions[2].runtimeVersion = "LuaJIT";
    const std::array<std::string, 1> meta{join("state/meta")};
    const Json configuration = frost::lang::composeLuaConfiguration("Lua 5.4", contributions, meta);
    EXPECT_EQ(*frost::lang::luaConfigurationSection(configuration, "Lua.runtime.version").asString(), "Lua 5.1")
        << "the first host that names a version replaces the user's";
    EXPECT_EQ(strings(frost::lang::luaConfigurationSection(configuration, "Lua.runtime.path")),
              (std::vector<std::string>{"Scripts/?.lua", "Mods/?.lua"}));
    EXPECT_EQ(*frost::lang::luaConfigurationSection(configuration, "Lua.runtime.builtin.io").asString(), "disable");
    EXPECT_EQ(*frost::lang::luaConfigurationSection(configuration, "Lua.runtime.builtin.os").asString(), "enable")
        << "a later host's builtin entry wins";
    EXPECT_EQ(strings(frost::lang::luaConfigurationSection(configuration, "Lua.workspace.library")),
              (std::vector<std::string>{"lib", "shared", "more", join("state/meta")}));
    EXPECT_EQ(strings(frost::lang::luaConfigurationSection(configuration, "Lua.diagnostics.globals")),
              (std::vector<std::string>{"Engine", "Log", "Mod"}));
    EXPECT_EQ(frost::lang::luaConfigurationSection(configuration, "Lua.workspace.checkThirdParty").asBool(), false);
}

TEST(LuaHost, MetaFilesLiveInAnOwnedFolderThatHoldsExactlyThem) {
    frost::io::MemFs fs(frost::systemClock());
    const auto directory = frost::lang::luaHostMetaDirectory(join("state"), "Frost Engine", kRoot);
    ASSERT_TRUE(directory.has_value()) << directory.error().describe();
    EXPECT_EQ(*directory, frost::lang::luaHostMetaDirectory(join("state"), "Frost Engine", kRoot).value())
        << "stable across calls (and runs: FNV-1a of the folder's key)";
    EXPECT_NE(*directory, frost::lang::luaHostMetaDirectory(join("state"), "Frost Engine", join("other")).value());
    EXPECT_NE(directory->find("Frost_Engine-"), std::string::npos) << *directory;
    EXPECT_FALSE(frost::lang::luaHostMetaDirectory("state", "x", kRoot).has_value());
    EXPECT_FALSE(frost::lang::luaHostMetaDirectory(join("state"), "", kRoot).has_value());

    const std::array first{frost::lang::LuaMetaFile{"engine.lua", "---@meta\nEngine = {}\n"},
                           frost::lang::LuaMetaFile{"components/transform.lua", "---@meta\n"}};
    ASSERT_TRUE(frost::lang::writeLuaMetaFiles(fs, *directory, first).has_value());
    const std::string engine = frost::io::joinPath(*directory, "engine.lua").value();
    const std::string transform = frost::io::joinPath(*directory, "components/transform.lua").value();
    EXPECT_EQ(fs.readFile(engine).value(), "---@meta\nEngine = {}\n");
    EXPECT_TRUE(fs.exists(transform).value_or(false));
    ASSERT_TRUE(fs.writeFileAtomic(frost::io::joinPath(*directory, "notes.txt").value(), "kept").has_value());

    const std::array second{frost::lang::LuaMetaFile{"engine.lua", "---@meta\nEngine = {}\n"}};
    ASSERT_TRUE(frost::lang::writeLuaMetaFiles(fs, *directory, second).has_value());
    EXPECT_FALSE(fs.exists(transform).value_or(true)) << "a .lua file no longer listed is removed";
    EXPECT_TRUE(fs.exists(frost::io::joinPath(*directory, "notes.txt").value()).value_or(false))
        << "only .lua files are FrostIDE's to prune";

    for (const std::string_view bad : {"../escape.lua", "/abs.lua", "a\\b.lua", "x.txt", "", "a//b.lua", "C:x.lua"}) {
        const std::array files{frost::lang::LuaMetaFile{std::string(bad), "x"}};
        const auto refused = frost::lang::writeLuaMetaFiles(fs, *directory, files);
        ASSERT_FALSE(refused.has_value()) << bad;
        EXPECT_EQ(refused.error().code(), frost::ErrorCode::InvalidArgument) << bad;
    }
    EXPECT_FALSE(frost::lang::writeLuaMetaFiles(fs, "relative", second).has_value());
}

TEST(LuaHost, FrostEngineIsAProviderOnTheApiWithItsCheckoutsProjectsAsFolders) {
    frost::io::MemFs fs(frost::systemClock());
    write(fs, "Docs/ScriptingAPI.md",
          "<!-- Generated by ScriptApiDocs::GenerateMarkdown() -->\n## Globals\n\n| `Wait(s)` | GlobalFunction | x |\n"
          "| `_VERSION: string` | GlobalValue | The Lua version (\"Lua 5.4\"). |\n");
    write(fs, "Samples/Game/Game.frostproj", "{}");
    write(fs, "Samples/Game/.frost/lua-api/frost.lua", "---@meta\n");
    write(fs, "Tools/Probe/Probe.feproject", "{}");
    frost::lang::FrostEngineLuaProvider provider;
    EXPECT_EQ(provider.name(), "FrostEngine");
    const auto checkout = provider.contribute(fs, kRoot);
    ASSERT_TRUE(checkout.has_value());
    EXPECT_EQ(checkout->runtimeVersion, "Lua 5.4");
    EXPECT_EQ(checkout->globals, (std::vector<std::string>{"Wait", "_VERSION"}));
    EXPECT_EQ(checkout->folders, (std::vector<std::string>{join("Samples/Game"), join("Tools/Probe")}));
    EXPECT_NE(std::ranges::find(checkout->inputs, join("Docs/ScriptingAPI.md")), checkout->inputs.end());
    EXPECT_NE(std::ranges::find(checkout->inputs, join("Tools/Probe/.frost/lua-api")), checkout->inputs.end())
        << "a definitions folder the engine writes later is an input";
    const auto project = provider.contribute(fs, join("Samples/Game"));
    ASSERT_TRUE(project.has_value());
    EXPECT_TRUE(project->folders.empty());
    EXPECT_EQ(project->library, std::vector<std::string>{".frost/lua-api"});
    EXPECT_EQ(project->globals, checkout->globals) << "a project inside the checkout finds the engine's reference";
    EXPECT_FALSE(provider.contribute(fs, join("Docs")).has_value()) << "not a FrostEngine folder";
}

// Strips // and /* */ comments and string literals' quotes are kept; good enough for our sources.
std::string codeOnly(const std::string& source) {
    std::string out;
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source.compare(i, 2, "//") == 0) {
            i = source.find('\n', i);
            if (i == std::string::npos)
                break;
            out += '\n';
        } else if (source.compare(i, 2, "/*") == 0) {
            i = source.find("*/", i);
            if (i == std::string::npos)
                break;
            ++i;
        } else {
            out += source[i];
        }
    }
    return out;
}

// LUA-13's acceptance: the Lua module (the LuaLS client, its configuration, the host API and the
// .luarc.json model) holds no FrostEngine-specific code; FrostEngine reaches LuaLS only through
// its provider (frost_engine_lua.*), registered by the composition root.
TEST(LuaHost, TheLuaModuleHoldsNoFrostEngineCode) {
    const std::string root = FROST_LANG_SOURCE_DIR;
    for (const std::string_view file :
         {"include/frost/lang/lua_service.hpp", "src/lua_service.cpp", "src/lua_config.cpp",
          "include/frost/lang/lua_host.hpp", "src/lua_host.cpp", "include/frost/lang/lua_rc.hpp", "src/lua_rc.cpp"}) {
        std::ifstream in(root + "/" + std::string(file), std::ios::binary);
        ASSERT_TRUE(in.good()) << root << "/" << file;
        std::stringstream text;
        text << in.rdbuf();
        const std::string code = codeOnly(text.str());
        for (const std::string_view marker :
             {"FrostEngine", "frostEngine", "frost_engine", "frostproj", "feproject", ".frost/", "Assets/Scripts"})
            EXPECT_EQ(code.find(marker), std::string::npos) << file << " mentions " << marker << " outside comments";
    }
}

} // namespace
