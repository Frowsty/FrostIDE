// SPDX-License-Identifier: Apache-2.0
// Checks Unreal descriptor parsing and local project inventory boundaries.
#include "frost/io/path.hpp"
#include "frost/project/unreal_project.hpp"

#include "support/directory_link.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>
namespace frost::project {
namespace {
std::string at(std::string_view path) {
    return io::joinPath(io::kNativePathStyle == io::PathStyle::Windows ? "C:/Synthetic" : "/Synthetic", path).value();
}
TEST(UnrealProject, ReadsAuthoredProjectWithoutEvaluatingBuildRules) {
    auto model = parseUnrealProject(
        R"({"FileVersion":3,"EngineAssociation":"synthetic-install","Description":"original fixture","Modules":[{"Name":"Oracle","Type":"Runtime","LoadingPhase":"Default"}],"Plugins":[{"Name":"OptionalFeature","Enabled":false,"Optional":true}],"PreBuildSteps":{"Win64":["DO NOT EXECUTE"]}})",
        at("Oracle.uproject"));
    ASSERT_TRUE(model);
    EXPECT_EQ(model->engineAssociation, "synthetic-install");
    ASSERT_EQ(model->modules.size(), 1U);
    EXPECT_EQ(model->modules[0].name, "Oracle");
    ASSERT_EQ(model->plugins.size(), 1U);
    EXPECT_FALSE(model->plugins[0].enabled);
    EXPECT_TRUE(model->plugins[0].optional);
    auto installed = resolveUnrealInstallation(*model, {{"synthetic-install", at("Engine")}});
    ASSERT_TRUE(installed);
    EXPECT_EQ(installed->root, at("Engine"));
    EXPECT_FALSE(resolveUnrealInstallation(*model, {}));
    EXPECT_FALSE(resolveUnrealInstallation(*model, {{"synthetic-install", at("A")}, {"synthetic-install", at("B")}}));
}
TEST(UnrealProject, RejectsAmbiguousOrMalformedKnownMetadata) {
    EXPECT_FALSE(parseUnrealProject(R"({"FileVersion":3,"Modules":[],"Modules":[]})", at("Oracle.uproject")));
    EXPECT_FALSE(
        parseUnrealProject(R"({"FileVersion":3,"Plugins":[{"Name":"X","Enabled":"true"}]})", at("Oracle.uproject")));
    EXPECT_FALSE(parseUnrealProject(R"({"FileVersion":3,"EngineAssociation":false})", at("Oracle.uproject")));
    EXPECT_FALSE(parseUnrealProject(
        R"({"FileVersion":3,"Modules":[{"Name":"X","Type":"Runtime"},{"Name":"X","Type":"Editor"}]})",
        at("Oracle.uproject")));
    EXPECT_FALSE(parseUnrealProject(R"({"FileVersion":999})", at("Oracle.uproject")));
    EXPECT_FALSE(parseUnrealProject(R"({"FileVersion":3})", "relative.uproject"));
    EXPECT_FALSE(parseUnrealPluginDescriptor(R"({"FileVersion":3,"Modules":[{"Name":"../Escape","Type":"Runtime"}]})"));
}
TEST(UnrealProject, ReadsPluginMetadataWithoutLoadingOrInstallingAnyModule) {
    auto model = parseUnrealPluginDescriptor(
        R"({"FileVersion":3,"FriendlyName":"Synthetic metadata only","Modules":[{"Name":"AuthoredModule","Type":"Editor"}]})");
    ASSERT_TRUE(model);
    EXPECT_EQ(model->modules[0].type, "Editor");
}
TEST(UnrealProject, InventoriesOnlyLocalDescriptorsAndRuleNamesWithoutEvaluatingTheirContents) {
    frost::test::ScratchDir scratch("unreal-metadata");
    auto& fs = io::realFileSystem();
    auto write = [&](std::string_view name, std::string_view bytes) {
        const auto path = scratch.join(name);
        EXPECT_TRUE(fs.createDirectories(io::parentPath(path)));
        EXPECT_TRUE(fs.writeFileAtomic(path, bytes));
    };
    write(
        "Oracle.uproject",
        R"({"FileVersion":3,"Modules":[{"Name":"Oracle","Type":"Runtime"}],"Plugins":[{"Name":"LocalOne","Enabled":false}]})");
    write("Source/OracleEditor.Target.cs", "NOT VALID C#; MUST NOT BE EVALUATED");
    write("Source/Oracle/Oracle.Build.cs", "NOT VALID C#; MUST NOT BE READ AS RULES");
    write("Plugins/Category/LocalOne/LocalOne.uplugin",
          R"({"FileVersion":3,"Modules":[{"Name":"LocalModule","Type":"Editor"}]})");
    write("Plugins/Category/LocalOne/Source/LocalModule/LocalModule.Build.cs", "NOT VALID C#");
    // Epic's descriptor search stops below an already discovered plugin.
    write("Plugins/Category/LocalOne/Nested/Ignore.uplugin", "malformed and intentionally ignored");
    auto model = readUnrealWorkspace(fs, scratch.join("Oracle.uproject"));
    ASSERT_TRUE(model) << model.error().describe();
    ASSERT_EQ(model->targetRules.size(), 1U);
    EXPECT_EQ(model->targetRules[0].name, "OracleEditor");
    ASSERT_EQ(model->localPlugins.size(), 1U);
    EXPECT_EQ(model->localPlugins[0].name, "LocalOne");
    ASSERT_EQ(model->moduleRules.size(), 2U);
    EXPECT_EQ(model->moduleRules[0].owner, "Project");
    EXPECT_EQ(model->moduleRules[1].owner, "plugin:LocalOne");
}

TEST(UnrealProject, RefusesInitialSourceAndPluginsDirectoryLinks) {
    frost::test::ScratchDir scratch("unreal-links");
    auto& fs = io::realFileSystem();
    const auto root = scratch.join("Project"), outside = scratch.join("Outside");
    ASSERT_TRUE(fs.createDirectories(root));
    ASSERT_TRUE(fs.createDirectories(outside));
    const auto project = io::joinPath(root, "Oracle.uproject").value();
    ASSERT_TRUE(fs.writeFileAtomic(project, R"({"FileVersion":3})"));
    for (const std::string name : {"Source", "Plugins"}) {
        const auto link = io::joinPath(root, name).value();
        frost::test::makeDirectoryLink(link, outside);
        auto model = readUnrealWorkspace(fs, project);
        ASSERT_FALSE(model);
        EXPECT_EQ(model.error().code(), ErrorCode::PermissionDenied);
        ASSERT_TRUE(fs.removeFile(link));
    }
}
TEST(UnrealProject, RefusesLinkedSourceInsideAProjectLocalPlugin) {
    frost::test::ScratchDir scratch("unreal-plugin-link");
    auto& fs = io::realFileSystem();
    const auto root = scratch.join("Project"), outside = scratch.join("Outside"),
               plugin = scratch.join("Project/Plugins/LocalOne");
    ASSERT_TRUE(fs.createDirectories(plugin));
    ASSERT_TRUE(fs.createDirectories(outside));
    const auto project = io::joinPath(root, "Oracle.uproject").value();
    ASSERT_TRUE(fs.writeFileAtomic(project, R"({"FileVersion":3})"));
    ASSERT_TRUE(fs.writeFileAtomic(io::joinPath(plugin, "LocalOne.uplugin").value(), R"({"FileVersion":3})"));
    frost::test::makeDirectoryLink(io::joinPath(plugin, "Source").value(), outside);
    auto model = readUnrealWorkspace(fs, project);
    ASSERT_FALSE(model);
    EXPECT_EQ(model.error().code(), ErrorCode::PermissionDenied);
}

} // namespace
} // namespace frost::project
