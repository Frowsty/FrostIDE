// SPDX-License-Identifier: Apache-2.0
// Authored public API product refusals, persistence and command palette entry points.
#include "frost/io/scripted_process_launcher.hpp"

#include "workbench_rig.hpp"
#include <gtest/gtest.h>
namespace frost::app::test {
TEST(ExtensionOnboarding, ExactApiValidationAndApplicationScopePrecedeAnyExecution) {
    WorkDir dir("extension-api");
    io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    const auto original = rig.settings->snapshot()->get(settings::key::extensionCatalog);
    EXPECT_FALSE(rig.workbench->setExtensionCatalog("{\"apiVersion\":2,\"extensions\":[]}"));
    EXPECT_EQ(rig.settings->snapshot()->get(settings::key::extensionCatalog), original);
    EXPECT_FALSE(rig.settings->set(settings::Scope::ProjectShared, settings::key::extensionCatalog, original));
    EXPECT_FALSE(rig.workbench->executeExtensionCommand("missing/command"));
    EXPECT_FALSE(rig.workbench->debugWithExtension("missing/debug"));
    EXPECT_TRUE(launcher.launches().empty());
    ASSERT_TRUE(rig.workbench->setExtensionCatalog("{\"apiVersion\":1,\"extensions\":[]}"));
    auto state = rig.workbench->extensionCatalogState();
    EXPECT_EQ(state.unsignedAt("apiVersion"), 1u);
    ASSERT_NE(state.find("extensions"), nullptr);
    EXPECT_TRUE(state.find("extensions")->items().empty());
}
} // namespace frost::app::test
