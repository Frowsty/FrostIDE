// SPDX-License-Identifier: Apache-2.0
// Real managed server against two SDK projects and unsaved UTF-16 documents.
#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/managed_service.hpp"
#include "frost/lang/uri.hpp"
#include "frost/lang/workspace_edit.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/text/document.hpp"

#include "../../src/lang/src/timeout.hpp"
#include "support/managed_profile_guard.hpp"
#include "support/managed_service_shutdown.hpp"
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
                                        lang::Json params, IClock::Duration timeout = 3s) {
    lang::detail::Deadline deadline(scheduler, timeout);
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
    ASSERT_FALSE(dotnet.empty()) << "Required managed fixture lacks FROST_TEST_CSHARP_DOTNET";
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
    ASSERT_FALSE(dotnet.empty()) << "Required managed fixture lacks FROST_TEST_CSHARP_DOTNET";
    ASSERT_FALSE(serverPath.empty()) << "Required managed fixture lacks FROST_TEST_CSHARP_SERVER";
    test::ScratchDir scratch("managed-e2e");
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(scratch.join("App")).has_value());
    ASSERT_TRUE(fs.createDirectories(scratch.join("Lib")).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("NuGet.Config"),
                                   "<configuration><packageSources><clear/></packageSources></configuration>"));
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
    const auto shadowPath = scratch.join("Lib/Shadow.cs");
    const std::string shadowText =
        "namespace Shadow; public class @var { public static implicit operator @var(int x) => new @var(); public "
        "static implicit operator int(@var x) => 99; } public class Probe { public int Introduce() { return 1; } "
        "public int Inline() { var answer = 1; return answer; } }\n";
    ASSERT_TRUE(fs.writeFileAtomic(shadowPath, shadowText));
    const std::string semanticSource =
        "public class Operations { private static int Add(int left,long right)=>left+(int)right; "
        "public static int Run()=>Add(1,2); private static void Unused(){} private static int deadField; "
        "private int instanceField; private static void Used(){} public static void Keep()=>Used(); "
        "public static int Extract(int input){int doubled=input*2; return doubled+1;} "
        "private static int Clash(int a,long b)=>0; private static int Clash(long a,long b)=>0; "
        "public static int Existing()=>Clash(1L,1); } public class Extra {}\n";
    const auto semanticFile = scratch.join("Lib/Refactor.cs");
    ASSERT_TRUE(fs.writeFileAtomic(semanticFile, semanticSource));
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
    test::ManagedServiceShutdown shutdown{service, **reactor, pool};
    auto appText =
        text::Document::fromUtf8("using Library; public class Example { public string Run() { var s = "
                                 "\"\xf0\x9f\x98\x80\"; return new Greeter().SayHello(); MissingMethod(); } }\n");
    auto libText = text::Document::fromUtf8(
        "namespace Library; public class Greeter { public string SayHello() => \"Unsaved\"; public int Compute() { "
        "return 21 + 21; } public int Inline() { var answer = 42; return answer; } public int Multiple() { var count = "
        "42; return count + count; } }\n");
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
        // Real external metadata symbol from the evaluated reference pack, through the same host
        // endpoint normal F12 invokes. Reference assemblies may legitimately have no method IL.
        auto library =
            pool.spawn(sched::Lane::UserVisible,
                       request(pool, service, "frost/managed/librarySource",
                               positionParams(*uri, 47).set("mode", "decompiled").set("allowSourceLink", false), 15s))
                .join();
        ASSERT_TRUE(library) << library.error().describe();
        ASSERT_TRUE(library->isObject());
        EXPECT_EQ(library->stringAt("kind"), "decompiled");
        EXPECT_FALSE(library->stringAt("source").empty());
        EXPECT_EQ(library->stringAt("assemblySha256").size(), 64U);
        EXPECT_FALSE(library->stringAt("moduleId").empty());
        EXPECT_EQ(library->stringAt("extension"), ".cs");
        auto evaluated = pool.spawn(sched::Lane::UserVisible,
                                    request(pool, service, "frost/projectModel", lang::Json::object(), 15s))
                             .join();
        ASSERT_TRUE(evaluated) << evaluated.error().describe();
        ASSERT_TRUE(evaluated->isArray());
        EXPECT_NE(evaluated->dump().find("App.csproj"), std::string::npos);
        EXPECT_NE(evaluated->dump().find("Lib.csproj"), std::string::npos);
        EXPECT_NE(evaluated->dump().find("Greeter.cs"), std::string::npos);
        auto completion = pool.spawn(sched::Lane::UserVisible,
                                     request(pool, service, "textDocument/completion", positionParams(*uri, 97), 15s))
                              .join();
        ASSERT_TRUE(completion) << completion.error().describe();
        EXPECT_NE(completion->dump().find("SayHello"), std::string::npos);
        auto definition = pool.spawn(sched::Lane::UserVisible,
                                     request(pool, service, "textDocument/definition", positionParams(*uri, 98), 15s))
                              .join();
        EXPECT_TRUE(definition && definition->dump().find("Greeter.cs") != std::string::npos);
        // Actual owned host proposals over a referenced-project overlay; source files stay unchanged.
        auto libUri = lang::pathToUri(lib);
        ASSERT_TRUE(libUri);
        const std::string localSource =
            "namespace Library; public class Greeter { public string SayHello() => \"Unsaved\"; public int Compute() { "
            "return 21 + 21; } public int Inline() { var answer = 42; return answer; } public int Multiple() { var "
            "count = 42; return count + count; } }\n";
        auto actionsAt = [&](std::string_view selected, bool cursor = false) {
            const auto start = localSource.find(selected);
            auto params = lang::Json::object().set("textDocument", lang::Json::object().set("uri", *libUri));
            auto position = [](std::size_t column) {
                return lang::Json::object().set("line", 0).set("character", static_cast<int>(column));
            };
            params.set("range", lang::Json::object()
                                    .set("start", position(start))
                                    .set("end", position(start + (cursor ? 0 : selected.size()))));
            params.set("context", lang::Json::object().set("diagnostics", lang::Json::array()));
            return pool
                .spawn(sched::Lane::UserVisible,
                       request(pool, service, "textDocument/codeAction", std::move(params), 15s))
                .join();
        };
        const auto semanticUri = lang::pathToUri(semanticFile);
        ASSERT_TRUE(semanticUri);
        const auto semanticActions = [&](std::string_view selected, bool cursor) {
            const auto start = semanticSource.find(selected);
            EXPECT_NE(start, std::string::npos);
            auto position = [](std::size_t column) {
                return lang::Json::object().set("line", 0).set("character", static_cast<int>(column));
            };
            return pool
                .spawn(sched::Lane::UserVisible,
                       request(pool, service, "textDocument/codeAction",
                               lang::Json::object()
                                   .set("textDocument", lang::Json::object().set("uri", *semanticUri))
                                   .set("range", lang::Json::object()
                                                     .set("start", position(start))
                                                     .set("end", position(start + (cursor ? 0 : selected.size()))))
                                   .set("context", lang::Json::object().set("diagnostics", lang::Json::array())),
                               15s))
                .join();
        };
        const auto signature = semanticActions("int left", true);
        ASSERT_TRUE(signature) << signature.error().describe();
        EXPECT_NE(signature->dump().find("Swap first two parameters"), std::string::npos);
        EXPECT_NE(signature->dump().find("left:"), std::string::npos);
        EXPECT_NE(signature->dump().find("right:"), std::string::npos);
        EXPECT_NE(signature->dump().find("frostManaged"), std::string::npos);
        const auto overloaded = semanticActions("int a,long b", true);
        ASSERT_TRUE(overloaded) << overloaded.error().describe();
        EXPECT_EQ(overloaded->dump().find("Swap first two parameters"), std::string::npos);
        const auto unused = semanticActions("Unused", true);
        ASSERT_TRUE(unused) << unused.error().describe();
        EXPECT_NE(unused->dump().find("Delete private declaration with no source references"), std::string::npos);
        const auto used = semanticActions("Used(){}", true);
        ASSERT_TRUE(used) << used.error().describe();
        EXPECT_EQ(used->dump().find("Delete private declaration with no source references"), std::string::npos);
        const auto layoutField = semanticActions("instanceField", true);
        ASSERT_TRUE(layoutField) << layoutField.error().describe();
        EXPECT_EQ(layoutField->dump().find("Delete private declaration with no source references"), std::string::npos);
        const auto extract = semanticActions("int doubled=input*2; return doubled+1;", false);
        ASSERT_TRUE(extract) << extract.error().describe();
        EXPECT_NE(extract->dump().find("refactor.extract"), std::string::npos);
        const auto move = semanticActions("Extra {}", true);
        ASSERT_TRUE(move) << move.error().describe();
        EXPECT_NE(move->dump().find("refactor.move"), std::string::npos);
        EXPECT_NE(move->dump().find("Extra.cs"), std::string::npos);
        auto unchangedSemantic = fs.readFileSnapshot(semanticFile, 4096U);
        ASSERT_TRUE(unchangedSemantic);
        EXPECT_EQ(*unchangedSemantic, semanticSource);
        auto introduce = actionsAt("21 + 21");
        ASSERT_TRUE(introduce) << introduce.error().describe();
        EXPECT_NE(introduce->dump().find("Introduce local for return value (Roslyn)"), std::string::npos);
        EXPECT_NE(introduce->dump().find("documentChanges"), std::string::npos);
        auto inlineLocal = actionsAt("answer;", true);
        ASSERT_TRUE(inlineLocal) << inlineLocal.error().describe();
        EXPECT_NE(inlineLocal->dump().find("Inline return local (Roslyn)"), std::string::npos);
        auto multiple = actionsAt("count + count", true);
        ASSERT_TRUE(multiple) << multiple.error().describe();
        EXPECT_EQ(multiple->dump().find("Inline return local (Roslyn)"), std::string::npos);
        auto unchangedLibrary = fs.readFileSnapshot(lib, 1024U);
        ASSERT_TRUE(unchangedLibrary);
        EXPECT_EQ(*unchangedLibrary, "namespace Library; public class Greeter {}\n");
        // A real type named @var changes conversion semantics although both forms compile.
        // Neither inferred-local transformation may be offered in that semantic scope.
        auto shadowUri = lang::pathToUri(shadowPath);
        ASSERT_TRUE(shadowUri);
        for (const auto& selected : {std::string("1; } public int Inline"), std::string("answer;")}) {
            const auto start = shadowText.find(selected);
            ASSERT_NE(start, std::string::npos);
            auto position = [](std::size_t column) {
                return lang::Json::object().set("line", 0).set("character", static_cast<int>(column));
            };
            auto parameters = lang::Json::object()
                                  .set("textDocument", lang::Json::object().set("uri", *shadowUri))
                                  .set("range", lang::Json::object()
                                                    .set("start", position(start))
                                                    .set("end", position(start + (selected.starts_with('1') ? 1 : 0))))
                                  .set("context", lang::Json::object().set("diagnostics", lang::Json::array()));
            auto response = pool.spawn(sched::Lane::UserVisible,
                                       request(pool, service, "textDocument/codeAction", std::move(parameters), 15s))
                                .join();
            ASSERT_TRUE(response) << response.error().describe();
            EXPECT_EQ(response->dump().find("Introduce local for return value (Roslyn)"), std::string::npos);
            EXPECT_EQ(response->dump().find("Inline return local (Roslyn)"), std::string::npos);
        }
        auto params = positionParams(*uri, 98);
        params.set("newName", "Welcome");
        auto rename =
            pool.spawn(sched::Lane::UserVisible, request(pool, service, "textDocument/rename", std::move(params), 15s))
                .join();
        ASSERT_TRUE(rename.has_value()) << rename.error().describe();
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
    shutdown.stop();
    EXPECT_EQ(service.state(), lang::LuaState::Stopped);
}
} // namespace
