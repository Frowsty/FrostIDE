// SPDX-License-Identifier: Apache-2.0
// Mandatory offline acceptance over an authored package and a real SDK. No public-feed dependency.
#include "frost/build/managed_packages.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/vcxproj.hpp"

#include "support/managed_profile_guard.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

namespace frost::build {
TEST(ManagedPackagesReal, LocalFeedRestoreSearchAndResolvedVersionsUseActualSdk) {
    ASSERT_TRUE(test::sdkTestMayStart());
    const auto dotnet = project::processEnvironmentVariable("FROST_TEST_CSHARP_DOTNET").value_or("");
    ASSERT_TRUE(io::isAbsolutePath(dotnet)) << "Required configured SDK is missing";
    test::ScratchDir scratch("nuget-local-feed");
    auto& fs = io::realFileSystem();
    const auto library = scratch.join("Library/Library.csproj"), consumer = scratch.join("Consumer/Consumer.csproj"),
               feed = scratch.join("feed"), config = scratch.join("NuGet.Config");
    ASSERT_TRUE(fs.createDirectories(scratch.join("Library")));
    ASSERT_TRUE(fs.createDirectories(scratch.join("Consumer")));
    ASSERT_TRUE(fs.createDirectories(feed));
    ASSERT_TRUE(fs.writeFileAtomic(config, "<configuration><packageSources><clear/></packageSources></configuration>"));
    ASSERT_TRUE(fs.writeFileAtomic(
        library,
        "<Project "
        "Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net10.0</"
        "TargetFramework><PackageId>FrostFixture.Local</PackageId><Version>1.2.3</Version></PropertyGroup></Project>"));
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("Library/Marker.cs"), "public class Marker {}"));
    const auto run = [&](std::vector<std::string> arguments) -> Result<io::ProcessResult> {
        io::ProcessSpec spec;
        spec.executable = dotnet;
        spec.arguments = std::move(arguments);
        spec.workingDirectory = scratch.path();
        spec.stdinMode = io::Stdio::Null;
        spec.stdoutMode = io::Stdio::Pipe;
        spec.stderrMode = io::Stdio::Pipe;
        spec.killOnClose = true;
        for (const auto* name : {"DOTNET_CLI_HOME", "HOME", "USERPROFILE", "LOCALAPPDATA", "APPDATA", "NUGET_PACKAGES",
                                 "NUGET_HTTP_CACHE_PATH", "NUGET_PLUGINS_CACHE_PATH"})
            spec.environment.emplace_back(name, scratch.join(std::string("profile/") + name));
        for (const auto* name : {"DOTNET_CLI_TELEMETRY_OPTOUT", "DOTNET_SKIP_FIRST_TIME_EXPERIENCE"})
            spec.environment.emplace_back(name, "1");
        spec.environment.emplace_back("DOTNET_GENERATE_ASPNET_CERTIFICATE", "0");
        spec.environment.emplace_back("DOTNET_ADD_GLOBAL_TOOLS_TO_PATH", "0");
        return io::runToCompletion(io::realProcessLauncher(), spec, {}, std::chrono::minutes(3));
    };
    auto restored = run({"restore", library, "--configfile", config});
    ASSERT_TRUE(restored);
    ASSERT_EQ(restored->exitCode, 0) << restored->stdoutBytes << restored->stderrBytes;
    auto packed = run({"pack", library, "--no-restore", "--configuration", "Release", "--output", feed});
    ASSERT_TRUE(packed);
    ASSERT_EQ(packed->exitCode, 0) << packed->stdoutBytes << packed->stderrBytes;
    ASSERT_TRUE(
        fs.writeFileAtomic(consumer, "<Project "
                                     "Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net10.0</"
                                     "TargetFramework></PropertyGroup><ItemGroup><PackageReference "
                                     "Include=\"FrostFixture.Local\" Version=\"1.2.3\"/></ItemGroup></Project>"));
    ManagedPackageRequest request;
    request.project = {dotnet, consumer, "Debug", "net10.0", {}};
    request.query = PackageQuery::Restore;
    request.source = feed;
    auto spec = managedPackageSpec(request);
    ASSERT_TRUE(spec);
    auto resolved = run(spec->arguments);
    ASSERT_TRUE(resolved);
    ASSERT_EQ(resolved->exitCode, 0) << resolved->stdoutBytes << resolved->stderrBytes;
    request.query = PackageQuery::Installed;
    request.source.clear();
    spec = managedPackageSpec(request);
    ASSERT_TRUE(spec);
    auto listed = run(spec->arguments);
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->exitCode, 0) << listed->stdoutBytes << listed->stderrBytes;
    auto report = parseManagedPackageReport(listed->stdoutBytes, request.query);
    ASSERT_TRUE(report);
    EXPECT_TRUE(std::ranges::any_of(report->packages, [](const auto& package) {
        return package.id == "FrostFixture.Local" && package.version == "1.2.3" && package.framework == "net10.0";
    }));
    request.query = PackageQuery::Search;
    request.search = "FrostFixture.Local";
    request.source = feed;
    request.exactMatch = true;
    spec = managedPackageSpec(request);
    ASSERT_TRUE(spec);
    auto searched = run(spec->arguments);
    ASSERT_TRUE(searched);
    ASSERT_EQ(searched->exitCode, 0) << searched->stdoutBytes << searched->stderrBytes;
    auto hits = parseManagedPackageReport(searched->stdoutBytes, request.query);
    ASSERT_TRUE(hits);
    EXPECT_TRUE(std::ranges::any_of(hits->packages, [](const auto& package) {
        return package.id == "FrostFixture.Local" && package.version == "1.2.3";
    }));
}
} // namespace frost::build
