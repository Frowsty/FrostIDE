// SPDX-License-Identifier: Apache-2.0
// Trusted managed language service through the real workbench and editors.
#include "frost/io/process.hpp"
#include "frost/lang/managed_service.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "../../src/app/src/unity_context.hpp"
#include "support/managed_profile_guard.hpp"
#include "workbench_rig.hpp"
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#ifndef _WIN32
#include <sys/inotify.h>
#include <sys/stat.h>

#include <cerrno>
#include <filesystem>

#include <unistd.h>
#endif

namespace {
using namespace std::chrono_literals;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
TEST(WorkbenchManaged, TrustAndUnsavedCrossProjectRenameReachEditors) {
    if (!frost::test::sdkTestMayStart())
        GTEST_SKIP()
            << "Windows NuGet LocalAppData migration marker is absent; SDK startup would modify the user profile";
    const std::string dotnet = qgetenv("FROST_TEST_CSHARP_DOTNET").toStdString();
    const std::string assembly = qgetenv("FROST_TEST_CSHARP_SERVER").toStdString();
    if (dotnet.empty() || assembly.empty())
        GTEST_SKIP() << "pinned managed test tools are unavailable";
    WorkDir dir("managed-workbench");
    const std::vector<QByteArray> profileVariables = {"FROST_DOTNET_CLI_HOME",
                                                      "LOCALAPPDATA",
                                                      "APPDATA",
                                                      "HOME",
                                                      "USERPROFILE",
                                                      "XDG_DATA_HOME",
                                                      "XDG_CONFIG_HOME",
                                                      "NUGET_PACKAGES",
                                                      "NUGET_HTTP_CACHE_PATH",
                                                      "NUGET_PLUGINS_CACHE_PATH",
                                                      "FROST_SDK",
                                                      "DOTNET_ROOT"};
    std::vector<QByteArray> previousValues;
    for (const auto& name : profileVariables) {
        previousValues.push_back(qgetenv(name.constData()));
        qputenv(name.constData(), QByteArray::fromStdString(dir.join("profile/" + name.toStdString())));
    }
    qputenv("FROST_SDK", QByteArray::fromStdString(dir.path()));
    qputenv("DOTNET_ROOT", QByteArray::fromStdString(std::string(frost::io::parentPath(dotnet))));
    const auto restoreEnvironment = qScopeGuard([profileVariables, previousValues] {
        for (std::size_t i = 0; i < profileVariables.size(); ++i) {
            if (previousValues[i].isNull())
                qunsetenv(profileVariables[i].constData());
            else
                qputenv(profileVariables[i].constData(), previousValues[i]);
        }
    });
    dir.write("Probe.slnx", R"(<Solution><Project Path="Lib/Lib.csproj"/><Project Path="App/App.csproj"/></Solution>)");
    dir.write(
        "Lib/Lib.csproj",
        R"(<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>)");
    dir.write(
        "App/App.csproj",
        R"(<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup><ItemGroup><ProjectReference Include="../Lib/Lib.csproj"/></ItemGroup></Project>)");
    dir.write("Lib/Greeter.cs", "namespace Library; public class Greeter { public string SayHello() => \"Hello\"; }\n");
    dir.write("App/Example.cs", "using Library;\npublic class Example {\n public string Run() {\n  var g = new "
                                "Greeter();\n  var bad = MissingMethod();\n  return g.SayHello();\n }\n}\n");
    auto options = frost::lang::managedServiceOptions(dotnet, assembly, dir.path(), true);
    options.environment.emplace_back("DOTNET_CLI_HOME", dir.join(".dotnet-home"));
    for (const auto& name : profileVariables)
        if (name != "FROST_DOTNET_CLI_HOME" && name != "FROST_SDK" && name != "DOTNET_ROOT")
            options.environment.emplace_back(name.toStdString(), qgetenv(name.constData()).toStdString());
    frost::io::ProcessSpec restore;
    restore.executable = dotnet;
    restore.arguments = {"restore", dir.join("Probe.slnx"), "--ignore-failed-sources", "--nologo"};
    restore.workingDirectory = dir.path();
    restore.environment = options.environment;
    auto restored = frost::io::runToCompletion(frost::io::realProcessLauncher(), restore, {}, 45s);
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->exitCode, 0) << restored->stdoutBytes << restored->stderrBytes;
    frost::app::test::RigOptions config;
    config.workbench.clangd = false;
    ASSERT_TRUE(frost::app::test::copyTree(frost::io::realFileSystem(), std::string(frost::io::parentPath(assembly)),
                                           dir.join("managed")));
    WorkbenchRig rig(dir, config);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("App/Example.cs")));
    EXPECT_EQ(rig.workbench->managed(), nullptr);
    auto* dialog = rig.window->findChild<frost::ui::TrustDialog*>(QStringLiteral("trustDialog"));
    ASSERT_NE(dialog, nullptr);
    auto* description = dialog->findChild<QLabel*>(QStringLiteral("trustDescription"));
    ASSERT_NE(description, nullptr);
    EXPECT_TRUE(description->text().contains(QStringLiteral("project"), Qt::CaseInsensitive));
    dialog->trustButton()->click();
    ASSERT_TRUE(waitFor(
        [&] { return rig.workbench->managed() && rig.workbench->managed()->state() == frost::lang::LuaState::Running; },
        30s));
    auto* editor = &rig.workbench->currentView()->editor();
    ASSERT_TRUE(waitFor([&] { return !editor->diagnostics().empty(); }, 30s));
    editor->goToLine(6, 14);
    bool hover = waitFor(
        [&] {
            rig.workbench->language()->showQuickDocumentation();
            return waitFor([&] { return rig.workbench->language()->quickDocPopup().isVisible(); }, 2s);
        },
        30s);
    ASSERT_TRUE(hover);
    EXPECT_TRUE(rig.workbench->language()->quickDocPopup().plainText().contains(QStringLiteral("SayHello")));
    rig.workbench->language()->gotoDeclaration();
    ASSERT_TRUE(waitFor(
        [&] {
            auto* v = rig.workbench->currentView();
            return v && v->path() == dir.join("Lib/Greeter.cs");
        },
        15s));
    auto* lib = rig.workbench->currentView();
    lib->editor().goToLine(1, 1);
    ASSERT_TRUE(lib->editor().typeText("// unsaved\n"));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("App/Example.cs"), {6, 14}));
    editor = &rig.workbench->currentView()->editor();
    editor->goToLine(1, 1);
    ASSERT_TRUE(editor->typeText("// unsaved\n"));
    editor->goToLine(7, 14);
    rig.workbench->language()->rename();
    ASSERT_TRUE(waitFor([&] { return rig.workbench->language()->renameInput().isVisible(); }, 15s));
    auto* rename = rig.workbench->language()->renameInput().field();
    rename->setText(QStringLiteral("Welcome"));
    frost::app::test::sendKey(rename, Qt::Key_Return, Qt::NoModifier);
    ASSERT_TRUE(waitFor([&] { return editor->editor().text().text().find("g.Welcome()") != std::string::npos; }, 15s));
    EXPECT_NE(lib->editor().editor().text().text().find("string Welcome()"), std::string::npos);
    EXPECT_TRUE(rig.trust->untrust(dir.path()));
    EXPECT_TRUE(waitFor([&] { return rig.workbench->managed() == nullptr; }, 5s));
    const std::string alternate = dir.join("alternate-server.dll");
    ASSERT_TRUE(QFile::copy(QString::fromStdString(assembly), QString::fromStdString(alternate)));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  alternate));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    // A queued settings callback and a later trust timer tick must not restart a shut-down workbench.
    rig.workbench->shutdown();
    EXPECT_FALSE(
        waitFor([&] { return rig.workbench->managed() != nullptr || rig.workbench->lua() != nullptr; }, 1500ms));
}

