// SPDX-License-Identifier: Apache-2.0
// Authored discovery regressions; real DbgEng rendering is asserted by separate adapter fixtures.
#include "frost/core/clock.hpp"
#include "frost/debug/natvis_discovery.hpp"
#include "frost/io/mem_fs.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>
using namespace frost;
using frost::debug::test::workPath;
TEST(NatvisDiscovery, ExplicitItemsAndUserDependencyRootsDeduplicateAndKeepDeterministicOrder) {
    ManualClock clock;
    io::MemFs fs(clock);
    ASSERT_TRUE(fs.createDirectories(workPath("project/dependency")));
    ASSERT_TRUE(fs.createDirectories(workPath("user")));
    ASSERT_TRUE(fs.writeFileAtomic(workPath("project/own.natvis"), "<AutoVisualizer/>"));
    ASSERT_TRUE(fs.writeFileAtomic(workPath("project/dependency/Z.NATVIS"), "<AutoVisualizer/>"));
    ASSERT_TRUE(fs.writeFileAtomic(workPath("project/dependency/a.natvis"), "<AutoVisualizer/>"));
    ASSERT_TRUE(fs.writeFileAtomic(workPath("user/user.natvis"), "<AutoVisualizer/>"));
    debug::NatvisDiscovery request;
    request.files = {workPath("user/user.natvis"), workPath("project/own.natvis")};
    request.roots = {workPath("project"), workPath("user")};
    auto result = debug::discoverNatvis(fs, request);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->files, (std::vector<std::string>{workPath("user/user.natvis"), workPath("project/own.natvis"),
                                                       workPath("project/dependency/Z.NATVIS"),
                                                       workPath("project/dependency/a.natvis")}));
    EXPECT_TRUE(result->diagnostics.empty());
}
TEST(NatvisDiscovery, MissingOptionalInputIsDiagnosticButLimitsNeverSilentlyTruncate) {
    ManualClock clock;
    io::MemFs fs(clock);
    ASSERT_TRUE(fs.createDirectories(workPath("p")));
    ASSERT_TRUE(fs.writeFileAtomic(workPath("p/a.natvis"), "<AutoVisualizer/>"));
    ASSERT_TRUE(fs.writeFileAtomic(workPath("p/b.natvis"), "<AutoVisualizer/>"));
    debug::NatvisDiscovery request;
    request.files = {workPath("missing.natvis")};
    auto missing = debug::discoverNatvis(fs, request);
    ASSERT_TRUE(missing) << missing.error().describe();
    EXPECT_TRUE(missing->files.empty());
    EXPECT_EQ(missing->diagnostics.size(), 1);
    request.roots = {workPath("p")};
    request.maxFiles = 1;
    auto bounded = debug::discoverNatvis(fs, request);
    ASSERT_FALSE(bounded);
    EXPECT_EQ(bounded.error().code(), ErrorCode::OutOfRange);
}
TEST(NatvisDiscovery, InvalidPathsAndNonFilesCannotBecomeVisualizerLoadArguments) {
    ManualClock clock;
    io::MemFs fs(clock);
    debug::NatvisDiscovery request;
    request.files = {"relative.natvis"};
    EXPECT_FALSE(debug::discoverNatvis(fs, request));
    ASSERT_TRUE(fs.createDirectories(workPath("directory.natvis")));
    request.files = {workPath("directory.natvis")};
    EXPECT_FALSE(debug::discoverNatvis(fs, request));
    request.files.clear();
    request.roots = {"relative"};
    EXPECT_FALSE(debug::discoverNatvis(fs, request));
}
TEST(NatvisDiscovery, DependencyDirectoryDiscoveryDoesNotWalkBuildTreesOrInventMembership) {
    ManualClock clock;
    io::MemFs fs(clock);
    ASSERT_TRUE(fs.createDirectories(workPath("dependency/build/deep")));
    ASSERT_TRUE(fs.writeFileAtomic(workPath("dependency/Frost.natvis"), "<AutoVisualizer/>"));
    ASSERT_TRUE(fs.writeFileAtomic(workPath("dependency/build/deep/unrelated.natvis"), "<AutoVisualizer/>"));
    debug::NatvisDiscovery request;
    request.directories = {workPath("dependency")};
    auto result = debug::discoverNatvis(fs, request);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->files, (std::vector<std::string>{workPath("dependency/Frost.natvis")}));
}
