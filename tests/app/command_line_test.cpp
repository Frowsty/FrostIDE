// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost's command-line parser: modes, positional paths, the "--"
// separator, conflicting modes and unknown options.

#include "frost/app/command_line.hpp"

#include <gtest/gtest.h>

#include <string_view>
#include <vector>

namespace {

using frost::app::Mode;

frost::Result<frost::app::CommandLine> parse(std::vector<std::string_view> args) {
    return frost::app::parseCommandLine(args);
}

TEST(CommandLine, NoArgumentsStartsGui) {
    const auto parsed = parse({});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->mode, Mode::Gui);
    EXPECT_TRUE(parsed->paths.empty());
}

TEST(CommandLine, RecognisesModes) {
    EXPECT_EQ(parse({"--version"})->mode, Mode::Version);
    EXPECT_EQ(parse({"--help"})->mode, Mode::Help);
    EXPECT_EQ(parse({"--selftest"})->mode, Mode::SelfTest);
    const auto onscreen = parse({"--selftest=onscreen"});
    EXPECT_EQ(onscreen->mode, Mode::SelfTest);
    EXPECT_TRUE(onscreen->selfTestOnScreen);
}

TEST(CommandLine, CollectsPathsInOrder) {
    const auto parsed = parse({"a.sln", "dir/CMakeLists.txt"});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->paths, (std::vector<std::string>{"a.sln", "dir/CMakeLists.txt"}));
}

TEST(CommandLine, DoubleDashEndsOptions) {
    const auto parsed = parse({"--", "--version", "x"});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->mode, Mode::Gui);
    EXPECT_EQ(parsed->paths, (std::vector<std::string>{"--version", "x"}));
}

TEST(CommandLine, RejectsUnknownOption) {
    const auto parsed = parse({"--verison"});
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code(), frost::ErrorCode::InvalidArgument);
    EXPECT_NE(parsed.error().message().find("--verison"), std::string::npos);
}

TEST(CommandLine, RejectsSingleDashTypos) {
    for (const std::string_view typo : {"-version", "-h", "-"}) {
        const auto parsed = parse({typo});
        ASSERT_FALSE(parsed.has_value()) << typo;
        EXPECT_EQ(parsed.error().code(), frost::ErrorCode::InvalidArgument) << typo;
    }
    // After "--", a leading dash is part of a path.
    const auto parsed = parse({"--", "-odd-name.cpp"});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->paths, (std::vector<std::string>{"-odd-name.cpp"}));
}

TEST(CommandLine, RejectsConflictingModes) {
    EXPECT_FALSE(parse({"--version", "--selftest"}).has_value());
    // Repeating the same mode is harmless.
    EXPECT_TRUE(parse({"--version", "--version"}).has_value());
}

TEST(CommandLine, UsageMentionsEveryOption) {
    const std::string_view usage = frost::app::usageText();
    for (const std::string_view option : {"--version", "--help", "--selftest", "--selftest=onscreen"}) {
        EXPECT_NE(usage.find(option), std::string_view::npos) << option;
    }
}

} // namespace