TEST(WorkbenchManaged, UnityDiscoveryRequiresOwnedVersionedRegularFiles) {
    WorkDir dir("unity-control-valid");
    const std::string generation(32, '1');
    dir.write("Assets/Probe.cs", "public class Probe {}\n");
    dir.write("Packages/manifest.json", "{}");
    dir.write("ProjectSettings/ProjectVersion.txt", "scratch");
    dir.write(".frost/unity/owner", "FrostIDE Unity project context v1");
    dir.write(".frost/unity/revision", "1:" + generation + ":" + std::string(32, 'a'));
    dir.write(".frost/unity/generations/" + generation + "/FrostUnity.sln", "solution");
    EXPECT_EQ(frost::app::trustedUnityRevision(dir.path()), generation);
    dir.write(".frost/unity/revision", "1:../../bad");
    EXPECT_TRUE(frost::app::trustedUnityRevision(dir.path()).empty());
    dir.write(".frost/unity/owner", "not owned");
    EXPECT_TRUE(frost::app::trustedUnityRevision(dir.path()).empty());
}

#ifndef _WIN32
TEST(WorkbenchManaged, UnityDiscoveryDoesNotOpenUntrustedMetadataAndRejectsFifo) {
    WorkDir dir("unity-untrusted-fifo");
    dir.write("Assets/Probe.cs", "public class Probe {}\n");
    dir.write("Packages/manifest.json", "{}");
    dir.write("ProjectSettings/ProjectVersion.txt", "scratch");
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(dir.join(".frost/unity")));
    const std::string fifo = dir.join(".frost/unity/owner");
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    const int watcher = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    ASSERT_GE(watcher, 0);
    const auto closeWatcher = qScopeGuard([watcher] { ::close(watcher); });
    ASSERT_GE(::inotify_add_watch(watcher, fifo.c_str(), IN_OPEN), 0);
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    char events[1024];
    errno = 0;
    EXPECT_EQ(::read(watcher, events, sizeof(events)), -1);
    EXPECT_EQ(errno, EAGAIN); // Untrusted project opening never opened the FIFO.
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    bool sawTrustedOpen = false;
    EXPECT_TRUE(waitFor(
        [&] {
            if (!sawTrustedOpen)
                sawTrustedOpen = ::read(watcher, events, sizeof(events)) > 0;
            return sawTrustedOpen;
        },
        5s));
    EXPECT_TRUE(frost::app::trustedUnityRevision(dir.path()).empty());
}

