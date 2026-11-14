// SPDX-License-Identifier: Apache-2.0
// LuaLS configuration, discovery of the pinned release, its state arguments, FrostEngine workspace
// detection, the trust boundary and the supervised transport with a scripted server.
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/frost_engine_lua.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/lang/managed_service.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/text/document.hpp"

#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
#ifdef _WIN32
const std::string& root() {
    static const std::string value = "C:\\project";
    return value;
}
const std::string& tool() {
    static const std::string value = "C:\\tools\\lua-language-server.exe";
    return value;
}
#else
const std::string& root() {
    static const std::string value = "/project";
    return value;
}
const std::string& tool() {
    static const std::string value = "/tools/lua-language-server";
    return value;
}
#endif
std::string frame(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
TEST(LuaService, DiscoveryRejectsRelativeAndMissingExplicitPaths) {
    frost::io::MemFs fs(frost::systemClock());
    ASSERT_TRUE(fs.createDirectories(frost::io::parentPath(tool())).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(tool(), "exe").has_value());
    frost::lang::LuaSearch search{
        .configuredPath = "relative", .directories = {std::string(frost::io::parentPath(tool()))}, .pathVariable = {}};
    EXPECT_EQ(frost::lang::findLuaLanguageServer(fs, search).error().code(), frost::ErrorCode::InvalidArgument);
    search.configuredPath = root();
    EXPECT_EQ(frost::lang::findLuaLanguageServer(fs, search).error().code(), frost::ErrorCode::NotFound);
    search.configuredPath.clear();
    EXPECT_EQ(frost::lang::findLuaLanguageServer(fs, search).value(), tool());
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
    const std::string state = frost::io::joinPath(root(), "state").value();
    const auto arguments = frost::lang::luaLanguageServerArguments(state);
    ASSERT_TRUE(arguments.has_value());
    EXPECT_EQ(*arguments, (std::vector<std::string>{"--logpath=" + frost::io::joinPath(state, "log").value(),
                                                    "--metapath=" + frost::io::joinPath(state, "meta").value()}));
    EXPECT_EQ(frost::lang::luaLanguageServerArguments("relative").error().code(), frost::ErrorCode::InvalidArgument);
}
TEST(LuaService, ExperimentalEmmyHasExplicitStdioAndPrivateResourcesWithoutLuaLsFlags) {
    const auto state = frost::io::joinPath(root(), "state").value();
    auto args = frost::lang::emmyLuaArguments(state);
    ASSERT_TRUE(args);
    EXPECT_EQ(*args, (std::vector<std::string>{"--communication", "stdio", "--resources-path",
                                               frost::io::joinPath(state, "emmylua").value(), "--log-path", "none"}));
    EXPECT_FALSE(frost::lang::emmyLuaArguments("relative"));
}
TEST(LuaService, FrostEngineWorkspaceNeedsAFrostprojInTheRoot) {
    frost::io::MemFs fs(frost::systemClock());
    ASSERT_TRUE(fs.createDirectories(frost::io::joinPath(root(), "sub").value()).has_value());
    EXPECT_FALSE(frost::lang::isFrostEngineLuaWorkspace(fs, root()));
    ASSERT_TRUE(fs.writeFileAtomic(frost::io::joinPath(root(), "sub/Game.frostproj").value(), "{}").has_value());
    EXPECT_FALSE(frost::lang::isFrostEngineLuaWorkspace(fs, root())) << "only the root itself counts";
    ASSERT_TRUE(fs.createDirectories(frost::io::joinPath(root(), "dir.frostproj").value()).has_value());
    EXPECT_FALSE(frost::lang::isFrostEngineLuaWorkspace(fs, root())) << "a directory is not a project file";
    ASSERT_TRUE(fs.writeFileAtomic(frost::io::joinPath(root(), "Game.frostproj").value(), "{}").has_value());
    EXPECT_TRUE(frost::lang::isFrostEngineLuaWorkspace(fs, root()));
    EXPECT_FALSE(frost::lang::isFrostEngineLuaWorkspace(fs, frost::io::joinPath(root(), "missing").value()));
}
// One rule for the LuaLS defaults and the FrostEngine tool window: .frostproj or .feproject, ASCII
// case ignored on every platform, a non-empty stem, a regular file directly in the root.
TEST(LuaService, FrostEngineProjectDetectionIsOneRuleForBothSuffixesAndAnyCase) {
    using frost::lang::isFrostEngineProjectFileName;
    for (const char* name : {"Game.frostproj", "Game.FROSTPROJ", "Old.feproject", "Old.FeProject", "a.b.frostproj"})
        EXPECT_TRUE(isFrostEngineProjectFileName(name)) << name;
    for (const char* name : {".frostproj", ".feproject", "Game.frostproj.bak", "Game.frost", "frostproj", ""})
        EXPECT_FALSE(isFrostEngineProjectFileName(name)) << name;

    frost::io::MemFs fs(frost::systemClock());
    const std::string legacy = frost::io::joinPath(root(), "legacy").value();
    const std::string upper = frost::io::joinPath(root(), "upper").value();
    const std::string hidden = frost::io::joinPath(root(), "hidden").value();
    ASSERT_TRUE(fs.createDirectories(legacy).has_value());
    ASSERT_TRUE(fs.createDirectories(upper).has_value());
    ASSERT_TRUE(fs.createDirectories(hidden).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(frost::io::joinPath(legacy, "Old.feproject").value(), "{}").has_value());
    ASSERT_TRUE(fs.writeFileAtomic(frost::io::joinPath(upper, "GAME.FROSTPROJ").value(), "{}").has_value());
    ASSERT_TRUE(fs.writeFileAtomic(frost::io::joinPath(hidden, ".frostproj").value(), "{}").has_value());
    for (const auto& [dir, expected] : {std::pair{legacy, true}, std::pair{upper, true}, std::pair{hidden, false}}) {
        EXPECT_EQ(frost::lang::isFrostEngineProjectRoot(fs, dir), expected) << dir;
        EXPECT_EQ(frost::lang::isFrostEngineLuaWorkspace(fs, dir), expected) << dir;
    }
    EXPECT_FALSE(frost::lang::isFrostEngineProjectRoot(fs, ""));
}
TEST(ManagedService, RequiresTrustAndConfiguresCSharpProcess) {
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::SimExecutor sim;
    frost::io::ScriptedProcessLauncher launcher;
    auto options = frost::lang::managedServiceOptions(tool(), root() + "/CSharpLanguageServer.dll", root(), false);
    EXPECT_EQ(options.languageId, "csharp");
    ASSERT_EQ(options.arguments.size(), 3U);
    EXPECT_EQ(options.arguments[0], root() + "/CSharpLanguageServer.dll");
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
    ASSERT_TRUE(trusted.openDocument(frost::io::joinPath(root(), "main.cs").value(), document->text()).has_value());
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
    auto config = frost::lang::frostEngineLuaDefaults("Lua 5.4", [] {
        frost::lang::FrostEngineLuaProfile unreferenced;
        unreferenced.frostEngine = true; // A FrostEngine workspace whose reference was not found.
        return unreferenced;
    }());
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
    EXPECT_EQ(frost::lang::luaConfigurationSection(frost::lang::luaDefaults("Lua 5.4"), "Lua.hint.enable").asBool(),
              true);
}
TEST(LuaService, ParseCheckReportAndInspectGateValidateInputs) {
    frost::io::MemFs fs(frost::systemClock());
    const std::string fileA = frost::io::joinPath(root(), "scripts/a.lua").value();
    const std::string fileB = frost::io::joinPath(root(), "scripts/b.lua").value();
    ASSERT_TRUE(fs.createDirectories(frost::io::parentPath(fileA)).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(fileA, "local x = 1\nprint(missingGlobal)\n").has_value());
    ASSERT_TRUE(fs.writeFileAtomic(fileB, "local unused = 2\n").has_value());

    const auto emptyReport = frost::lang::parseLuaCheckReport("[]", &fs);
    ASSERT_TRUE(emptyReport.has_value()) << emptyReport.error().describe();
    EXPECT_TRUE(emptyReport->empty());
    EXPECT_TRUE(frost::lang::parseLuaCheckReport("  \n ", &fs).value().empty());
    EXPECT_FALSE(frost::lang::parseLuaCheckReport("{not json", &fs).has_value());
    EXPECT_FALSE(frost::lang::parseLuaCheckReport("[1]", &fs).has_value());

    const std::string uriA = frost::lang::pathToUri(fileA).value();
    const std::string uriB = frost::lang::pathToUri(fileB).value();
    const frost::lang::Json reportJson =
        frost::lang::Json::object()
            .set(uriB,
                 frost::lang::Json::array(
                     {frost::lang::Json::object()
                          .set("code", "unused-local")
                          .set("message", "Unused local `unused`.")
                          .set("severity", 4)
                          .set("range",
                               frost::lang::Json::object()
                                   .set("start", frost::lang::Json::object().set("line", 0).set("character", 6))
                                   .set("end", frost::lang::Json::object().set("line", 0).set("character", 12)))}))
            .set(uriA,
                 frost::lang::Json::array(
                     {frost::lang::Json::object()
                          .set("code", "undefined-global")
                          .set("message", "Undefined global `missingGlobal`.")
                          .set("severity", 2)
                          .set("range",
                               frost::lang::Json::object()
                                   .set("start", frost::lang::Json::object().set("line", 1).set("character", 6))
                                   .set("end", frost::lang::Json::object().set("line", 1).set("character", 19)))}));
    const auto parsed = frost::lang::parseLuaCheckReport(reportJson.dump(), &fs);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().describe();
    ASSERT_EQ(parsed->size(), 2U);
    EXPECT_EQ((*parsed)[0].file, fileA);
    ASSERT_EQ((*parsed)[0].diagnostics.size(), 1U);
    EXPECT_EQ((*parsed)[0].diagnostics[0].code, "undefined-global");
    EXPECT_EQ((*parsed)[0].diagnostics[0].severity, frost::lang::DiagnosticSeverity::Warning);
    EXPECT_EQ((*parsed)[0].diagnostics[0].line, 2);
    EXPECT_EQ((*parsed)[0].diagnostics[0].column, 7);
    ASSERT_TRUE((*parsed)[0].diagnostics[0].range.has_value());
    EXPECT_EQ((*parsed)[0].diagnostics[0].range->begin, frost::ByteOffset{18});
    EXPECT_EQ((*parsed)[0].diagnostics[0].range->end, frost::ByteOffset{31});
    EXPECT_EQ((*parsed)[1].file, fileB);
    ASSERT_EQ((*parsed)[1].diagnostics.size(), 1U);
    EXPECT_EQ((*parsed)[1].diagnostics[0].code, "unused-local");
    EXPECT_EQ((*parsed)[1].diagnostics[0].severity, frost::lang::DiagnosticSeverity::Hint);

    frost::io::ScriptedProcessLauncher launcher;
    frost::lang::LuaInspectOptions inspectOpts;
    inspectOpts.directory = frost::io::joinPath(root(), "scripts").value();
    inspectOpts.stateDirectory = frost::io::joinPath(root(), "state").value();
    inspectOpts.search = frost::lang::LuaSearch{};
    inspectOpts.trusted = false;
    EXPECT_EQ(frost::lang::inspectLuaDirectory(launcher, fs, inspectOpts).error().code(), frost::ErrorCode::Untrusted);
    EXPECT_TRUE(launcher.launches().empty());

    inspectOpts.trusted = true;
    EXPECT_EQ(frost::lang::inspectLuaDirectory(launcher, fs, inspectOpts).error().code(), frost::ErrorCode::NotFound);
    EXPECT_TRUE(launcher.launches().empty());

    inspectOpts.directory = "relative/scripts";
    EXPECT_EQ(frost::lang::inspectLuaDirectory(launcher, fs, inspectOpts).error().code(),
              frost::ErrorCode::InvalidArgument);
}
TEST(LuaService, DelayedRequestSurvivesServiceDestruction) {
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::SimExecutor sim;
    frost::io::ScriptedProcessLauncher launcher;
    frost::lang::LuaServiceOptions options;
    options.serverPath = tool();
    options.workspaceRoot = root();
    auto service =
        std::make_unique<frost::lang::LuaService>(launcher, frost::io::AsyncContext{**reactor, sim}, options);
    auto request = service->request("textDocument/hover", frost::lang::Json::object());
    service.reset();
    auto result = sim.spawn(frost::sched::Lane::UserVisible, std::move(request)).join();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Busy);
}
TEST(LuaService, ProjectionOpenRequiresLiveProviderAndDoesNotRetainRejectedDocument) {
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor);
    frost::sched::SimExecutor sim;
    frost::io::ScriptedProcessLauncher launcher;
    frost::lang::LuaServiceOptions options;
    options.serverPath = tool();
    options.workspaceRoot = root();
    options.trusted = true;
    frost::lang::LuaService service(launcher, {**reactor, sim}, options);
    auto document = frost::text::Document::fromUtf8("Cast()");
    ASSERT_TRUE(document);
    const auto path = frost::io::joinPath(root(), ".binding-projection.lua").value();
    const auto opened = service.openProjectionDocument(path, document->text());
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code(), frost::ErrorCode::Busy);
    EXPECT_FALSE(service.documentVersion(path));
    EXPECT_TRUE(launcher.launches().empty());
    // Editor opens deliberately retain their established offline/next-start behavior.
    EXPECT_TRUE(service.openDocument(path, document->text()));
    EXPECT_TRUE(service.documentVersion(path));
}
TEST(LuaService, TrustStopsLaunchAndScriptedServerReceivesLuaDocuments) {
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::SimExecutor sim;
    frost::io::ScriptedProcessLauncher launcher;
    frost::lang::LuaServiceOptions options;
    options.serverPath = tool();
    options.workspaceRoot = root();
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
    ASSERT_TRUE(service.openDocument(frost::io::joinPath(root(), "main.lua").value(), document->text()).has_value());
    ASSERT_TRUE(service.start().has_value());
    sim.runFor(5ms);
    ASSERT_EQ(service.state(), frost::lang::LuaState::Running);
    const auto projectionPath = frost::io::joinPath(root(), ".strict-projection.lua").value();
    const auto projection = service.openProjectionDocument(projectionPath, document->text());
    ASSERT_TRUE(projection);
    EXPECT_EQ(service.documentVersion(projectionPath), *projection);
    const auto duplicateProjection = service.openProjectionDocument(projectionPath, document->text());
    ASSERT_FALSE(duplicateProjection);
    EXPECT_EQ(duplicateProjection.error().code(), frost::ErrorCode::AlreadyExists);
    EXPECT_TRUE(service.closeDocument(projectionPath));
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
