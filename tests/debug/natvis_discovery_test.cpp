// SPDX-License-Identifier: Apache-2.0
// Authored discovery regressions; real DbgEng rendering is asserted by separate adapter fixtures.
#include "frost/core/clock.hpp"
#include "frost/debug/natvis_discovery.hpp"
#include "frost/io/mem_fs.hpp"
#include <gtest/gtest.h>
using namespace frost;
TEST(NatvisDiscovery, ExplicitItemsAndUserDependencyRootsDeduplicateAndKeepDeterministicOrder) {
    ManualClock clock; io::MemFs fs(clock, io::PathStyle::Posix);
    ASSERT_TRUE(fs.createDirectories("/project/dependency"));
    ASSERT_TRUE(fs.createDirectories("/user"));
    ASSERT_TRUE(fs.writeFileAtomic("/project/own.natvis", "<AutoVisualizer/>"));
    ASSERT_TRUE(fs.writeFileAtomic("/project/dependency/Z.NATVIS", "<AutoVisualizer/>"));
    ASSERT_TRUE(fs.writeFileAtomic("/project/dependency/a.natvis", "<AutoVisualizer/>"));
    ASSERT_TRUE(fs.writeFileAtomic("/user/user.natvis", "<AutoVisualizer/>"));
    debug::NatvisDiscovery request;
    request.files = {"/user/user.natvis", "/project/own.natvis"}; request.roots = {"/project", "/user"};
    auto result = debug::discoverNatvis(fs, request); ASSERT_TRUE(result);
    EXPECT_EQ(result->files, (std::vector<std::string>{"/user/user.natvis", "/project/own.natvis", "/project/dependency/Z.NATVIS", "/project/dependency/a.natvis"}));
    EXPECT_TRUE(result->diagnostics.empty());
}
TEST(NatvisDiscovery, MissingOptionalInputIsDiagnosticButLimitsNeverSilentlyTruncate) {
    ManualClock clock; io::MemFs fs(clock, io::PathStyle::Posix);
    ASSERT_TRUE(fs.createDirectories("/p"));
    ASSERT_TRUE(fs.writeFileAtomic("/p/a.natvis", "<AutoVisualizer/>"));
    ASSERT_TRUE(fs.writeFileAtomic("/p/b.natvis", "<AutoVisualizer/>"));
    debug::NatvisDiscovery request; request.files = {"/missing.natvis"};
    auto missing = debug::discoverNatvis(fs, request); ASSERT_TRUE(missing);
    EXPECT_TRUE(missing->files.empty()); EXPECT_EQ(missing->diagnostics.size(), 1);
    request.roots = {"/p"}; request.maxFiles = 1;
    auto bounded = debug::discoverNatvis(fs, request); ASSERT_FALSE(bounded);
    EXPECT_EQ(bounded.error().code(), ErrorCode::OutOfRange);
}
TEST(NatvisDiscovery, InvalidPathsAndNonFilesCannotBecomeVisualizerLoadArguments) {
    ManualClock clock; io::MemFs fs(clock, io::PathStyle::Posix);
    debug::NatvisDiscovery request; request.files = {"relative.natvis"};
    EXPECT_FALSE(debug::discoverNatvis(fs, request));
    ASSERT_TRUE(fs.createDirectories("/directory.natvis")); request.files = {"/directory.natvis"};
    EXPECT_FALSE(debug::discoverNatvis(fs, request));
    request.files.clear(); request.roots = {"relative"};
    EXPECT_FALSE(debug::discoverNatvis(fs, request));
}
TEST(NatvisDiscovery, DependencyDirectoryDiscoveryDoesNotWalkBuildTreesOrInventMembership) {
    ManualClock clock; io::MemFs fs(clock, io::PathStyle::Posix);
    ASSERT_TRUE(fs.createDirectories("/dependency/build/deep"));
    ASSERT_TRUE(fs.writeFileAtomic("/dependency/Frost.natvis", "<AutoVisualizer/>"));
    ASSERT_TRUE(fs.writeFileAtomic("/dependency/build/deep/unrelated.natvis", "<AutoVisualizer/>"));
    debug::NatvisDiscovery request; request.directories = {"/dependency"};
    auto result = debug::discoverNatvis(fs, request); ASSERT_TRUE(result);
    EXPECT_EQ(result->files, (std::vector<std::string>{"/dependency/Frost.natvis"}));
}