TEST(WorkbenchManaged, UnityDiscoveryRefusesLinksAndNonRegularControlFiles) {
    const std::string generation(32, '1');
    const std::string digest(32, 'a');
    for (const std::string kind : {"owner-fifo", "revision-fifo", "solution-fifo", "owner-link", "revision-link",
                                   "solution-link", "parent-link"}) {
        WorkDir dir("unity-control-" + kind);
        dir.write("Assets/Probe.cs", "public class Probe {}\n");
        dir.write("Packages/manifest.json", "{}");
        dir.write("ProjectSettings/ProjectVersion.txt", "scratch");
        const std::string base = kind == "parent-link" ? "redirected/unity/" : ".frost/unity/";
        const std::string solution = base + "generations/" + generation + "/FrostUnity.sln";
        if (kind != "owner-fifo" && kind != "owner-link")
            dir.write(base + "owner", "FrostIDE Unity project context v1");
        if (kind != "revision-fifo" && kind != "revision-link")
            dir.write(base + "revision", "1:" + generation + ":" + digest);
        if (kind != "solution-fifo" && kind != "solution-link")
            dir.write(solution, "solution");
        const auto makeFifo = [&](const std::string& relative) {
            const std::string path = dir.join(relative);
            EXPECT_TRUE(frost::io::realFileSystem().createDirectories(std::string(frost::io::parentPath(path))));
            EXPECT_EQ(::mkfifo(path.c_str(), 0600), 0);
        };
        const auto makeLink = [&](const std::string& relative, const std::string& target) {
            const std::string path = dir.join(relative);
            EXPECT_TRUE(frost::io::realFileSystem().createDirectories(std::string(frost::io::parentPath(path))));
            std::error_code error;
            std::filesystem::create_symlink(dir.join(target), path, error);
            EXPECT_FALSE(error) << error.message();
        };
        if (kind == "owner-fifo")
            makeFifo(base + "owner");
        if (kind == "revision-fifo")
            makeFifo(base + "revision");
        if (kind == "solution-fifo")
            makeFifo(solution);
        if (kind == "owner-link") {
            dir.write("external-owner", "FrostIDE Unity project context v1");
            makeLink(base + "owner", "external-owner");
        }
        if (kind == "revision-link") {
            dir.write("external-revision", "1:" + generation + ":" + digest);
            makeLink(base + "revision", "external-revision");
        }
        if (kind == "solution-link") {
            dir.write("external-solution", "solution");
            makeLink(solution, "external-solution");
        }
        if (kind == "parent-link")
            makeLink(".frost", "redirected");
        EXPECT_TRUE(frost::app::trustedUnityRevision(dir.path()).empty()) << kind;
    }
}
#endif

