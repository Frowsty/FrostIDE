// SPDX-License-Identifier: Apache-2.0
// Authored product onboarding refusal: application-only manifests cannot launch untrusted tools.
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/json.hpp"

#include "workbench_rig.hpp"
#include <gtest/gtest.h>
namespace frost::app::test {
TEST(LanguageRegistryOnboarding, InvalidManifestNeverReplacesApplicationSettingOrLaunches) {
    WorkDir dir("registry-invalid");
    io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(dir, options);
    const auto original = rig.settings->snapshot()->get(settings::key::languageServerRegistry);
    EXPECT_FALSE(rig.workbench->setLanguageServerRegistry("{\"version\":2,\"servers\":[]}"));
    EXPECT_EQ(rig.settings->snapshot()->get(settings::key::languageServerRegistry), original);
    EXPECT_TRUE(launcher.launches().empty());
}
TEST(LanguageRegistryOnboarding, ProjectCannotSupplyExecutableRegistryAndUntrustedRootDoesNotLaunch) {
    WorkDir dir("registry-project-control");
    dir.write("workspace/main.py", "value = 1\n");
    io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, options);
    EXPECT_FALSE(rig.settings->set(settings::Scope::ProjectShared, settings::key::languageServerRegistry,
                                   std::string("{\"version\":1,\"servers\":[]}")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("workspace/main.py")));
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_TRUE(rig.workbench->languageServerRegistryState().find("servers")->items().empty());
}
} // namespace frost::app::test
