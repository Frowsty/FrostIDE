// SPDX-License-Identifier: Apache-2.0
// Product SDK context/build/run admission with scripted tools; real SDK fixtures are separate.
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include "workbench_rig.hpp"
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>
namespace {
using namespace std::chrono_literals;
using namespace frost;
using namespace frost::app::test;
lang::Json evaluated(const WorkDir& dir, std::string framework = "net10.0", std::string rid = "",
                     std::string projectName = "App.csproj") {
    auto properties = lang::Json::object()
                          .set("TargetPath", dir.join("project/bin/App.dll"))
                          .set("TargetFramework", framework)
                          .set("TargetFrameworks", "net10.0;netstandard2.1")
                          .set("OutputType", "Exe")
                          .set("RuntimeIdentifier", rid)
                          .set("MSBuildProjectFullPath", dir.join("project/" + projectName))
                          .set("UsingMicrosoftNETSdk", "true")
                          .set("AssemblyName", "App")
                          .set("LangVersion", "latest")
                          .set("Nullable", "enable")
                          .set("DefineConstants", "RELEASE;ACTUAL")
                          .set("MSBuildAllProjects", "");
    auto items = lang::Json::object();
    for (const auto kind : {"Compile", "ProjectReference", "PackageReference", "PackageVersion", "Analyzer",
                            "AdditionalFiles", "EditorConfigFiles", "Reference"})
        items.set(kind, lang::Json::array());
    items.set("Compile",
              lang::Json::array().push(
                  lang::Json::object().set("Identity", "Program.cs").set("FullPath", dir.join("project/Program.cs"))));
    return lang::Json::object().set("Properties", properties).set("Items", items);
}
void setup(WorkbenchRig& rig, const WorkDir& dir) {
    ASSERT_TRUE(rig.settings->set(settings::Scope::Application, settings::key::toolsManagedServerPath,
                                  dir.join("missing-host.dll")));
    ASSERT_TRUE(rig.settings->set(settings::Scope::Application, settings::key::toolsLuaLanguageServerPath,
                                  dir.join("missing-luals")));
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
}
TEST(ManagedModelProduct, ActualEvaluatedContextReachesBuildProblemsRunAndImportAdmission) {
    WorkDir dir("managed-model-product");
    dir.write("project/App.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\" />");
    dir.write("project/Program.cs", "bad source\n");
    dir.write("tools/dotnet", "scripted SDK");
    io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, options);
    setup(rig, dir);
    const auto wire = evaluated(dir).dump();
    for (int i = 0; i < 2; ++i)
        launcher.expect({.expectExecutable = dir.join("tools/dotnet"), .stdoutBytes = wire});
    ASSERT_TRUE(rig.workbench->configureManagedProject({.dotnet = dir.join("tools/dotnet"),
                                                        .project = dir.join("project/App.csproj"),
                                                        .configuration = "Release",
                                                        .targetFramework = "net10.0"}));
    ASSERT_TRUE(waitFor([&] { return rig.workbench->managedProjectEvaluation() != nullptr; }, 5s));
    const auto model = rig.workbench->managedProjectEvaluation();
    ASSERT_TRUE(model);
    EXPECT_EQ(model->request.configuration, "Release");
    EXPECT_EQ(model->inputFiles.at(dir.join("project/App.csproj")), "<Project Sdk=\"Microsoft.NET.Sdk\" />");
    ASSERT_EQ(rig.workbench->build().configurations().size(), 1);
    EXPECT_EQ(rig.workbench->build().configurations()[0].name(), "Release|net10.0");
    const auto runs = rig.workbench->managedRunConfigurations();
    ASSERT_EQ(runs.size(), 1);
    EXPECT_EQ(runs[0].arguments, (std::vector<std::string>{dir.join("project/bin/App.dll")}));
    auto guard = rig.workbench->managedRunAdmissionFor(runs[0]);
    ASSERT_TRUE(guard());
    launcher.expect({.expectExecutable = dir.join("tools/dotnet"),
                     .exitCode = 1,
                     .stdoutBytes = dir.join("project/Program.cs") + "(1,1): error CS1001: Identifier expected [" +
                                    dir.join("project/App.csproj") + "]\n"});
    ASSERT_TRUE(rig.workbench->build().build(build::BuildAction::Build));
    ASSERT_TRUE(waitFor(
        [&] { return rig.workbench->build().lastOutcome().has_value() && !rig.workbench->build().isBuilding(); }, 5s));
    EXPECT_FALSE(rig.workbench->build().lastOutcome()->succeeded());
    ASSERT_FALSE(rig.workbench->build().diagnostics().empty());
    EXPECT_EQ(rig.workbench->build().diagnostics()[0].diagnostic.code, "CS1001");
    dir.write("project/App.csproj", "changed input");
    EXPECT_FALSE(guard()) << "captured import bytes must gate queued generated runs";
    rig.workbench->clearManagedProjectContext();
    EXPECT_EQ(rig.workbench->managedProjectEvaluation(), nullptr);
    EXPECT_FALSE(guard());
}
TEST(ManagedModelProduct, ClearingPendingContextTerminatesItsOwnedSdkQueryAndNeverPublishes) {
    WorkDir dir("managed-model-cancel");
    dir.write("project/App.csproj", "<Project />");
    dir.write("tools/dotnet", "scripted SDK");
    io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, options);
    setup(rig, dir);
    launcher.expect({.expectExecutable = dir.join("tools/dotnet"), .neverExits = true});
    ASSERT_TRUE(rig.workbench->configureManagedProject({.dotnet = dir.join("tools/dotnet"),
                                                        .project = dir.join("project/App.csproj"),
                                                        .configuration = "Release",
                                                        .targetFramework = "net10.0"}));
    ASSERT_TRUE(waitFor([&] { return !launcher.launches().empty(); }, 3s));
    rig.workbench->clearManagedProjectContext();
    ASSERT_TRUE(waitFor([&] { return launcher.launches()[0].terminated; }, 3s));
    EXPECT_EQ(rig.workbench->managedProjectEvaluation(), nullptr);
}
TEST(ManagedModelProduct, RapidTrustRevokeRegrantCancelsActiveQueryWithoutGuiTransition) {
    WorkDir dir("managed-model-trust-revision");
    dir.write("project/App.csproj", "<Project />");
    dir.write("tools/dotnet", "scripted SDK");
    io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, options);
    setup(rig, dir);
    launcher.expect({.expectExecutable = dir.join("tools/dotnet"), .neverExits = true});
    ASSERT_TRUE(rig.workbench->configureManagedProject({.dotnet = dir.join("tools/dotnet"),
                                                        .project = dir.join("project/App.csproj"),
                                                        .configuration = "Release",
                                                        .targetFramework = "net10.0"}));
    ASSERT_TRUE(waitFor([&] { return !launcher.launches().empty(); }, 3s));
    // No GUI event dispatch between mutations: final trusted bool is unchanged.
    ASSERT_TRUE(rig.trust->untrust(dir.join("project")));
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(waitFor([&] { return launcher.launches()[0].terminated; }, 3s));
    EXPECT_EQ(rig.workbench->managedProjectEvaluation(), nullptr);
}
TEST(ManagedModelProduct, NormalNuGetAndUnitTestsUseActualSelectedContextAndCancelOnReplacement) {
    WorkDir dir("managed-tools-selected-context");
    dir.write("project/App.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\" />");
    dir.write("tools/dotnet", "scripted actual SDK");
    io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, options);
    setup(rig, dir);
    // Open both normal product panes before a standalone SDK context is selected.
    rig.window->toolWindows().show(app::kNuGetToolWindow, false);
    rig.window->toolWindows().show(app::kUnitTestsToolWindow, false);
    auto* packages = rig.window->toolWindows().content(app::kNuGetToolWindow);
    auto* providers = rig.window->toolWindows().content(app::kUnitTestsToolWindow);
    ASSERT_NE(providers, nullptr);
    auto* tests = providers->findChild<QWidget*>("dotnetTestsWidget");
    ASSERT_NE(packages, nullptr);
    ASSERT_NE(tests, nullptr);
    const auto wire = evaluated(dir).dump();
    for (int i = 0; i < 2; ++i)
        launcher.expect({.expectExecutable = dir.join("tools/dotnet"), .stdoutBytes = wire});
    ASSERT_TRUE(rig.workbench->configureManagedProject({.dotnet = dir.join("tools/dotnet"),
                                                        .project = dir.join("project/App.csproj"),
                                                        .configuration = "Release",
                                                        .targetFramework = "net10.0"}));
    ASSERT_TRUE(waitFor([&] {
        return packages->findChild<QLineEdit*>("managedTargetFramework")->text() == "net10.0" &&
               tests->findChild<QLineEdit*>("managedTargetFramework")->text() == "net10.0";
    }));
    for (auto* widget : {packages, tests}) {
        EXPECT_EQ(widget->findChild<QComboBox*>("managedProjectPath")->currentText().toStdString(),
                  dir.join("project/App.csproj"));
        EXPECT_EQ(widget->findChild<QComboBox*>("managedConfiguration")->currentText(), "Release");
    }
    const auto previous = launcher.launches().size();
    launcher.expect({.expectExecutable = dir.join("tools/dotnet"), .neverExits = true});
    for (auto* button : tests->findChildren<QPushButton*>())
        if (button->text() == "Discover")
            button->click();
    ASSERT_TRUE(waitFor([&] { return launcher.launches().size() > previous; }));
    rig.workbench->clearManagedProjectContext();
    ASSERT_TRUE(waitFor([&] { return launcher.launches()[previous].terminated; }));
    ASSERT_TRUE(waitFor([&] { return tests->findChild<QLineEdit*>("managedTargetFramework")->text().isEmpty(); }));
    EXPECT_EQ(tests->findChild<QTableWidget*>()->rowCount(), 0);
    dir.write("project/Tests.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\" />");
    const auto replacement = evaluated(dir, "netstandard2.1", "linux-x64", "Tests.csproj").dump();
    for (int i = 0; i < 2; ++i)
        launcher.expect({.expectExecutable = dir.join("tools/dotnet"), .stdoutBytes = replacement});
    ASSERT_TRUE(rig.workbench->configureManagedProject({.dotnet = dir.join("tools/dotnet"),
                                                        .project = dir.join("project/Tests.csproj"),
                                                        .configuration = "Custom",
                                                        .targetFramework = "netstandard2.1",
                                                        .runtimeIdentifier = "linux-x64"}));
    ASSERT_TRUE(waitFor([&] {
        return packages->findChild<QLineEdit*>("managedTargetFramework")->text() == "netstandard2.1" &&
               tests->findChild<QLineEdit*>("managedTargetFramework")->text() == "netstandard2.1";
    }));
    for (auto* widget : {packages, tests}) {
        EXPECT_EQ(widget->findChild<QComboBox*>("managedProjectPath")->currentText().toStdString(),
                  dir.join("project/Tests.csproj"));
        EXPECT_EQ(widget->findChild<QComboBox*>("managedConfiguration")->currentText(), "Custom");
        EXPECT_EQ(widget->findChild<QLineEdit*>("managedRuntimeIdentifier")->text(), "linux-x64");
    }
}
} // namespace
