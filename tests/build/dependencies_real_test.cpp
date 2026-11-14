// SPDX-License-Identifier: Apache-2.0
// Later-execution acceptance only. No skips: required configured tools must exist.
#include "frost/build/dependencies.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/vcxproj.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>

namespace frost::build {
namespace {
std::string tool(const char* name) { return project::processEnvironmentVariable(name).value_or(""); }
class DependenciesReal : public testing::Test {
protected:
    void SetUp() override {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        root = io::joinPath(FROST_DEPENDENCY_WORK_PATH, suffix).value();
        ASSERT_TRUE(fs.createDirectories(root));
        auto opened = project::TrustStore::open(fs, io::joinPath(root, "trusted-roots.txt").value());
        ASSERT_TRUE(opened);
        trust = std::move(*opened);
        ASSERT_TRUE(trust->trust(root));
    }
    std::string copyFixture(std::string_view name) {
        const auto source = io::joinPath(FROST_DEPENDENCY_FIXTURES, name).value();
        const auto destination = io::joinPath(root, name).value();
        std::filesystem::copy(io::toStdPath(source).value(), io::toStdPath(destination).value(),
                              std::filesystem::copy_options::recursive);
        return destination;
    }
    Result<DependencyConsent> consent(const DependencyPlan& plan) {
        // Explicit fixture-test consent, scoped to this exact plan and its local input folders.
        for (const auto& folder : plan.trustRoots)
            if (auto grant = trust->trust(folder); !grant)
                return std::unexpected(std::move(grant.error()));
        return consentDependencies(plan, *trust);
    }
    void configureBuildRun(const DependencyPlan& plan, const std::string& source) {
        io::ProcessSpec cmake;
        cmake.executable = tool("FROST_TEST_CMAKE");
        ASSERT_TRUE(io::isAbsolutePath(cmake.executable)) << "set FROST_TEST_CMAKE";
        const auto ninja = tool("FROST_TEST_NINJA");
        ASSERT_TRUE(io::isAbsolutePath(ninja)) << "set FROST_TEST_NINJA";
        const auto binary = io::joinPath(root, "cmake-build").value();
        cmake.arguments = {
            "-S", source, "-B", binary, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_MAKE_PROGRAM=" + ninja};
        for (const auto& variable : plan.cmakeCache)
            cmake.arguments.push_back("-D" + variable);
        auto configured = io::runToCompletion(io::realProcessLauncher(), cmake, {}, std::chrono::minutes(5));
        ASSERT_TRUE(configured) << configured.error().describe();
        ASSERT_EQ(configured->exitCode, 0) << configured->stdoutBytes << configured->stderrBytes;
        cmake.arguments = {"--build", binary};
        auto built = io::runToCompletion(io::realProcessLauncher(), cmake, {}, std::chrono::minutes(5));
        ASSERT_TRUE(built) << built.error().describe();
        ASSERT_EQ(built->exitCode, 0) << built->stdoutBytes << built->stderrBytes;
        io::ProcessSpec run;
#ifdef _WIN32
        run.executable = io::joinPath(binary, "local_dependency.exe").value();
#else
        run.executable = io::joinPath(binary, "local_dependency").value();
#endif
        auto output = io::runToCompletion(io::realProcessLauncher(), run, {}, std::chrono::seconds(30));
        ASSERT_TRUE(output);
        EXPECT_EQ(output->exitCode, 0); // Independent program oracle: header returns 42.
    }
    io::IFileSystem& fs = io::realFileSystem();
    std::string root;
    std::unique_ptr<project::TrustStore> trust;
};
TEST_F(DependenciesReal, PhysicalOutputLinksAndRetargetedExecutablesRefuseBeforeLaunch) {
    const auto project = copyFixture("vcpkg");
    const auto outside = io::joinPath(root, "outside").value();
    ASSERT_TRUE(fs.createDirectories(outside));
    const auto sdkA = io::joinPath(root, "sdk-a").value();
    const auto sdkB = io::joinPath(root, "sdk-b").value();
    for (const auto& sdk : {sdkA, sdkB}) {
        ASSERT_TRUE(fs.createDirectories(io::joinPath(sdk, "scripts/buildsystems").value()));
        ASSERT_TRUE(fs.writeFileAtomic(io::joinPath(sdk, "vcpkg").value(), "fixture tool"));
        ASSERT_TRUE(
            fs.writeFileAtomic(io::joinPath(sdk, "scripts/buildsystems/vcpkg.cmake").value(), "# fixture toolchain"));
    }
    const auto alias = io::joinPath(root, "sdk-alias").value();
    std::error_code error;
    std::filesystem::create_directory_symlink(io::toStdPath(sdkA).value(), io::toStdPath(alias).value(), error);
    ASSERT_FALSE(error) << "Directory-link privilege is required: " << error.message();
    DependencyRequest request{.manager = DependencyManager::Vcpkg,
                              .root = project,
                              .executable = io::joinPath(alias, "vcpkg").value(),
                              .input = io::joinPath(project, "vcpkg.json").value(),
                              .outputDirectory = "escape/installed",
                              .triplet = "x64-windows"};
    std::filesystem::create_directory_symlink(io::toStdPath(outside).value(),
                                              io::toStdPath(io::joinPath(project, "escape").value()).value(), error);
    ASSERT_FALSE(error);
    EXPECT_FALSE(planDependencies(fs, request));
    const auto dangling = io::joinPath(project, "dangling").value();
    std::filesystem::create_directory_symlink(io::toStdPath(io::joinPath(root, "missing").value()).value(),
                                              io::toStdPath(dangling).value(), error);
    ASSERT_FALSE(error);
    request.outputDirectory = "dangling/installed";
    EXPECT_FALSE(planDependencies(fs, request));
    request.outputDirectory = ".frost/installed";
    auto plan = planDependencies(fs, request);
    ASSERT_TRUE(plan);
    auto allowed = consent(*plan);
    ASSERT_TRUE(allowed);
    ASSERT_TRUE(std::filesystem::remove(io::toStdPath(alias).value(), error)); // Only the known link, never recursive.
    ASSERT_FALSE(error);
    std::filesystem::create_directory_symlink(io::toStdPath(sdkB).value(), io::toStdPath(alias).value(), error);
    ASSERT_FALSE(error);
    io::ScriptedProcessLauncher launcher;
    EXPECT_FALSE(installDependencies(fs, launcher, *trust, *allowed));
    EXPECT_TRUE(launcher.launches().empty());
}
class ScopedConanHome {
public:
    explicit ScopedConanHome(const std::string& value) : previous(project::processEnvironmentVariable("CONAN_HOME")) {
#ifdef _WIN32
        _putenv_s("CONAN_HOME", value.c_str());
#else
        setenv("CONAN_HOME", value.c_str(), 1);
#endif
    }
    ~ScopedConanHome() {
#ifdef _WIN32
        _putenv_s("CONAN_HOME", previous.value_or("").c_str());
#else
        if (previous)
            setenv("CONAN_HOME", previous->c_str(), 1);
        else
            unsetenv("CONAN_HOME");
#endif
    }

private:
    std::optional<std::string> previous;
};
TEST_F(DependenciesReal, ConanCachedLocalRecipeGeneratesToolchainAndBuildsConsumer) {
    const auto fixture = copyFixture("conan");
    const auto cache = tool("FROST_TEST_CONAN_HOME");
    ASSERT_TRUE(io::isAbsolutePath(cache))
        << "Set a dedicated preseeded FROST_TEST_CONAN_HOME; fixture recipe is provided";
    ScopedConanHome cacheScope(cache);
    const auto client = io::joinPath(fixture, "client").value();
#ifdef _WIN32
    const auto defaultProfile = io::joinPath(fixture, "windows.profile").value();
#else
    const auto defaultProfile = io::joinPath(fixture, "linux.profile").value();
#endif
    auto host = tool("FROST_TEST_CONAN_HOST_PROFILE");
    auto build = tool("FROST_TEST_CONAN_BUILD_PROFILE");
    DependencyRequest request{.manager = DependencyManager::Conan,
                              .root = client,
                              .executable = tool("FROST_TEST_CONAN"),
                              .input = io::joinPath(client, "conanfile.txt").value(),
                              .outputDirectory = ".frost/generated",
                              .hostProfile = host.empty() ? defaultProfile : host,
                              .buildProfile = build.empty() ? defaultProfile : build};
    auto plan = planDependencies(fs, request);
    ASSERT_TRUE(plan) << plan.error().describe();
    auto allowed = consent(*plan);
    ASSERT_TRUE(allowed);
    auto installed = installDependencies(fs, io::realProcessLauncher(), *trust, *allowed);
    ASSERT_TRUE(installed);
    ASSERT_EQ(installed->exitCode, 0) << installed->stdoutBytes << installed->stderrBytes;
    ASSERT_TRUE(fs.readFile(io::joinPath(plan->resolvedOutput, "conan_toolchain.cmake").value()));
    configureBuildRun(*plan, client);
}
TEST_F(DependenciesReal, VcpkgFilesystemRegistryInstallsAndConfiguresActualConsumer) {
    const auto project = copyFixture("vcpkg");
#ifdef _WIN32
    const auto triplet = "x64-windows";
#else
    const auto triplet = "x64-linux";
#endif
    DependencyRequest request{.manager = DependencyManager::Vcpkg,
                              .root = project,
                              .executable = tool("FROST_TEST_VCPKG"),
                              .input = io::joinPath(project, "vcpkg.json").value(),
                              .outputDirectory = ".frost/installed",
                              .triplet = triplet};
    auto plan = planDependencies(fs, request);
    ASSERT_TRUE(plan) << plan.error().describe();
    auto allowed = consent(*plan);
    ASSERT_TRUE(allowed);
    auto installed = installDependencies(fs, io::realProcessLauncher(), *trust, *allowed);
    ASSERT_TRUE(installed) << installed.error().describe();
    ASSERT_EQ(installed->exitCode, 0) << installed->stdoutBytes << installed->stderrBytes;
    const auto header = io::joinPath(plan->resolvedOutput, std::string(triplet) + "/include/frost_local.hpp").value();
    ASSERT_TRUE(fs.readFile(header));
    configureBuildRun(*plan, project);
}
#ifdef _WIN32
TEST_F(DependenciesReal, NativeNugetRestoresLocalArchiveAndMsbuildConsumesItsTargets) {
    const auto project = copyFixture("native");
    DependencyRequest request{.manager = DependencyManager::NativeNuget,
                              .root = project,
                              .executable = tool("FROST_TEST_NUGET"),
                              .input = io::joinPath(project, "client/packages.config").value(),
                              .outputDirectory = "packages",
                              .localFeed = io::joinPath(project, "feed").value()};
    auto plan = planDependencies(fs, request);
    ASSERT_TRUE(plan) << plan.error().describe();
    auto allowed = consent(*plan);
    ASSERT_TRUE(allowed);
    auto restored = installDependencies(fs, io::realProcessLauncher(), *trust, *allowed);
    ASSERT_TRUE(restored);
    ASSERT_EQ(restored->exitCode, 0) << restored->stdoutBytes << restored->stderrBytes;
    ASSERT_TRUE(fs.readFile(io::joinPath(project, "packages/Frost.Local.1.0.0/include/frost_local.hpp").value()));
    io::ProcessSpec build;
    build.executable = tool("FROST_TEST_MSBUILD");
    ASSERT_TRUE(io::isAbsolutePath(build.executable));
    build.arguments = {io::joinPath(project, "client/client.vcxproj").value(), "/t:Build", "/p:Configuration=Debug",
                       "/p:Platform=x64", "/nologo"};
    auto result = io::runToCompletion(io::realProcessLauncher(), build, {}, std::chrono::minutes(5));
    ASSERT_TRUE(result);
    EXPECT_EQ(result->exitCode, 0) << result->stdoutBytes << result->stderrBytes;
}
#endif
} // namespace
} // namespace frost::build
