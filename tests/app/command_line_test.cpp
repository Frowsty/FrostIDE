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

TEST(CommandLine, FrostEngineProjectAndSourcePosition) {
    auto parsed = parse({"--project", "C:\\Game \xC3\x85", "C:\\Game \xC3\x85\\Assets\\main.lua:42:7"});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->project, "C:\\Game \xC3\x85");
    EXPECT_EQ(parsed->paths[0], "C:\\Game \xC3\x85\\Assets\\main.lua");
    EXPECT_EQ(parsed->line, 42);
    EXPECT_EQ(parsed->column, 7);
    parsed = parse({"--project", "/tmp/project.frostproj", "--line", "12", "--column", "3", "/tmp/my script.lua"});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->line, 12);
    EXPECT_EQ(parsed->column, 3);
    EXPECT_EQ(parsed->paths[0], "/tmp/my script.lua");
    parsed = parse({"--project", "/tmp/game", "/tmp/file.lua:0:0"});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->line, 0);
    EXPECT_EQ(parsed->column, 0);
    EXPECT_EQ(parsed->paths[0], "/tmp/file.lua");
}

TEST(CommandLine, RejectsInvalidProjectLocationOptions) {
    for (const std::vector<std::string_view>& args : {std::vector<std::string_view>{"--project"},
                                                      {"--project", "--line", "file.lua"},
                                                      {"--line", "-1", "file.lua"},
                                                      {"--line", "oops", "file.lua"},
                                                      {"--line", "1"},
                                                      {"--column", "4", "a", "b"},
                                                      {"--project", "a", "--project", "b"},
                                                      {"--help", "--project", "a"},
                                                      {"--line", "2147483648", "a"}}) {
        auto parsed = parse(args);
        EXPECT_FALSE(parsed.has_value());
    }
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

TEST(CommandLine, SelfTestCrashAndFreezeOptions) {
    using frost::app::SelfTestKind;
    const auto crash = parse({"--selftest=crash"});
    ASSERT_TRUE(crash.has_value());
    EXPECT_EQ(crash->mode, Mode::SelfTest);
    EXPECT_EQ(crash->selfTestKind, SelfTestKind::Crash);
    EXPECT_FALSE(crash->selfTestOnScreen);
    const auto freeze = parse({"--selftest=freeze"});
    ASSERT_TRUE(freeze.has_value());
    EXPECT_EQ(freeze->selfTestKind, SelfTestKind::Freeze);
    EXPECT_EQ(parse({"--selftest"})->selfTestKind, SelfTestKind::Checks);
    EXPECT_EQ(parse({"--selftest=onscreen"})->selfTestKind, SelfTestKind::Checks);
    EXPECT_EQ(parse({"--selftest=render"})->selfTestKind, SelfTestKind::Render);
    EXPECT_EQ(parse({"--selftest=render"})->mode, Mode::SelfTest);
    // A deliberate fault is never combined with anything else, not even itself or a path.
    for (const std::vector<std::string_view>& args :
         {std::vector<std::string_view>{"--selftest=crash", "--selftest=crash"},
          std::vector<std::string_view>{"--selftest=freeze", "file.txt"},
          std::vector<std::string_view>{"--selftest", "--selftest=crash"},
          std::vector<std::string_view>{"--", "x", "--selftest=freeze"}}) {
        const auto parsed = parse(args);
        if (args.front() == "--") {
            // After "--" it is a path, not an option.
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(parsed->mode, Mode::Gui);
            continue;
        }
        ASSERT_FALSE(parsed.has_value()) << args.front() << " " << args.back();
        EXPECT_EQ(parsed.error().code(), frost::ErrorCode::InvalidArgument);
    }
    EXPECT_FALSE(parse({"--selftest=crashy"}).has_value());
}

TEST(CommandLine, UsageMentionsEveryOption) {
    const std::string_view usage = frost::app::usageText();
    for (const std::string_view option : {"--version", "--help", "--selftest", "--selftest=onscreen",
                                          "--selftest=crash", "--selftest=freeze", "--selftest=render"}) {
        EXPECT_NE(usage.find(option), std::string_view::npos) << option;
    }
}

} // namespace
