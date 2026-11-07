// SPDX-License-Identifier: Apache-2.0
// LuaLS configuration, discovery of the pinned release, its state arguments, FrostEngine workspace
// detection, the trust boundary and the supervised transport with a scripted server.
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/lang/managed_service.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/text/document.hpp"

#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
#ifdef _WIN32
const std::string root = "C:\\project";
const std::string tool = "C:\\tools\\lua-language-server.exe";
#else
const std::string root = "/project";
const std::string tool = "/tools/lua-language-server";
#endif
std::string frame(std::string body) { return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body; }
TEST(LuaService, DiscoveryRejectsRelativeAndMissingExplicitPaths) {
    frost::io::MemFs fs(frost::systemClock());
    ASSERT_TRUE(fs.createDirectories(frost::io::parentPath(tool)).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(tool, "exe").has_value());
    frost::lang::LuaSearch search{
        .configuredPath = "relative", .directories = {std::string(frost::io::parentPath(tool))}, .pathVariable = {}};
    EXPECT_EQ(frost::lang::findLuaLanguageServer(fs, search).error().code(), frost::ErrorCode::InvalidArgument);
    search.configuredPath = root;
    EXPECT_EQ(frost::lang::findLuaLanguageServer(fs, search).error().code(), frost::ErrorCode::NotFound);
    search.configuredPath.clear();
    EXPECT_EQ(frost::lang::findLuaLanguageServer(fs, search).value(), tool);
}
// LUA-1: the default search names the pinned release's folder (staged and SDK) before PATH.
TEST(LuaService, DefaultSearchPrefersThePinnedInstall) {
    EXPECT_FALSE(frost::lang::pinnedLuaLanguageServerVersion().empty());
    const auto search = frost::lang::defaultLuaSearch();
    const std::string pinned = "luals-" + std::string(frost::lang::pinnedLuaLanguageServerVersion());
    const auto pinnedDirs = std::ranges::count_if(
        search.directories, [&](const std::string& dir) { return dir.find(pinned) != std::string::npos; });
    // The staged folder, and the SDK root when FROST_SDK names one (FND-10: no machine-specific
    // default root such as the bootstrap roots; tools/env.ps1 and ~/sdk/env.sh set FROST_SDK).
    const std::string sdk = frost::test::environment("FROST_SDK");
    EXPECT_EQ(pinnedDirs, sdk.empty() ? 1 : 2) << "the staged folder and the SDK root";
    EXPECT_NE(search.directories.front().find(pinned), std::string::npos) << search.directories.front();
    // <exe>/luals-<pinned>/bin, <exe>/lua-language-server/bin, <exe>, then $FROST_SDK/luals-<pinned>/bin.
    ASSERT_EQ(search.directories.size(), sdk.empty() ? 3U : 4U);
    if (!sdk.empty())
        EXPECT_EQ(search.directories.back(), frost::io::joinPath(sdk, pinned + "/bin").value());
    for (const auto& dir : search.directories)
        EXPECT_TRUE(frost::io::isAbsolutePath(dir)) << dir;
}
TEST(LuaService, StateArgumentsKeepLogAndMetaUnderTheGivenDirectory) {
    const std::string state = frost::io::joinPath(root, "state").value();
    const auto arguments = frost::lang::luaLanguageServerArguments(state);
    ASSERT_TRUE(arguments.has_value());
    EXPECT_EQ(*arguments, (std::vector<std::string>{"--logpath=" + frost::io::joinPath(state, "log").value(),
                                                    "--metapath=" + frost::io::joinPath(state, "meta").value()}));
    EXPECT_EQ(frost::lang::luaLanguageServerArguments("relative").error().code(), frost::ErrorCode::InvalidArgument);
}
TEST(LuaService, FrostEngineWorkspaceNeedsAFrostprojInTheRoot) {
    frost::io::MemFs fs(frost::systemClock());
    ASSERT_TRUE(fs.createDirectories(frost::io::joinPath(root, "sub").value()).has_value());
    EXPECT_FALSE(frost::lang::isFrostEngineLuaWorkspace(fs, root));
    ASSERT_TRUE(fs.writeFileAtomic(frost::io::joinPath(root, "sub/Game.frostproj").value(), "{}").has_value());
    EXPECT_FALSE(frost::lang::isFrostEngineLuaWorkspace(fs, root)) << "only the root itself counts";
    ASSERT_TRUE(fs.createDirectories(frost::io::joinPath(root, "dir.frostproj").value()).has_value());
    EXPECT_FALSE(frost::lang::isFrostEngineLuaWorkspace(fs, root)) << "a directory is not a project file";
    ASSERT_TRUE(fs.writeFileAtomic(frost::io::joinPath(root, "Game.frostproj").value(), "{}").has_value());
    EXPECT_TRUE(frost::lang::isFrostEngineLuaWorkspace(fs, root));
    EXPECT_FALSE(frost::lang::isFrostEngineLuaWorkspace(fs, frost::io::joinPath(root, "missing").value()));
}
TEST(ManagedService, RequiresTrustAndConfiguresCSharpProcess) {
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::SimExecutor sim;
    frost::io::ScriptedProcessLauncher launcher;
    auto options = frost::lang::managedServiceOptions(tool, root + "/CSharpLanguageServer.dll", root, false);
    EXPECT_EQ(options.languageId, "csharp");
    ASSERT_EQ(options.arguments.size(), 3U);
    EXPECT_EQ(options.arguments[0], root + "/CSharpLanguageServer.dll");
    frost::lang::ManagedService server(launcher, {**reactor, sim}, options);
    EXPECT_EQ(server.start().error().code(), frost::ErrorCode::Untrusted);
    EXPECT_TRUE(launcher.launches().empty());
    options.trusted = true;
    launcher.expect(frost::io::ScriptedProcess{
        .stdoutBytes = frame(R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{"textDocumentSync":2}}})"),
        .neverExits = true,
        .chunkBytes = 1024,
        .chunkInterval = 1ms});
    frost::lang::ManagedService trusted(launcher, {**reactor, sim}, options);
    auto document = frost::text::Document::fromUtf8("class Sample {}\n");
    ASSERT_TRUE(document.has_value());
    ASSERT_TRUE(trusted.openDocument(frost::io::joinPath(root, "main.cs").value(), document->text()).has_value());
    ASSERT_TRUE(trusted.start().has_value());
    sim.runFor(5ms);
    ASSERT_EQ(trusted.state(), frost::lang::LuaState::Running);
    auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    EXPECT_EQ(launches.front().spec.arguments, options.arguments);
    EXPECT_EQ(launches.front().spec.environment, options.environment);
    EXPECT_NE(launches.front().stdinBytes.find("\"languageId\":\"csharp\""), std::string::npos);
    launcher.expect(frost::io::ScriptedProcess{
        .stdoutBytes = frame(R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{"textDocumentSync":2}}})"),
        .neverExits = true,
        .chunkBytes = 1024,
        .chunkInterval = 1ms});
    auto pending = trusted.request("textDocument/hover", frost::lang::Json::object());
    ASSERT_TRUE(trusted.killServerForTesting().has_value());
    sim.runFor(1s);
    ASSERT_EQ(trusted.state(), frost::lang::LuaState::Running);
    EXPECT_EQ(trusted.restarts(), 1U);
    EXPECT_FALSE(sim.spawn(frost::sched::Lane::UserVisible, std::move(pending)).join().has_value());
    launches = launcher.launches();
    ASSERT_EQ(launches.size(), 2U);
    EXPECT_EQ(launches[1].spec.arguments, options.arguments);
    EXPECT_EQ(launches[1].spec.environment, options.environment);
    EXPECT_NE(launches[1].stdinBytes.find("\"languageId\":\"csharp\""), std::string::npos);
    sim.spawn(frost::sched::Lane::UserVisible, trusted.stop()).join();
    EXPECT_EQ(trusted.state(), frost::lang::LuaState::Stopped);
}

