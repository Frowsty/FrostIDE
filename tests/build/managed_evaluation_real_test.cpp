// SPDX-License-Identifier: Apache-2.0
// Later real SDK oracle consumes authored linked/removed/multitarget fixture without restore.
#include "frost/build/managed_evaluation.hpp"
#include "frost/io/file_system.hpp"
#include "frost/project/vcxproj.hpp"

#include "support/managed_profile_guard.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
namespace frost::build {
namespace {
TEST(ManagedEvaluationReal, EvaluatedItemsAndCompilerContextFollowActualSdkInnerBuilds) {
    ASSERT_TRUE(test::sdkTestMayStart()) << "SDK startup would change an unprepared Windows NuGet profile";
    const auto dotnet = project::processEnvironmentVariable("FROST_TEST_CSHARP_DOTNET").value_or("");
    ASSERT_TRUE(io::isAbsolutePath(dotnet)) << "Required configured SDK missing";
    test::ScratchDir scratch("evaluated-sdk");
    const auto destination = scratch.join("fixture");
    std::error_code error;
    std::filesystem::copy(io::toStdPath(FROST_EVALUATED_SDK_FIXTURE).value(), io::toStdPath(destination).value(),
                          std::filesystem::copy_options::recursive, error);
    ASSERT_FALSE(error) << error.message();
    for (const auto framework : {"net10.0", "netstandard2.1"}) {
        ManagedProjectRequest request{.dotnet = dotnet,
                                      .project = io::joinPath(destination, "Library/Library.csproj").value(),
                                      .configuration = "Debug",
                                      .targetFramework = framework};
        auto spec = managedProjectEvaluationSpec(request);
        ASSERT_TRUE(spec);
        for (const auto* name : {"DOTNET_CLI_HOME", "HOME", "USERPROFILE", "LOCALAPPDATA", "APPDATA", "NUGET_PACKAGES",
                                 "NUGET_HTTP_CACHE_PATH", "NUGET_PLUGINS_CACHE_PATH"})
            spec->environment.emplace_back(name, scratch.join(std::string("profile/") + name));
        spec->environment.emplace_back("DOTNET_CLI_TELEMETRY_OPTOUT", "1");
        spec->environment.emplace_back("DOTNET_SKIP_FIRST_TIME_EXPERIENCE", "1");
        spec->environment.emplace_back("DOTNET_GENERATE_ASPNET_CERTIFICATE", "0");
        spec->environment.emplace_back("DOTNET_ADD_GLOBAL_TOOLS_TO_PATH", "0");
        auto actual = io::runToCompletion(io::realProcessLauncher(), *spec, {}, std::chrono::minutes(2));
        ASSERT_TRUE(actual) << actual.error().describe();
        ASSERT_EQ(actual->exitCode, 0) << actual->stdoutBytes << actual->stderrBytes;
        auto model = parseManagedProjectEvaluation(request, actual->stdoutBytes);
        ASSERT_TRUE(model) << model.error().describe();
        const auto& compile = model->items.at("Compile");
        EXPECT_TRUE(std::ranges::any_of(compile, [](const auto& entry) { return entry.identity == "Greeter.cs"; }));
        EXPECT_FALSE(std::ranges::any_of(compile, [](const auto& entry) { return entry.identity == "Removed.cs"; }));
        EXPECT_TRUE(std::ranges::any_of(compile, [](const auto& entry) {
            auto link = entry.metadata.find("Link");
            return link != entry.metadata.end() && link->second == "Shared/Linked.cs";
        }));
        EXPECT_EQ(model->defineConstants.find("MODERN_TFM") != std::string::npos,
                  std::string_view(framework) == "net10.0");
        EXPECT_EQ(model->properties.at("ManagePackageVersionsCentrally"), "true");
        EXPECT_TRUE(std::ranges::any_of(model->items.at("PackageVersion"), [](const auto& entry) {
            return entry.identity == "Example.Central" && entry.metadata.at("Version") == "1.2.3";
        }));
        EXPECT_FALSE(io::realFileSystem()
                         .exists(io::joinPath(destination, "Library/obj/project.assets.json").value())
                         .value_or(true))
            << "evaluation must not restore";
    }
}
} // namespace
} // namespace frost::build
