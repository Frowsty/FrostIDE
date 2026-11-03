// SPDX-License-Identifier: Apache-2.0
// Live application readiness follows window, startup and current project scan.
#include "workbench_rig.hpp"
#include <gtest/gtest.h>
namespace {
TEST(UpdateReadiness, RequiresShownWindowStartupAndCurrentScan) {
    frost::app::test::WorkDir dir("update-readiness", "tiny_sln");
    frost::app::test::WorkbenchRig rig(dir);
    EXPECT_FALSE(rig.workbench->interactiveReady());
    rig.workbench->markInteractiveStartupComplete();
    EXPECT_FALSE(rig.workbench->interactiveReady());
    rig.show();
    EXPECT_TRUE(rig.workbench->interactiveReady());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    ASSERT_TRUE(frost::app::test::waitFor([&] { return rig.workbench->interactiveReady(); }));
    EXPECT_TRUE(rig.nav().initialScanReady());
    rig.window->hide();
    EXPECT_FALSE(rig.workbench->interactiveReady());
}
} // namespace