TEST(LuaService, ConfigurationMatchesEngineAndSupportsSections) {
    auto config = frost::lang::luaDefaults("Lua 5.4", true);
    EXPECT_EQ(frost::lang::luaConfigurationSection(config, "Lua.runtime.version").asString()->compare("Lua 5.4"), 0);
    EXPECT_EQ(frost::lang::luaConfigurationSection(config, "Lua.workspace.library")
                  .items()[0]
                  .asString()
                  ->compare(".frost/lua-api"),
              0);
    EXPECT_EQ(frost::lang::luaConfigurationSection(config, "Lua.runtime.builtin.io").asString()->compare("disable"), 0);
    EXPECT_TRUE(frost::lang::luaConfigurationSection(config, "other").isNull());
    EXPECT_EQ(
        *frost::lang::luaConfigurationSection(frost::lang::luaDefaults("Lua 5.1"), "Lua.runtime.version").asString(),
        "Lua 5.1");
}
TEST(LuaService, DelayedRequestSurvivesServiceDestruction) {
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::SimExecutor sim;
    frost::io::ScriptedProcessLauncher launcher;
    frost::lang::LuaServiceOptions options;
    options.serverPath = tool;
    options.workspaceRoot = root;
    auto service =
        std::make_unique<frost::lang::LuaService>(launcher, frost::io::AsyncContext{**reactor, sim}, options);
    auto request = service->request("textDocument/hover", frost::lang::Json::object());
    service.reset();
    auto result = sim.spawn(frost::sched::Lane::UserVisible, std::move(request)).join();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Busy);
}
TEST(LuaService, TrustStopsLaunchAndScriptedServerReceivesLuaDocuments) {
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::SimExecutor sim;
    frost::io::ScriptedProcessLauncher launcher;
    frost::lang::LuaServiceOptions options;
    options.serverPath = tool;
    options.workspaceRoot = root;
    {
        frost::lang::LuaService untrusted(launcher, {**reactor, sim}, options);
        EXPECT_EQ(untrusted.start().error().code(), frost::ErrorCode::Untrusted);
        EXPECT_TRUE(launcher.launches().empty());
    }
    options.trusted = true;
    int lifecycle = 0;
    options.onLifecycle = [&] { ++lifecycle; };
    launcher.expect(frost::io::ScriptedProcess{
        .stdoutBytes = frame(R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{"textDocumentSync":2}}})"),
        .neverExits = true,
        .chunkBytes = 1024,
        .chunkInterval = 1ms});
    frost::lang::LuaService service(launcher, {**reactor, sim}, options);
    auto document = frost::text::Document::fromUtf8("local x = 1\n");
    ASSERT_TRUE(document.has_value());
    ASSERT_TRUE(service.openDocument(frost::io::joinPath(root, "main.lua").value(), document->text()).has_value());
    ASSERT_TRUE(service.start().has_value());
    sim.runFor(5ms);
    ASSERT_EQ(service.state(), frost::lang::LuaState::Running);
    EXPECT_EQ(lifecycle, 1);
    EXPECT_EQ(service.encoding(), frost::lang::PositionEncoding::Utf16);
    auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    EXPECT_TRUE(launches[0].spec.arguments.empty());
    EXPECT_TRUE(launches[0].spec.killOnClose);
    EXPECT_NE(launches[0].stdinBytes.find("\"languageId\":\"lua\""), std::string::npos);
    launcher.expect(frost::io::ScriptedProcess{
        .stdoutBytes = frame(R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{"textDocumentSync":2}}})"),
        .neverExits = true,
        .chunkBytes = 1024,
        .chunkInterval = 1ms});
    auto oldRequest = service.request("textDocument/hover", frost::lang::Json::object());
    ASSERT_TRUE(service.killServerForTesting().has_value());
    sim.runFor(1s);
    EXPECT_EQ(service.state(), frost::lang::LuaState::Running);
    EXPECT_EQ(service.restarts(), 1U);
    EXPECT_EQ(lifecycle, 3);
    EXPECT_EQ(service.encoding(), frost::lang::PositionEncoding::Utf16);
    auto oldResult = sim.spawn(frost::sched::Lane::UserVisible, std::move(oldRequest)).join();
    EXPECT_FALSE(oldResult.has_value());
    launches = launcher.launches();
    ASSERT_EQ(launches.size(), 2U);
    EXPECT_NE(launches[1].stdinBytes.find("\"languageId\":\"lua\""), std::string::npos);
    sim.spawn(frost::sched::Lane::UserVisible, service.stop()).join();
    EXPECT_EQ(service.state(), frost::lang::LuaState::Stopped);
}
} // namespace
