// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for finding git (git_locator.hpp: the setting, then the install directories, then
// PATH) and for the command line and environment GitRunner gives git (git_runner.hpp), without
// running git.

#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/vcs/git_locator.hpp"
#include "frost/vcs/git_runner.hpp"
#include "frost/vcs/git_service.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::io::MemFs;
using namespace frost::vcs;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;
const std::string& executableName() {
    static const std::string value(kWindows ? "git.exe" : "git");
    return value;
}
const char kSep = kWindows ? ';' : ':';

std::string dir(std::string_view name) { return (kWindows ? "C:\\" : "/") + std::string(name); }
std::string exeIn(std::string_view name) { return frost::io::joinPath(dir(name), executableName()).value(); }

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
                     .pathVariable = std::string("relative") + kSep + dir("path1") + kSep + dir("path2"),
                     .excludedDirectories = {}};
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

TEST(FindGit, RejectsProjectPathCandidatesIncludingExplicitSetting) {
    MemFs fs(frost::systemClock());
    place(fs, "project");
    place(fs, "safe");
    GitSearch search;
    search.pathVariable = dir("project") + kSep + dir("safe");
    search.excludedDirectories = {dir("project")};
    auto found = findGit(fs, search);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->path, exeIn("safe"));
    search.configuredPath = exeIn("project");
    found = findGit(fs, search);
    ASSERT_FALSE(found.has_value());
    EXPECT_EQ(found.error().code(), ErrorCode::PermissionDenied);
}

TEST(GitRunnerSpec, PreCancelledInvocationDoesNotLaunch) {
    frost::io::ScriptedProcessLauncher launcher;
    GitRunner runner(launcher, exeIn("git"));
    std::stop_source stop;
    stop.request_stop();
    GitInvocation invocation;
    invocation.arguments = {"status"};
    invocation.stop = stop.get_token();
    auto result = runner.run(invocation);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_TRUE(launcher.launches().empty());
}

TEST(GitRunnerSpec, RevokedTrustRejectsMutationBeforeLaunch) {
    bool trusted = true;
    GitRunnerOptions options;
    options.stillTrusted = [&trusted] { return trusted; };
    GitRunner runner(frost::io::realProcessLauncher(), exeIn("git"), options);
    GitInvocation invocation;
    invocation.requiresTrust = true;
    invocation.workingDirectory = dir("repo");
    trusted = false;
    auto result = runner.run(invocation);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Untrusted);
}

TEST(FindGit, ExplainsWhatWasTried) {
    MemFs fs(frost::systemClock());
    GitSearch search{
        .configuredPath = "relative/git", .installDirectories = {}, .pathVariable = "", .excludedDirectories = {}};
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

TEST(GitServiceLifetime, CancelsRunningHeadReadBeforeJoining) {
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect(frost::io::ScriptedProcess{}); // Initial status.
    launcher.expect(frost::io::ScriptedProcess{.neverExits = true, .outputHeld = true});
    MemFs fs(frost::systemClock());
    auto service =
        GitService::create(GitRunner(launcher, exeIn("git")), fs, RepositoryInfo{dir("repo"), dir("repo/.git"), {}});
    ASSERT_TRUE(service->waitForIdle(std::chrono::seconds(3)));
    service->readHeadContent(frost::io::joinPath(dir("repo"), "file.txt").value(), [](auto) {});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (launcher.launches().size() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_GE(launcher.launches().size(), 2U);
    const auto start = std::chrono::steady_clock::now();
    service.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
    EXPECT_TRUE(launcher.launches().at(1).terminated);
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
    EXPECT_EQ(environmentValue(spec, "GIT_DIR"), std::make_optional(std::optional<std::string>()));
    EXPECT_EQ(environmentValue(spec, "GIT_WORK_TREE"), std::make_optional(std::optional<std::string>()));
    EXPECT_EQ(environmentValue(spec, "GIT_INDEX_FILE"), std::make_optional(std::optional<std::string>()));
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
