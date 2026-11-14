// SPDX-License-Identifier: Apache-2.0
#include "frost/build/dependencies.hpp"
#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include <gtest/gtest.h>
#include <algorithm>

namespace frost::build {
namespace {
std::string path(std::string_view relative) {
#ifdef _WIN32
    return io::joinPath("C:\\work", relative).value();
#else
    return io::joinPath("/work", relative).value();
#endif
}
class DependenciesTest : public testing::Test {
protected:
    void SetUp() override {
        for (const auto folder : {"project", "sdk/scripts/buildsystems", "feed", "profiles", "data"})
            ASSERT_TRUE(fs.createDirectories(path(folder)));
        for (const auto file : {"sdk/vcpkg", "sdk/conan", "sdk/nuget.exe", "profiles/host", "profiles/build"})
            ASSERT_TRUE(fs.writeFileAtomic(path(file), "fixture"));
        ASSERT_TRUE(fs.writeFileAtomic(path("project/vcpkg.json"), "{\"dependencies\":[\"local-only\"]}"));
        ASSERT_TRUE(fs.writeFileAtomic(path("sdk/scripts/buildsystems/vcpkg.cmake"), "# toolchain"));
        auto opened = project::TrustStore::open(fs, path("data/trust"));
        ASSERT_TRUE(opened);
        trust = std::move(*opened);
        ASSERT_TRUE(trust->trust(path("project")));
        ASSERT_TRUE(trust->trust(path("sdk")));
        ASSERT_TRUE(trust->trust(path("feed")));
        ASSERT_TRUE(trust->trust(path("profiles")));
    }
    DependencyRequest request() {
        return {.manager = DependencyManager::Vcpkg, .root = path("project"), .executable = path("sdk/vcpkg"),
            .input = path("project/vcpkg.json"), .outputDirectory = ".frost/dependencies/vcpkg",
            .localFeed = path("feed"), .triplet = "x64-windows"};
    }
    ManualClock clock;
    io::MemFs fs{clock};
    std::unique_ptr<project::TrustStore> trust;
};
TEST_F(DependenciesTest, VcpkgManifestUsesOfflineOverlayAndDisablesImplicitConfigureInstall) {
    auto plan = planDependencies(fs, request());
    ASSERT_TRUE(plan) << plan.error().describe();
    EXPECT_EQ(plan->process.arguments.front(), "install");
    EXPECT_NE(std::ranges::find(plan->process.arguments, "--no-downloads"), plan->process.arguments.end());
    EXPECT_NE(std::ranges::find(plan->cmakeCache, "VCPKG_MANIFEST_INSTALL:BOOL=OFF"), plan->cmakeCache.end());
    EXPECT_EQ(dependencyCmakeProfile(*plan).root, path("project"));
}
TEST_F(DependenciesTest, ChangedManifestOrTrustPreventsInvocationAfterConsent) {
    auto plan = planDependencies(fs, request());
    ASSERT_TRUE(plan);
    auto consent = consentDependencies(*plan, *trust);
    ASSERT_TRUE(consent);
    ASSERT_TRUE(fs.writeFileAtomic(path("project/vcpkg.json"), "changed"));
    io::ScriptedProcessLauncher launcher;
    EXPECT_FALSE(installDependencies(fs, launcher, *trust, *consent));
    EXPECT_TRUE(launcher.launches().empty());
    plan = planDependencies(fs, request());
    ASSERT_TRUE(plan);
    consent = consentDependencies(*plan, *trust);
    ASSERT_TRUE(consent);
    ASSERT_TRUE(trust->untrust(path("feed")));
    EXPECT_FALSE(installDependencies(fs, launcher, *trust, *consent));
    EXPECT_TRUE(launcher.launches().empty());
}
TEST_F(DependenciesTest, ConsentedInvocationReachesActualProcessContractAndCancellationPreventsIt) {
    auto plan = planDependencies(fs, request());
    ASSERT_TRUE(plan);
    auto consent = consentDependencies(*plan, *trust);
    ASSERT_TRUE(consent);
    io::ScriptedProcessLauncher launcher;
    std::stop_source stop;
    stop.request_stop();
    EXPECT_FALSE(installDependencies(fs, launcher, *trust, *consent, stop.get_token()));
    EXPECT_TRUE(launcher.launches().empty());
    launcher.expect({.expectExecutable = path("sdk/vcpkg"), .stdoutBytes = "installed from local overlay\n"});
    auto result = installDependencies(fs, launcher, *trust, *consent);
    ASSERT_TRUE(result);
    ASSERT_EQ(launcher.launches().size(), 1U);
    EXPECT_EQ(launcher.launches().front().spec.arguments, plan->process.arguments);
    EXPECT_EQ(result->stdoutBytes, "installed from local overlay\n");
    auto duplicate = installDependencies(fs, launcher, *trust, *consent);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(launcher.launches().size(), 1U);
}
TEST_F(DependenciesTest, ConanUsesTwoExplicitProfilesAndNoRemotes) {
    ASSERT_TRUE(fs.writeFileAtomic(path("project/conanfile.txt"), "[generators]\n"));
    auto input = request();
    input.manager = DependencyManager::Conan; input.executable = path("sdk/conan");
    input.input = path("project/conanfile.txt"); input.hostProfile = path("profiles/host"); input.buildProfile = path("profiles/build");
    auto plan = planDependencies(fs, input);
    ASSERT_TRUE(plan);
    EXPECT_NE(std::ranges::find(plan->process.arguments, "--no-remote"), plan->process.arguments.end());
    EXPECT_NE(std::ranges::find(plan->process.arguments, "--profile:host=" + input.hostProfile), plan->process.arguments.end());
    EXPECT_NE(std::ranges::find(plan->process.arguments, "--profile:build=" + input.buildProfile), plan->process.arguments.end());
}
TEST_F(DependenciesTest, NativeNugetRequiresExistingPackagesConfigAndExplicitLocalSource) {
    auto input = request(); input.manager = DependencyManager::NativeNuget; input.executable = path("sdk/nuget.exe");
    ASSERT_TRUE(fs.writeFileAtomic(path("project/packages.config"), "<packages/>"));
    input.input = path("project/packages.config"); input.outputDirectory = "packages";
    auto plan = planDependencies(fs, input);
    ASSERT_TRUE(plan);
    EXPECT_NE(std::ranges::find(plan->process.arguments, "-NonInteractive"), plan->process.arguments.end());
    input.localFeed.clear();
    EXPECT_FALSE(planDependencies(fs, input));
    input = request(); input.outputDirectory = "../outside";
    EXPECT_FALSE(planDependencies(fs, input));
}
} // namespace
} // namespace frost::build
