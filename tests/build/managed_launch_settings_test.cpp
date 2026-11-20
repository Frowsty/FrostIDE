// SPDX-License-Identifier: Apache-2.0
// Launch intent, quoting and refusal contract; actual SDK profile journey is separate.
#include "frost/build/managed_launch_settings.hpp"

#include <gtest/gtest.h>
namespace frost::build {
TEST(ManagedLaunchSettings, PreservesProjectIntentAndReportsUnsupportedProviders) {
    auto profiles = parseManagedLaunchSettings(R"({"profiles":{
        "http":{"commandName":"Project","launchBrowser":true,"launchUrl":"health","applicationUrl":"http://localhost:5142",
        "commandLineArgs":"--mode \"two words\" \"\"","environmentVariables":{"ASPNETCORE_ENVIRONMENT":"Development"}},
        "iis":{"commandName":"IISExpress"},"container":{"commandName":"Docker"}}})");
    ASSERT_TRUE(profiles) << profiles.error().describe();
    ASSERT_EQ(profiles->size(), 3u);
    EXPECT_EQ((*profiles)[0].arguments, (std::vector<std::string>{"--mode", "two words", ""}));
    EXPECT_TRUE((*profiles)[0].launchBrowser);
    EXPECT_EQ((*profiles)[0].environment.at("ASPNETCORE_ENVIRONMENT"), "Development");
    EXPECT_TRUE((*profiles)[0].unavailable.empty());
    EXPECT_FALSE((*profiles)[1].unavailable.empty());
    EXPECT_FALSE((*profiles)[2].unavailable.empty());
}
TEST(ManagedLaunchSettings, RefusesAmbiguityAndNeverDropsExecutionOverrides) {
    EXPECT_FALSE(parseManagedLaunchSettings(R"({"profiles":{"same":{},"same":{}}})"));
    EXPECT_FALSE(parseManagedLaunchSettings(R"({"profiles":{"p":{"commandName":"Project","launchBrowser":"true"}}})"));
    EXPECT_FALSE(
        parseManagedLaunchSettings(R"({"profiles":{"p":{"commandName":"Project","commandLineArgs":"\"bad"}}})"));
    auto unsupported =
        parseManagedLaunchSettings(R"({"profiles":{"p":{"commandName":"Project","executablePath":"other"}}})");
    ASSERT_TRUE(unsupported);
    EXPECT_FALSE(unsupported->front().unavailable.empty());
}
} // namespace frost::build
