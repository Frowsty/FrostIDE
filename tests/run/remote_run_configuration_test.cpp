// SPDX-License-Identifier: Apache-2.0
// R19 pure remote Run configuration contracts; no endpoint is contacted.
#include "frost/run/run_manager.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include <gtest/gtest.h>
namespace frost::run {
TEST(RemoteRunConfiguration, StoreRequiresVersionSixAndRetainsLinuxPathIdentity) {
    RunConfiguration configuration;
    configuration.id="remote.test";configuration.name="Named remote test";configuration.type=RunType::Remote;
    configuration.target="approved-endpoint";configuration.program="/opt/program with spaces/app";configuration.workingDirectory="/home/user/source";
    configuration.arguments={"literal ; $() argument","two words"};
    ASSERT_TRUE(validateRunConfiguration(configuration));
    const std::vector<RunConfiguration> values{configuration};
    auto encoded=serializeRunConfigurations(values);
    auto parsed=parseRunConfigurations(encoded);ASSERT_TRUE(parsed);ASSERT_EQ(parsed->size(),1U);EXPECT_EQ(parsed->front(),configuration);
    auto plan=launchPlanFor(configuration);ASSERT_TRUE(plan);EXPECT_EQ(plan->remoteTarget,configuration.target);EXPECT_EQ(plan->program,configuration.program);
    const auto marker=encoded.find("\"version\": 6");ASSERT_NE(marker,std::string::npos);encoded.replace(marker,12,"\"version\": 5");
    EXPECT_FALSE(parseRunConfigurations(encoded));
}
TEST(RemoteRunConfiguration, ManagerCannotFallBackToNativeOrPrepareLocalDebugging) {
    io::ScriptedProcessLauncher launcher;
    auto manager=RunManager::create(launcher);
    RunConfiguration configuration;
    configuration.id="remote.test";configuration.name="Named target";configuration.type=RunType::Remote;
    configuration.target="endpoint";configuration.program="/bin/program";configuration.workingDirectory="/tmp";
    EXPECT_FALSE(manager->start(configuration));
    EXPECT_TRUE(launcher.launches().empty());
    configuration.beforeLaunch=BeforeLaunch::Build;EXPECT_FALSE(validateRunConfiguration(configuration));
}
} // namespace frost::run
