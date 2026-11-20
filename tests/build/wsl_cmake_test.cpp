// SPDX-License-Identifier: Apache-2.0
// Pure CMK-16 command/profile provenance cases; no WSL or tool is executed.
#include "frost/build/wsl_cmake.hpp"

#include <gtest/gtest.h>
namespace frost::build {
TEST(WslCMakeProfile, ExactMappedConfigureAndGovernedBuildArguments) {
    RemoteTarget endpoint;
    endpoint.id = "wsl";
    endpoint.name = "Named distro";
    endpoint.distribution = "Ubuntu-24.04";
    endpoint.transportExecutable =
        io::kNativePathStyle == io::PathStyle::Windows ? "C:/Windows/System32/wsl.exe" : "/usr/bin/wsl";
    const std::string local = io::kNativePathStyle == io::PathStyle::Windows ? "C:/project" : "/project";
    endpoint.mappings = {{local, "/mnt/source"}};
    WslCMakeProfile profile;
    profile.id = "fixture";
    profile.name = "Fixture";
    profile.targetId = "wsl";
    profile.localSourceDirectory = local;
    profile.buildDirectory = "/home/builder/output";
    profile.cacheDefinitions = {"TEXT:STRING=two words ; $() literal"};
    auto configure = wslCMakeCommand(profile, endpoint, WslCMakeOperation::Configure, 2);
    ASSERT_TRUE(configure);
    EXPECT_EQ(configure->arguments[1], "/mnt/source");
    EXPECT_EQ(configure->arguments.back(), "-DTEXT:STRING=two words ; $() literal");
    auto build = wslCMakeCommand(profile, endpoint, WslCMakeOperation::Build, 3);
    ASSERT_TRUE(build);
    EXPECT_EQ(build->arguments, (std::vector<std::string>{"--build", "/home/builder/output", "--parallel", "3"}));
    profile.buildDirectory = "/mnt/source/build";
    EXPECT_FALSE(wslCMakeCommand(profile, endpoint, WslCMakeOperation::Configure, 2));
    profile.buildDirectory = "/home/output/../elsewhere";
    EXPECT_FALSE(wslCMakeCommand(profile, endpoint, WslCMakeOperation::Configure, 2));
}
} // namespace frost::build
