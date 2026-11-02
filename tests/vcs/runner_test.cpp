// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for finding git (git_locator.hpp: the setting, then the install directories, then
// PATH) and for the command line and environment GitRunner gives git (git_runner.hpp), without
// running git.

#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/vcs/git_locator.hpp"
#include "frost/vcs/git_runner.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::io::MemFs;
using namespace frost::vcs;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;
const std::string kExe = kWindows ? "git.exe" : "git";
const char kSep = kWindows ? ';' : ':';

std::string dir(std::string_view name) { return (kWindows ? "C:\\" : "/") + std::string(name); }
std::string exeIn(std::string_view name) { return frost::io::joinPath(dir(name), kExe).value(); }

void place(MemFs& fs, std::string_view name) {
    ASSERT_TRUE(fs.createDirectories(dir(name)).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(exeIn(name), "binary").has_value());
}

TEST(FindGit, PrefersTheSettingThenTheInstallDirectoriesThenPath) {
    MemFs fs(frost::systemClock());
    place(fs, "setting");
    place(fs, "install");
    place(fs, "path2");
    GitSearch search{.configuredPath = exeIn("setting"),
                     .installDirectories = {dir("missing"), dir("install")},
                     .pathVariable = std::string("relative") + kSep + dir("path1") + kSep + dir("path2")};
    auto found = findGit(fs, search);
    ASSERT_TRUE(found.has_value()) << found.error().describe();
    EXPECT_EQ(found->path, exeIn("setting"));
    EXPECT_EQ(found->origin, GitOrigin::Setting);

    search.configuredPath.clear();
    found = findGit(fs, search);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->path, exeIn("install"));
    EXPECT_EQ(found->origin, GitOrigin::InstallDirectory);

    search.installDirectories = {dir("missing")};
    found = findGit(fs, search);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->path, exeIn("path2"));
    EXPECT_EQ(found->origin, GitOrigin::Path);
    EXPECT_EQ(toString(found->origin), "PATH");
}

TEST(FindGit, ExplainsWhatWasTried) {
    MemFs fs(frost::systemClock());
    GitSearch search{.configuredPath = "relative/git", .installDirectories = {}, .pathVariable = ""};
    EXPECT_EQ(findGit(fs, search).error().code(), ErrorCode::InvalidArgument);
    search.configuredPath = exeIn("nowhere");
    EXPECT_EQ(findGit(fs, search).error().code(), ErrorCode::NotFound);
    search.configuredPath.clear();
    search.installDirectories = {dir("missing")};
    search.pathVariable = dir("empty");
    auto missing = findGit(fs, search);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    EXPECT_NE(missing.error().message().find(dir("missing")), std::string::npos) << missing.error().message();
}

std::optional<std::optional<std::string>> environmentValue(const frost::io::ProcessSpec& spec, std::string_view name) {
    std::optional<std::optional<std::string>> value;
    for (const auto& [key, setting] : spec.environment) {
        if (key == name) {
            value = setting; // The last change wins.
        }
    }
    return value;
}

TEST(GitRunnerSpec, EnglishOutputNoPromptsAndOptionalLocksForBackgroundWork) {
    GitRunner runner(frost::io::realProcessLauncher(), exeIn("git"));
    GitInvocation invocation;
    invocation.arguments = {"status"};
    invocation.workingDirectory = dir("repo");
    frost::io::ProcessSpec spec = runner.processSpec(invocation);
    EXPECT_EQ(spec.executable, exeIn("git"));
    EXPECT_EQ(spec.workingDirectory, dir("repo"));
    EXPECT_TRUE(spec.killOnClose);
    const std::vector<std::string> prefix = {"--no-pager",     "-c",    "core.quotepath=false", "-c",
                                             "color.ui=false", "status"};
    EXPECT_EQ(spec.arguments, prefix);
    EXPECT_EQ(environmentValue(spec, "LC_ALL"), std::optional<std::optional<std::string>>("C"));
    EXPECT_EQ(environmentValue(spec, "GIT_TERMINAL_PROMPT"), std::optional<std::optional<std::string>>("0"));
    EXPECT_EQ(environmentValue(spec, "GIT_EDITOR"), std::optional<std::optional<std::string>>(":"));
    EXPECT_EQ(environmentValue(spec, "LANGUAGE"), std::make_optional(std::optional<std::string>())); // Removed.
    EXPECT_FALSE(environmentValue(spec, "GIT_OPTIONAL_LOCKS").has_value());
    EXPECT_EQ(spec.stdinMode, frost::io::Stdio::Null);

    invocation.background = true;
    invocation.stdinData = "input";
    invocation.environment = {{"LC_ALL", "C.UTF-8"}};
    spec = runner.processSpec(invocation);
    EXPECT_EQ(environmentValue(spec, "GIT_OPTIONAL_LOCKS"), std::optional<std::optional<std::string>>("0"));
    EXPECT_EQ(environmentValue(spec, "LC_ALL"), std::optional<std::optional<std::string>>("C.UTF-8"));
    EXPECT_EQ(spec.stdinMode, frost::io::Stdio::Pipe);
}

TEST(GitRunnerSpec, RestrictedModeTurnsHooksAndFsmonitorOff) {
    GitRunnerOptions options;
    options.trusted = false;
    options.noHooksDirectory = dir("no-hooks");
    GitRunner runner(frost::io::realProcessLauncher(), exeIn("git"), options);
    EXPECT_FALSE(runner.trusted());
    GitInvocation invocation;
    invocation.arguments = {"status"};
    invocation.workingDirectory = dir("repo");
    const frost::io::ProcessSpec spec = runner.processSpec(invocation);
    EXPECT_NE(std::ranges::find(spec.arguments, "core.fsmonitor=false"), spec.arguments.end());
    EXPECT_NE(std::ranges::find(spec.arguments, "core.hooksPath=" + dir("no-hooks")), spec.arguments.end());
    EXPECT_EQ(spec.arguments.back(), "status");

    GitRunnerOptions defaults;
    defaults.trusted = false;
    const GitRunner fallback(frost::io::realProcessLauncher(), exeIn("git"), defaults);
    const frost::io::ProcessSpec other = fallback.processSpec(invocation);
    const bool hasHooksPath = std::ranges::any_of(other.arguments, [](const std::string& argument) {
        return argument.starts_with("core.hooksPath=") && argument.size() > std::string_view("core.hooksPath=").size();
    });
    EXPECT_TRUE(hasHooksPath);
}

} // namespace
