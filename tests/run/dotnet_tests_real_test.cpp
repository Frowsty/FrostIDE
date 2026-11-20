// SPDX-License-Identifier: Apache-2.0
// Real adapter acceptance. Explicit local feed required; no public feeds, no missing-tool skips.
#include "frost/build/managed_packages.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/vcxproj.hpp"
#include "frost/run/dotnet_tests.hpp"

#include "support/managed_profile_guard.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
namespace frost::run {
TEST(DotnetTestsReal, ActualAdapterDiscoveryReportsAndFailedMethodSelectionAgree) {
    ASSERT_TRUE(test::sdkTestMayStart());
    const auto dotnet = project::processEnvironmentVariable("FROST_TEST_CSHARP_DOTNET").value_or("");
    const auto feed = project::processEnvironmentVariable("FROST_TEST_DOTNET_TEST_FEED").value_or("");
    ASSERT_TRUE(io::isAbsolutePath(dotnet));
    ASSERT_TRUE(io::isAbsolutePath(feed))
        << "Provision the fixture's exact test SDK/adapter packages and dependencies in an explicit local feed";
    test::ScratchDir scratch("dotnet-adapter");
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(scratch.join("project")));
    ASSERT_TRUE(fs.createDirectories(scratch.join("report")));
    for (const auto* file : {"Tests.csproj", "Tests.cs"}) {
        auto contents = fs.readFile(io::joinPath(FROST_DOTNET_TEST_FIXTURE, file).value());
        ASSERT_TRUE(contents);
        ASSERT_TRUE(fs.writeFileAtomic(scratch.join(std::string("project/") + file), *contents));
    }
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("NuGet.Config"),
                                   "<configuration><packageSources><clear/></packageSources></configuration>"));
    const auto execute = [&](io::ProcessSpec spec) {
        for (const auto* name : {"DOTNET_CLI_HOME", "HOME", "USERPROFILE", "LOCALAPPDATA", "APPDATA", "NUGET_PACKAGES",
                                 "NUGET_HTTP_CACHE_PATH", "NUGET_PLUGINS_CACHE_PATH"})
            spec.environment.emplace_back(name, scratch.join(std::string("profile/") + name));
        spec.environment.emplace_back("DOTNET_CLI_TELEMETRY_OPTOUT", "1");
        spec.environment.emplace_back("DOTNET_SKIP_FIRST_TIME_EXPERIENCE", "1");
        spec.environment.emplace_back("DOTNET_GENERATE_ASPNET_CERTIFICATE", "0");
        spec.environment.emplace_back("DOTNET_ADD_GLOBAL_TOOLS_TO_PATH", "0");
        return io::runToCompletion(io::realProcessLauncher(), spec, {}, std::chrono::minutes(3));
    };
    build::ManagedPackageRequest restore;
    restore.project = {dotnet, scratch.join("project/Tests.csproj"), "Debug", "net10.0", {}};
    restore.query = build::PackageQuery::Restore;
    restore.source = feed;
    auto spec = build::managedPackageSpec(restore);
    ASSERT_TRUE(spec);
    auto restored = execute(*spec);
    ASSERT_TRUE(restored);
    ASSERT_EQ(restored->exitCode, 0) << restored->stdoutBytes << restored->stderrBytes;
    DotnetTestRequest request;
    request.project = restore.project;
    request.reportDirectory = scratch.join("report");
    request.discover = true;
    request.build = true;
    spec = dotnetTestSpec(request);
    ASSERT_TRUE(spec);
    auto discovered = execute(*spec);
    ASSERT_TRUE(discovered);
    ASSERT_EQ(discovered->exitCode, 0) << discovered->stdoutBytes << discovered->stderrBytes;
    auto names = parseDotnetTestDiscovery(discovered->stdoutBytes);
    ASSERT_TRUE(names);
    EXPECT_EQ(names->size(), 3u);
    request.discover = false;
    request.build = false;
    spec = dotnetTestSpec(request);
    ASSERT_TRUE(spec);
    auto run = execute(*spec);
    ASSERT_TRUE(run);
    EXPECT_NE(run->exitCode, 0);
    auto reportBytes = fs.readFile(scratch.join("report/results.trx"));
    ASSERT_TRUE(reportBytes);
    auto results = parseDotnetTrx(*reportBytes);
    ASSERT_TRUE(results);
    ASSERT_EQ(results->size(), 3u);
    for (const auto outcome : {TestOutcome::Passed, TestOutcome::Failed, TestOutcome::Skipped})
        EXPECT_EQ(
            std::ranges::count_if(*results, [outcome](const auto& result) { return result.result.outcome == outcome; }),
            1);
    const auto failed =
        std::ranges::find_if(*results, [](const auto& result) { return result.result.outcome == TestOutcome::Failed; });
    ASSERT_NE(failed, results->end());
    ASSERT_FALSE(failed->fullyQualifiedName.empty());
    ASSERT_FALSE(failed->result.failures.empty());
    EXPECT_NE(failed->result.failures.front().message.find("Intentional adapter failure"), std::string::npos);
    ASSERT_TRUE(fs.createDirectories(scratch.join("selected")));
    request.reportDirectory = scratch.join("selected");
    request.selected = {failed->fullyQualifiedName};
    spec = dotnetTestSpec(request);
    ASSERT_TRUE(spec);
    auto selected = execute(*spec);
    ASSERT_TRUE(selected);
    EXPECT_NE(selected->exitCode, 0);
    reportBytes = fs.readFile(scratch.join("selected/results.trx"));
    ASSERT_TRUE(reportBytes);
    auto selectedReport = parseDotnetTrx(*reportBytes);
    ASSERT_TRUE(selectedReport);
    ASSERT_EQ(selectedReport->size(), 1u);
    EXPECT_EQ(selectedReport->front().fullyQualifiedName, failed->fullyQualifiedName);
}
} // namespace frost::run