TEST(WorkbenchManaged, UnityGenerationReloadReplaysDirtyDocuments) {
    if (!frost::test::sdkTestMayStart())
        GTEST_SKIP() << "Windows NuGet migration marker is absent";
    const std::string dotnet = qgetenv("FROST_TEST_CSHARP_DOTNET").toStdString();
    const std::string server = qgetenv("FROST_TEST_CSHARP_SERVER").toStdString();
    if (dotnet.empty() || server.empty())
        GTEST_SKIP() << "pinned managed test tools are unavailable";
    WorkDir dir("unity-managed-reload");
    const std::vector<QByteArray> names = {"FROST_DOTNET_CLI_HOME",
                                           "LOCALAPPDATA",
                                           "APPDATA",
                                           "HOME",
                                           "USERPROFILE",
                                           "XDG_DATA_HOME",
                                           "XDG_CONFIG_HOME",
                                           "NUGET_PACKAGES",
                                           "NUGET_HTTP_CACHE_PATH",
                                           "NUGET_PLUGINS_CACHE_PATH",
                                           "DOTNET_ROOT"};
    std::vector<QByteArray> oldValues;
    for (const auto& name : names) {
        oldValues.push_back(qgetenv(name.constData()));
        qputenv(name.constData(), QByteArray::fromStdString(dir.join("profile/" + name.toStdString())));
    }
    qputenv("DOTNET_ROOT", QByteArray::fromStdString(std::string(frost::io::parentPath(dotnet))));
    const auto restore = qScopeGuard([names, oldValues] {
        for (std::size_t i = 0; i < names.size(); ++i)
            if (oldValues[i].isNull())
                qunsetenv(names[i].constData());
            else
                qputenv(names[i].constData(), oldValues[i]);
    });
    dir.write("Packages/manifest.json", "{}");
    dir.write("ProjectSettings/ProjectVersion.txt", "scratch");
    const std::string source = "public class Example { public string Run() => new NewType().Welcome(); }\n";
    dir.write("Assets/Example.cs", source);
    dir.write("Assets/NewType.cs", "public class NewType { public string Welcome() => \"ok\"; }\n");
    const std::string first(32, '1'), second(32, '2'), digest(32, 'a');
    const auto generation = [&](const std::string& id, bool includeNewType) {
        const std::string prefix = ".frost/unity/generations/" + id + "/";
        const std::string csproj =
            R"(<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework><EnableDefaultCompileItems>false</EnableDefaultCompileItems></PropertyGroup><ItemGroup><Compile Include="../../../../Assets/Example.cs"/>)" +
            std::string(includeNewType ? R"(<Compile Include="../../../../Assets/NewType.cs"/>)" : "") +
            "</ItemGroup></Project>";
        dir.write(prefix + "Probe.csproj", csproj);
        dir.write(prefix + "FrostUnity.sln",
                  "\r\nMicrosoft Visual Studio Solution File, Format Version 12.00\r\n"
                  "Project(\"{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}\") = \"Probe\", \"Probe.csproj\", "
                  "\"{12345678-1234-1234-1234-123456789012}\"\r\n"
                  "EndProject\r\nGlobal\r\n\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\r\n"
                  "\t\tDebug|Any CPU = Debug|Any CPU\r\n\tEndGlobalSection\r\n"
                  "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\r\n"
                  "\t\t{12345678-1234-1234-1234-123456789012}.Debug|Any CPU.ActiveCfg = Debug|Any CPU\r\n"
                  "\tEndGlobalSection\r\nEndGlobal\r\n");
    };
    generation(first, false);
    generation(second, true);
    dir.write(".frost/unity/owner", "FrostIDE Unity project context v1");
    dir.write(".frost/unity/revision", "1:" + first + ":" + digest);
    frost::app::test::RigOptions config;
    config.workbench.clangd = false;
    WorkbenchRig rig(dir, config);
    ASSERT_TRUE(
        rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedDotnetPath, dotnet));
    ASSERT_TRUE(
        rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath, server));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Assets/Example.cs")));
    auto* view = rig.workbench->currentView();
    ASSERT_NE(view, nullptr);
    view->editor().goToLine(1, 1);
    ASSERT_TRUE(view->editor().typeText("public class Unsaved { void X() { MissingUnsaved(); } }\n"));
    const auto hasDiagnostic = [&](std::string_view message) {
        auto* service = rig.workbench->managed();
        if (!service)
            return false;
        auto diagnostics = service->diagnostics().forFile(dir.join("Assets/Example.cs"));
        return diagnostics && std::ranges::any_of(diagnostics->diagnostics, [&](const auto& item) {
                   return item.message.find(message) != std::string::npos;
               });
    };
    ASSERT_TRUE(waitFor([&] { return hasDiagnostic("NewType") && hasDiagnostic("MissingUnsaved"); }, 60s));
    auto* previous = rig.workbench->managed();
    dir.write(".frost/unity/revision", "1:" + second + ":" + digest);
    ASSERT_TRUE(waitFor([&] { return rig.workbench->managed() && rig.workbench->managed() != previous; }, 10s));
    ASSERT_TRUE(waitFor([&] { return hasDiagnostic("MissingUnsaved") && !hasDiagnostic("NewType"); }, 60s));
    EXPECT_TRUE(view->document().dirty());
    EXPECT_NE(view->editor().editor().text().text().find("MissingUnsaved"), std::string::npos);
    auto disk = frost::io::realFileSystem().readFile(dir.join("Assets/Example.cs"));
    ASSERT_TRUE(disk);
    EXPECT_EQ(*disk, source);
}
} // namespace
