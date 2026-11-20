// SPDX-License-Identifier: Apache-2.0
#include "frost/build/managed_packages.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"

#include "build_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
namespace frost::build {
TEST(ManagedPackages, SearchUsesConfiguredCredentialsAndExplicitSourceWithoutShell) {
    ManagedPackageRequest request;
    request.project = {
        test::MemTree::path("sdk/dotnet"), test::MemTree::path("project/App.csproj"), "Debug", "net10.0", {}};
    request.query = PackageQuery::Search;
    request.search = "Json";
    request.source = test::MemTree::path("feed");
    request.prerelease = true;
    auto spec = managedPackageSpec(request);
    ASSERT_TRUE(spec);
    EXPECT_EQ(spec->arguments.front(), "package");
    EXPECT_EQ(spec->arguments.back(), request.source);
    EXPECT_TRUE(spec->killOnClose);
    request.search = "--interactive";
    EXPECT_FALSE(managedPackageSpec(request));
    request.query = PackageQuery::Installed;
    request.search.clear();
    request.source.clear();
    spec = managedPackageSpec(request);
    ASSERT_TRUE(spec);
    EXPECT_NE(std::find(spec->arguments.begin(), spec->arguments.end(), "--no-restore"), spec->arguments.end());
}
TEST(ManagedPackages, PreservesRealFeedIdentityProblemsAndFrameworkVersions) {
    const auto search = parseManagedPackageReport(
        R"({"version":2,"problems":[{"text":"Private feed authentication failed"}],"searchResult":[{"sourceName":"local","packages":[{"id":"Example","latestVersion":"2.1.0-preview"}]}]})",
        PackageQuery::Search);
    ASSERT_TRUE(search);
    ASSERT_EQ(search->packages.size(), 1u);
    EXPECT_EQ(search->packages[0].source, "local");
    EXPECT_EQ(search->problems.size(), 1u);
    const auto installed = parseManagedPackageReport(R"({"version":1,"projects":[{"path":"App.csproj","frameworks":[{"framework":"net10.0","topLevelPackages":[{"id":"Example","requestedVersion":"[1,3)","resolvedVersion":"2.0.0"
}],"transitivePackages":[{"id":"Other","resolvedVersion":"1.0.0","vulnerabilities":[{"severity":"High","advisoryurl":"https://example.test/advisory"}]}]
}]
}]
})", PackageQuery::Vulnerable);
    ASSERT_TRUE(installed);
ASSERT_EQ(installed->packages.size(), 2u);
EXPECT_EQ(installed->packages[0].framework, "net10.0");
EXPECT_TRUE(installed->packages[1].transitive);
EXPECT_NE(installed->packages[1].status.find("High"), std::string::npos);
EXPECT_FALSE(parseManagedPackageReport(R"({"projects":[]})", PackageQuery::Installed));
}
}
TEST(ManagedPackages, InputSnapshotsEnforcePerFileBudgetBeforeParsingAndHonorCancellation) {
    test::MemTree tree;
    tree.write("large.props", std::string(4 * 1024 * 1024 + 1, 'x'));
    const auto snapshots = snapshotManagedPackageInputs(tree.fs, {test::MemTree::path("large.props")});
    ASSERT_FALSE(snapshots);
    EXPECT_EQ(snapshots.error().code(), ErrorCode::OutOfRange);
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled =
        snapshotManagedPackageInputs(tree.fs, {test::MemTree::path("missing.props")}, stop.get_token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code(), ErrorCode::Cancelled);
}
