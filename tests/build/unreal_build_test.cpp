// SPDX-License-Identifier: Apache-2.0
// Checks UnrealBuildTool argument handling and cancellation for trusted launches.
#include "frost/build/unreal_build.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"

#include "build_test_util.hpp"
#include <gtest/gtest.h>

#include <chrono>
namespace frost::build {
namespace {
UnrealBuildRequest request() {
    return {.tool = test::MemTree::path("Tools/dotnet"),
            .buildToolAssembly = test::MemTree::path("Engine/UnrealBuildTool.dll"),
            .projectFile = test::MemTree::path("Game With Spaces/Oracle.uproject"),
            .target = "OracleEditor",
            .headerManifest = {}};
}
TEST(UnrealBuild, PreservesSelectedToolAndProjectAsSingleArguments) {
    auto input = request();
    input.operation = UnrealOperation::GenerateCompileDatabase;
    auto spec = unrealBuildSpec(input);
    ASSERT_TRUE(spec);
    EXPECT_EQ(spec->executable, input.tool);
    EXPECT_EQ(spec->arguments,
              (std::vector<std::string>{input.buildToolAssembly, "OracleEditor", "Win64", "Development",
                                        "-Project=" + input.projectFile, "-Mode=GenerateClangDatabase"}));
    EXPECT_TRUE(spec->killOnClose);
    EXPECT_EQ(spec->stdinMode, io::Stdio::Null);
    input.operation = UnrealOperation::HeaderTool;
    EXPECT_FALSE(unrealBuildSpec(input));
    input.headerManifest = test::MemTree::path("Game With Spaces/Oracle.uhtmanifest");
    spec = unrealBuildSpec(input);
    ASSERT_TRUE(spec);
    EXPECT_EQ(spec->arguments[1], "-Mode=UnrealHeaderTool");
    EXPECT_EQ(spec->arguments.back(), input.headerManifest);
}
TEST(UnrealBuild, RejectsRelativePathsShellWrappersAndTargetSwitches) {
    auto input = request();
    input.target = "-NoBuild";
    EXPECT_FALSE(unrealBuildSpec(input));
    input = request();
    input.tool = test::MemTree::path("Build.CMD");
    EXPECT_FALSE(unrealBuildSpec(input));
    input = request();
    input.projectFile = "relative.uproject";
    EXPECT_FALSE(unrealBuildSpec(input));
}
TEST(UnrealBuild, RefusesUntrustedProjectWithZeroLaunchesAndCancelsOwnedTree) {
    test::MemTree tree;
    auto trust = project::TrustStore::open(tree.fs, test::MemTree::path("trust.txt"));
    ASSERT_TRUE(trust);
    io::ScriptedProcessLauncher launcher;
    auto input = request();
    auto session = startUnrealBuild(launcher, **trust, input, {});
    ASSERT_FALSE(session);
    EXPECT_EQ(session.error().code(), ErrorCode::Untrusted);
    EXPECT_TRUE(launcher.launches().empty());
    ASSERT_TRUE((*trust)->trust(test::MemTree::path("Game With Spaces")));
    launcher.expect({.expectExecutable = input.tool, .neverExits = true});
    session = startUnrealBuild(launcher, **trust, input, {});
    ASSERT_TRUE(session);
    (*session)->cancel();
    auto outcome = (*session)->waitFor(std::chrono::seconds(5));
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(outcome->cancelled);
    ASSERT_EQ(launcher.launches().size(), 1U);
    EXPECT_TRUE(launcher.launches().front().terminated);
}
} // namespace
} // namespace frost::build
