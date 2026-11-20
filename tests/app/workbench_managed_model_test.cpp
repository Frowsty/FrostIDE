// SPDX-License-Identifier: Apache-2.0
// Product SDK context/build/run admission with scripted tools; real SDK fixtures are separate.
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include "workbench_rig.hpp"
#include <QTreeWidget>
namespace {
using namespace std::chrono_literals;
using namespace frost;
using namespace frost::app::test;
lang::Json evaluated(const WorkDir& dir) {
    auto properties = lang::Json::object()
                          .set("TargetPath", dir.join("project/bin/App.dll"))
                          .set("TargetFramework", "net10.0")
                          .set("TargetFrameworks", "net10.0;netstandard2.1")
                          .set("OutputType", "Exe")
                          .set("RuntimeIdentifier", "")
                          .set("MSBuildProjectFullPath", dir.join("project/App.csproj"))
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
} // namespace
