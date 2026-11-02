// SPDX-License-Identifier: Apache-2.0
// Real managed server against two SDK projects and unsaved UTF-16 documents.
#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/managed_service.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/text/document.hpp"

#include "../../src/lang/src/timeout.hpp"
#include "support/managed_profile_guard.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace std::chrono_literals;
using namespace frost;

sched::Task<Result<lang::Json>> request(sched::IScheduler& scheduler, lang::ManagedService& service, std::string method,
                                        lang::Json params) {
    lang::detail::Deadline deadline(scheduler, 3s);
    co_return co_await service.request(std::move(method), std::move(params), deadline.token());
}
lang::Json positionParams(const std::string& uri, int column) {
    return lang::Json::object()
        .set("textDocument", lang::Json::object().set("uri", uri))
        .set("position", lang::Json::object().set("line", 0).set("character", column));
}
std::string testEnvironment(const char* name) {
#ifdef _WIN32
    auto wide = io::toWide(name);
    if (!wide)
        return {};
    const DWORD size = GetEnvironmentVariableW(wide->c_str(), nullptr, 0);
    if (!size)
        return {};
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(wide->c_str(), value.data(), size);
    if (!length || length >= size)
        return {};
    value.resize(length);
    return io::fromWide(value).value_or("");
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}
TEST(ManagedProfileGuard, RequiresTheExactExistingMarkerWithoutCreatingIt) {
    test::ScratchDir scratch("managed-marker");
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(scratch.path()).has_value());
    EXPECT_FALSE(test::nugetMigrationMarkerPresent(std::filesystem::path(scratch.path())));
    ASSERT_TRUE(fs.createDirectories(scratch.join("NuGet/Migrations")).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("NuGet/Migrations/other"), "").has_value());
    EXPECT_FALSE(test::nugetMigrationMarkerPresent(std::filesystem::path(scratch.path())));
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("NuGet/Migrations/1"), "").has_value());
    EXPECT_TRUE(test::nugetMigrationMarkerPresent(std::filesystem::path(scratch.path())));
}
TEST(ManagedE2E, InheritedProfilePreservesCustomNugetFeed) {
    if (!test::sdkTestMayStart())
        GTEST_SKIP()
            << "Windows NuGet LocalAppData migration marker is absent; SDK startup would modify the user profile";
    const std::string dotnet = testEnvironment("FROST_TEST_CSHARP_DOTNET");
    if (dotnet.empty())
        GTEST_SKIP() << "Set FROST_TEST_CSHARP_DOTNET to the pinned SDK";
    test::ScratchDir scratch("managed-feed");
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(scratch.join(".nuget/NuGet")).has_value());
    ASSERT_TRUE(fs.createDirectories(scratch.join("appdata/NuGet")).has_value());
    const std::string config =
        R"(<configuration><packageSources><clear/><add key="ScratchFeed" value="https://example.invalid/v3/index.json"/></packageSources></configuration>)";
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join(".nuget/NuGet/NuGet.Config"), config).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("appdata/NuGet/NuGet.Config"), config).has_value());
    auto options = lang::managedServiceOptions(dotnet, scratch.join("server.dll"), scratch.path(), true);
    EXPECT_TRUE(
        std::ranges::none_of(options.environment, [](const auto& item) { return item.first == "DOTNET_CLI_HOME"; }));
    io::ProcessSpec spec;
    spec.executable = dotnet;
    spec.arguments = {"nuget", "list", "source", "--format", "short"};
    spec.workingDirectory = scratch.path();
    spec.environment = options.environment;
    spec.environment.emplace_back("DOTNET_CLI_HOME", std::nullopt);
    spec.environment.emplace_back("HOME", scratch.path());
    spec.environment.emplace_back("USERPROFILE", scratch.path());
    spec.environment.emplace_back("APPDATA", scratch.join("appdata"));
    spec.environment.emplace_back("LOCALAPPDATA", scratch.join("local"));
    spec.environment.emplace_back("XDG_DATA_HOME", scratch.join("xdg-data"));
    spec.environment.emplace_back("XDG_CONFIG_HOME", scratch.join("xdg-config"));
    spec.environment.emplace_back("NUGET_PACKAGES", scratch.join("packages"));
    auto result = io::runToCompletion(io::realProcessLauncher(), spec, {}, 30s);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->exitCode, 0) << result->stdoutBytes << result->stderrBytes;
    EXPECT_NE(result->stdoutBytes.find("example.invalid"), std::string::npos) << result->stdoutBytes;
}
TEST(ManagedE2E, UnsavedCrossProjectUtf16NavigationAndRename) {
    if (!frost::test::sdkTestMayStart())
        GTEST_SKIP()
            << "Windows NuGet LocalAppData migration marker is absent; SDK startup would modify the user profile";
    const std::string dotnet = testEnvironment("FROST_TEST_CSHARP_DOTNET");
    const std::string serverPath = testEnvironment("FROST_TEST_CSHARP_SERVER");
    if (dotnet.empty() || serverPath.empty())
        GTEST_SKIP() << "Set FROST_TEST_CSHARP_DOTNET and FROST_TEST_CSHARP_SERVER to absolute pinned tools";
    test::ScratchDir scratch("managed-e2e");
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(scratch.join("App")).has_value());
    ASSERT_TRUE(fs.createDirectories(scratch.join("Lib")).has_value());
    ASSERT_TRUE(
        fs.writeFileAtomic(scratch.join("Probe.slnx"),
                           R"(<Solution><Project Path="Lib/Lib.csproj"/><Project Path="App/App.csproj"/></Solution>)")
            .has_value());
    ASSERT_TRUE(
        fs.writeFileAtomic(
              scratch.join("Lib/Lib.csproj"),
              R"(<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>)")
            .has_value());
    ASSERT_TRUE(
        fs.writeFileAtomic(
              scratch.join("App/App.csproj"),
              R"(<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup><ItemGroup><ProjectReference Include="../Lib/Lib.csproj"/></ItemGroup></Project>)")
            .has_value());
    const std::string app = scratch.join("App/Example.cs");
    const std::string lib = scratch.join("Lib/Greeter.cs");
    ASSERT_TRUE(fs.writeFileAtomic(app, "using Library; public class Example {}\n").has_value());
    ASSERT_TRUE(fs.writeFileAtomic(lib, "namespace Library; public class Greeter {}\n").has_value());
    auto options = lang::managedServiceOptions(dotnet, serverPath, scratch.path(), true);
    options.environment.emplace_back("DOTNET_CLI_HOME", scratch.join("dotnet-home"));
    for (const auto* name : {"LOCALAPPDATA", "APPDATA", "HOME", "USERPROFILE", "XDG_DATA_HOME", "XDG_CONFIG_HOME",
                             "NUGET_PACKAGES", "NUGET_HTTP_CACHE_PATH", "NUGET_PLUGINS_CACHE_PATH"})
        options.environment.emplace_back(name, scratch.join(std::string("profile/") + name));
    io::ProcessSpec restore;
    restore.executable = dotnet;
    restore.arguments = {"restore", scratch.join("Probe.slnx"), "--ignore-failed-sources", "--nologo"};
    restore.workingDirectory = scratch.path();
    restore.environment = options.environment;
    auto restored = io::runToCompletion(io::realProcessLauncher(), restore, {}, 45s);
    ASSERT_TRUE(restored.has_value()) << restored.error().describe();
    ASSERT_EQ(restored->exitCode, 0) << restored->stdoutBytes << restored->stderrBytes;
    auto reactor = io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    sched::ThreadPool pool(sched::ThreadPoolOptions{.workerCount = 2});
    lang::ManagedService service(io::realProcessLauncher(), {**reactor, pool}, options);
    auto appText =
        text::Document::fromUtf8("using Library; public class Example { public string Run() { var s = "
                                 "\"\xf0\x9f\x98\x80\"; return new Greeter().SayHello(); MissingMethod(); } }\n");
    auto libText = text::Document::fromUtf8(
        "namespace Library; public class Greeter { public string SayHello() => \"Unsaved\"; }\n");
    ASSERT_TRUE(appText && libText);
    ASSERT_TRUE(service.openDocument(app, appText->text()).has_value());
    ASSERT_TRUE(service.openDocument(lib, libText->text()).has_value());
    ASSERT_TRUE(service.start().has_value());
    auto uri = lang::pathToUri(app);
    ASSERT_TRUE(uri.has_value());
    bool ready = false;
    auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto response =
            pool.spawn(sched::Lane::UserVisible, request(pool, service, "textDocument/hover", positionParams(*uri, 98)))
                .join();
        if (response && response->dump().find("SayHello") != std::string::npos) {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(250ms);
    }
    EXPECT_TRUE(ready) << "the server did not load the referenced project";
    if (ready) {
        auto completion = pool.spawn(sched::Lane::UserVisible,
                                     request(pool, service, "textDocument/completion", positionParams(*uri, 97)))
                              .join();
        EXPECT_TRUE(completion && completion->dump().find("SayHello") != std::string::npos);
        auto definition = pool.spawn(sched::Lane::UserVisible,
                                     request(pool, service, "textDocument/definition", positionParams(*uri, 98)))
                              .join();
        EXPECT_TRUE(definition && definition->dump().find("Greeter.cs") != std::string::npos);
        auto params = positionParams(*uri, 98);
        params.set("newName", "Welcome");
        auto rename =
            pool.spawn(sched::Lane::UserVisible, request(pool, service, "textDocument/rename", std::move(params)))
                .join();
        ASSERT_TRUE(rename.has_value());
        const auto result = rename->dump();
        EXPECT_NE(result.find("Example.cs"), std::string::npos);
        EXPECT_NE(result.find("Greeter.cs"), std::string::npos);
        EXPECT_NE(result.find("Welcome"), std::string::npos);
        EXPECT_NE(result.find("\"character\":95"), std::string::npos);
    }
    bool diagnosed = false;
    deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto diagnostics = service.diagnostics().forFile(app);
        if (diagnostics) {
            for (const auto& item : diagnostics->diagnostics)
                if (item.message.find("MissingMethod") != std::string::npos)
                    diagnosed = true;
        }
        if (diagnosed)
            break;
        std::this_thread::sleep_for(100ms);
    }
    EXPECT_TRUE(diagnosed) << "the unsaved missing method was not diagnosed";
    pool.spawn(sched::Lane::UserVisible, service.stop()).join();
    EXPECT_EQ(service.state(), lang::LuaState::Stopped);
    (*reactor)->shutdown();
    pool.shutdown();
}
} // namespace
