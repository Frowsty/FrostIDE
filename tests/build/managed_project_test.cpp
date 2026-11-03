// SPDX-License-Identifier: Apache-2.0
// SDK-project property query and build argument tests without starting a process.

#include "frost/build/managed_project.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"

#include "build_test_util.hpp"
#include <gtest/gtest.h>

#include <chrono>

namespace frost::build {
namespace {

TEST(ManagedProject, QueryAndBuildShareTheSelectedConfigurationFrameworkAndRid) {
    ManagedProjectRequest request{.dotnet = test::MemTree::path("sdk/dotnet"),
                                  .project = test::MemTree::path("project/App.csproj"),
                                  .configuration = "Debug",
                                  .targetFramework = "net10.0",
                                  .runtimeIdentifier = "linux-x64"};
    const auto query = managedProjectQuerySpec(request);
    const auto build = managedProjectBuildSpec(request);
    ASSERT_TRUE(query);
    ASSERT_TRUE(build);
    EXPECT_EQ(query->executable, request.dotnet);
    EXPECT_EQ(query->arguments[1], request.project);
    EXPECT_EQ(query->arguments[4], "-property:Configuration=Debug");
    EXPECT_EQ(query->arguments[5], "-property:SelfContained=false");
    EXPECT_EQ(query->arguments[6], "-property:TargetFramework=net10.0");
    EXPECT_EQ(query->arguments[7], "-property:RuntimeIdentifier=linux-x64");
    EXPECT_EQ(build->arguments[0], "build");
    EXPECT_EQ(build->arguments[3], "Debug");
    EXPECT_EQ(build->arguments[5], "net10.0");
    EXPECT_EQ(build->arguments.back(), "linux-x64");
    EXPECT_TRUE(query->killOnClose);
    EXPECT_TRUE(build->killOnClose);
}

TEST(ManagedProject, DiscoveryAllowsNoFrameworkButBuildRequiresOneAndRejectsPropertyInjection) {
    ManagedProjectRequest request{.dotnet = test::MemTree::path("sdk/dotnet"),
                                  .project = test::MemTree::path("project/App.csproj"),
                                  .configuration = "Debug"};
    EXPECT_TRUE(managedProjectQuerySpec(request));
    EXPECT_FALSE(managedProjectBuildSpec(request));
    request.targetFramework = "net10.0;Other=Injected";
    EXPECT_FALSE(managedProjectQuerySpec(request));
    EXPECT_FALSE(managedProjectBuildSpec(request));
}

TEST(ManagedProject, ParsesMsbuildPropertiesWithoutGuessingTheTargetPath) {
    const auto result = parseManagedProjectProperties(
        R"({"Properties":{"TargetPath":"/project/bin/app.dll","TargetFramework":"net10.0",)"
        R"("TargetFrameworks":"","OutputType":"Exe","RuntimeIdentifier":""}})");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->targetPath, "/project/bin/app.dll");
    EXPECT_EQ(result->targetFramework, "net10.0");
    EXPECT_EQ(result->outputType, "Exe");
    EXPECT_FALSE(parseManagedProjectProperties(R"({"Properties":{"TargetPath":123}})"));
}

TEST(ManagedProject, FinalTargetMustMatchTheExplicitConsoleFrameworkAndRid) {
    ManagedProjectRequest request{.dotnet = test::MemTree::path("sdk/dotnet"),
                                  .project = test::MemTree::path("project/App.csproj"),
                                  .configuration = "Debug",
                                  .targetFramework = "net10.0",
                                  .runtimeIdentifier = "linux-x64"};
    ManagedProjectProperties properties{.targetPath = test::MemTree::path("project/bin/App.dll"),
                                        .targetFramework = "net10.0",
                                        .outputType = "Exe",
                                        .runtimeIdentifier = "linux-x64"};
    ASSERT_TRUE(managedConsoleTarget(request, properties));
    EXPECT_EQ(*managedConsoleTarget(request, properties), properties.targetPath);
    properties.runtimeIdentifier = "win-x64";
    EXPECT_FALSE(managedConsoleTarget(request, properties));
    properties.runtimeIdentifier = "linux-x64";
    properties.outputType = "Library";
    EXPECT_FALSE(managedConsoleTarget(request, properties));
}

TEST(ManagedProject, UntrustedProjectCannotStartBuild) {
    test::MemTree tree;
    ASSERT_TRUE(tree.fs.createDirectories(test::MemTree::path("data")));
    ASSERT_TRUE(tree.fs.createDirectories(test::MemTree::path("project")));
    auto trust = project::TrustStore::open(tree.fs, test::MemTree::path("data/trust.txt"));
    ASSERT_TRUE(trust);
    io::ScriptedProcessLauncher launcher;
    ManagedProjectRequest request{.dotnet = test::MemTree::path("sdk/dotnet"),
                                  .project = test::MemTree::path("project/App.csproj"),
                                  .configuration = "Debug",
                                  .targetFramework = "net10.0"};
    auto denied = startManagedProjectBuild(launcher, **trust, request, {test::MemTree::path("project")}, {});
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code(), ErrorCode::Untrusted);
    EXPECT_TRUE(launcher.launches().empty());
    ASSERT_TRUE(tree.fs.createDirectories(test::MemTree::path("other")));
    ASSERT_TRUE((*trust)->trust(test::MemTree::path("other")));
    auto wrongRoot = startManagedProjectBuild(launcher, **trust, request, {test::MemTree::path("other")}, {});
    ASSERT_FALSE(wrongRoot);
    EXPECT_EQ(wrongRoot.error().code(), ErrorCode::Untrusted);
    EXPECT_TRUE(launcher.launches().empty());
    auto query = startManagedProjectQuery(launcher, **trust, request, {test::MemTree::path("other")}, {});
    ASSERT_FALSE(query);
    EXPECT_EQ(query.error().code(), ErrorCode::Untrusted);
    EXPECT_TRUE(launcher.launches().empty());
}

TEST(ManagedProject, CancellingATrustedManagedBuildEndsItsProcessTree) {
    test::MemTree tree;
    ASSERT_TRUE(tree.fs.createDirectories(test::MemTree::path("data")));
    ASSERT_TRUE(tree.fs.createDirectories(test::MemTree::path("project")));
    auto trust = project::TrustStore::open(tree.fs, test::MemTree::path("data/trust.txt"));
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(test::MemTree::path("project")));
    io::ScriptedProcessLauncher launcher;
    ManagedProjectRequest request{.dotnet = test::MemTree::path("sdk/dotnet"),
                                  .project = test::MemTree::path("project/App.csproj"),
                                  .configuration = "Debug",
                                  .targetFramework = "net10.0"};
    launcher.expect({.expectExecutable = request.dotnet, .neverExits = true});
    auto session = startManagedProjectBuild(launcher, **trust, request, {test::MemTree::path("project")}, {});
    ASSERT_TRUE(session);
    (*session)->cancel();
    auto result = (*session)->waitFor(std::chrono::seconds(5));
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->cancelled);
    ASSERT_EQ(launcher.launches().size(), 1U);
    EXPECT_TRUE(launcher.launches().front().terminated);
}

} // namespace
} // namespace frost::build
