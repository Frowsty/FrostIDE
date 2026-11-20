// SPDX-License-Identifier: Apache-2.0
#include "frost/project/unreal_project.hpp"
#include "frost/io/path.hpp"
#include <gtest/gtest.h>
namespace frost::project {
namespace {
std::string at(std::string_view path) { return io::joinPath(io::kNativePathStyle == io::PathStyle::Windows ? "C:/Synthetic" : "/Synthetic", path).value(); }
TEST(UnrealProject, ReadsAuthoredProjectWithoutEvaluatingBuildRules) {
    auto model = parseUnrealProject(R"({"FileVersion":3,"EngineAssociation":"synthetic-install","Description":"original fixture","Modules":[{"Name":"Oracle","Type":"Runtime","LoadingPhase":"Default"}],"Plugins":[{"Name":"OptionalFeature","Enabled":false,"Optional":true}],"PreBuildSteps":{"Win64":["DO NOT EXECUTE"]}})", at("Oracle.uproject"));
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
    EXPECT_FALSE(parseUnrealProject(R"({"FileVersion":3,"Plugins":[{"Name":"X","Enabled":"true"}]})", at("Oracle.uproject")));
    EXPECT_FALSE(parseUnrealProject(R"({"FileVersion":3,"EngineAssociation":false})", at("Oracle.uproject")));
    EXPECT_FALSE(parseUnrealProject(R"({"FileVersion":3,"Modules":[{"Name":"X","Type":"Runtime"},{"Name":"X","Type":"Editor"}]})", at("Oracle.uproject")));
    EXPECT_FALSE(parseUnrealProject(R"({"FileVersion":999})", at("Oracle.uproject")));
    EXPECT_FALSE(parseUnrealProject(R"({"FileVersion":3})", "relative.uproject"));
    EXPECT_FALSE(parseUnrealPluginDescriptor(R"({"FileVersion":3,"Modules":[{"Name":"../Escape","Type":"Runtime"}]})"));
}
TEST(UnrealProject, ReadsPluginMetadataWithoutLoadingOrInstallingAnyModule) {
    auto model = parseUnrealPluginDescriptor(R"({"FileVersion":3,"FriendlyName":"Synthetic metadata only","Modules":[{"Name":"AuthoredModule","Type":"Editor"}]})");
    ASSERT_TRUE(model);
    EXPECT_EQ(model->modules[0].type, "Editor");
}
}
} // namespace frost::project
