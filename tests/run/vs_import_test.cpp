// SPDX-License-Identifier: Apache-2.0
#include "frost/run/vs_import.hpp"
#include "frost/io/path.hpp"
#include <gtest/gtest.h>
namespace frost::run {
namespace {
#ifdef _WIN32
const std::string root = "C:/import-workspace";
#else
const std::string root = "/import-workspace";
#endif
TEST(VisualStudioImport, LocalLaunchPreservesExactArgvEnvironmentAndCollisionFreeIds) {
    RunConfiguration existing; existing.id = "vs.launch.1";
    const std::vector taken{existing};
    auto result = importVisualStudioConfigurations(R"json({
      // Visual Studio files accept comments and trailing commas.
      "version":"0.2.1", "configurations":[{"name":"Game", "type":"exe",
      "program":"${workspaceRoot}/bin/game", "currentDir":"${workspaceRoot}",
      "args":["--name", "two words", "$(never-executed)", ""], "env":{"MODE":"debug","OLD":null},}],
    })json", "launch.vs.json", root, taken);
    ASSERT_TRUE(result) << result.error().message();
    ASSERT_EQ(result->configurations.size(), 1U);
    EXPECT_TRUE(result->skipped.empty());
    const auto& config = result->configurations.front();
    EXPECT_NE(config.id, existing.id);
    EXPECT_EQ(config.program, io::joinPath(root, "bin/game").value());
    EXPECT_EQ(config.arguments, (std::vector<std::string>{"--name", "two words", "$(never-executed)", ""}));
    EXPECT_EQ(config.environment[0], (EnvironmentEntry{"MODE", "debug"}));
    EXPECT_EQ(config.environment[1], (EnvironmentEntry{"OLD", std::nullopt}));
    EXPECT_TRUE(config.beforeLaunchTasks.empty());
}
TEST(VisualStudioImport, RemoteCommandsAndUnresolvedVsMacrosNeverBecomeLocalLaunches) {
    const auto result = importVisualStudioConfigurations(R"json({"tasks":[
      {"taskLabel":"remote", "type":"remote", "command":"ssh host erase"},
      {"taskLabel":"dynamic", "type":"launch", "command":"${env.COMSPEC}"},
      {"taskLabel":"multi", "type":"launch", "command":"${workspaceRoot}/tool", "commands":["danger"]},
      {"taskLabel":"build", "type":"launch", "command":"${workspaceRoot}/tool", "args":["all"]}
    ]})json", "tasks.vs.json", root);
    ASSERT_TRUE(result);
    ASSERT_EQ(result->configurations.size(), 1U);
    EXPECT_EQ(result->configurations.front().name, "build");
    EXPECT_EQ(result->skipped.size(), 3U);
}
TEST(VisualStudioImport, WrongTypedExecutionFieldsAndFutureVersionRefuse) {
    const auto malformed = importVisualStudioConfigurations(R"({"configurations":[{"name":"bad","type":7,"program":"app"}]})", "launch.vs.json", root);
    ASSERT_TRUE(malformed);
    EXPECT_TRUE(malformed->configurations.empty());
    ASSERT_EQ(malformed->skipped.size(), 1U);
    EXPECT_FALSE(importVisualStudioConfigurations(R"({"version":"9","tasks":[]})", "tasks.vs.json", root));
    EXPECT_FALSE(importVisualStudioConfigurations("{}", "launch.json", root));
}
}
}
