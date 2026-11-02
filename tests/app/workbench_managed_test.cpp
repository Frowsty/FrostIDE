// SPDX-License-Identifier: Apache-2.0
// Trusted managed language service through the real workbench and editors.
#include "frost/io/process.hpp"
#include "frost/lang/managed_service.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "support/managed_profile_guard.hpp"
#include "workbench_rig.hpp"
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>

#include <chrono>
#include <cstdlib>

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
} // namespace
