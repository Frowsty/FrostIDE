// SPDX-License-Identifier: Apache-2.0
// Checks conversion of source admission into the Mono adapter module.
#include "../../src/app/src/mono_adapter_preparation.hpp"
#include <gtest/gtest.h>

namespace frost::app {
namespace {
TEST(MonoAdapterPreparation, BindsMapToAdmittedImmutableSources) {
    ManagedModuleSnapshot snapshot;
    snapshot.source.moduleId = "mvid";
    snapshot.source.sources.emplace("/project/a.cs", "class A {}");
    snapshot.assemblyBytes = "image";
    snapshot.pdbBytes = "pdb";
    MonoSourceMap map{{"/project/a.cs"}, {{0x06000001, 0, 4, 2}}};
    auto module = makeVerifiedMonoModule("/project/a.dll", snapshot, map);
    ASSERT_TRUE(module);
    EXPECT_EQ((*module)->image, "image");
    EXPECT_EQ((*module)->sources.at(0), "/project/a.cs");
    EXPECT_EQ((*module)->points.at(0).methodToken, 0x06000001u);
    snapshot.assemblyBytes = "changed";
    EXPECT_EQ((*module)->image, "image");
    map.points[0].sourceIndex = 1;
    EXPECT_FALSE(makeVerifiedMonoModule("/project/a.dll", snapshot, map));
    map.points[0].sourceIndex = 0;
    map.sources[0] = "/project/other.cs";
    EXPECT_FALSE(makeVerifiedMonoModule("/project/a.dll", snapshot, map));
}
} // namespace
} // namespace frost::app
