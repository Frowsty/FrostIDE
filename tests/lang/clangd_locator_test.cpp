// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for finding and launching clangd (clangd.hpp): the search order (setting, pinned
// SDK, PATH), its errors, the command line, the -j default and the restart backoff.

#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/clangd.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::io::MemFs;
using frost::lang::ClangdOrigin;
using frost::lang::ClangdSearch;
using frost::lang::findClangd;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;
const std::string kExe = kWindows ? "clangd.exe" : "clangd";
const char kSep = kWindows ? ';' : ':';

std::string dir(std::string_view name) { return (kWindows ? "C:\\" : "/") + std::string(name); }
std::string exeIn(std::string_view name) { return frost::io::joinPath(dir(name), kExe).value(); }

void place(MemFs& fs, std::string_view name) {
    ASSERT_TRUE(fs.createDirectories(dir(name)).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(exeIn(name), "binary").has_value());
}

TEST(FindClangd, PrefersTheSettingThenTheSdkThenPath) {
    MemFs fs(frost::systemClock());
    place(fs, "setting");
    place(fs, "sdk");
    place(fs, "path2");
    ClangdSearch search{.configuredPath = exeIn("setting"),
                        .sdkDirectories = {dir("missing-sdk"), dir("sdk")},
                        .pathVariable = std::string("relative") + kSep + dir("path1") + kSep + dir("path2")};
    auto chosen = findClangd(fs, search);
    ASSERT_TRUE(chosen.has_value()) << chosen.error().describe();
    EXPECT_EQ(chosen->path, exeIn("setting"));
    EXPECT_EQ(chosen->origin, ClangdOrigin::Setting);

    search.configuredPath.clear();
    chosen = findClangd(fs, search);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(chosen->path, exeIn("sdk"));
    EXPECT_EQ(chosen->origin, ClangdOrigin::PinnedSdk);

    search.sdkDirectories = {dir("missing-sdk")};
    chosen = findClangd(fs, search);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(chosen->path, exeIn("path2")); // "relative" and the missing path1 are skipped.
    EXPECT_EQ(chosen->origin, ClangdOrigin::Path);
}

TEST(FindClangd, QuotedPathEntriesAreAccepted) {
    MemFs fs(frost::systemClock());
    place(fs, "quoted dir");
    auto chosen = findClangd(
        fs, ClangdSearch{.configuredPath = {}, .sdkDirectories = {}, .pathVariable = "\"" + dir("quoted dir") + "\""});
    ASSERT_TRUE(chosen.has_value()) << chosen.error().describe();
    EXPECT_EQ(chosen->path, exeIn("quoted dir"));
}

TEST(FindClangd, ReportsWhatIsWrong) {
    MemFs fs(frost::systemClock());
    place(fs, "sdk");
    // A configured path that does not exist has no fallback.
    auto missing = findClangd(
        fs, ClangdSearch{.configuredPath = exeIn("nowhere"), .sdkDirectories = {dir("sdk")}, .pathVariable = {}});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    auto relative =
        findClangd(fs, ClangdSearch{.configuredPath = "bin/clangd", .sdkDirectories = {}, .pathVariable = {}});
    ASSERT_FALSE(relative.has_value());
    EXPECT_EQ(relative.error().code(), ErrorCode::InvalidArgument);
    auto none =
        findClangd(fs, ClangdSearch{.configuredPath = {}, .sdkDirectories = {dir("other")}, .pathVariable = {}});
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().code(), ErrorCode::NotFound);
    EXPECT_NE(none.error().message().find(exeIn("other")), std::string::npos);
}

TEST(FindClangd, DefaultSearchNamesThePinnedVersion) {
    EXPECT_FALSE(frost::lang::pinnedLlvmVersion().empty());
    const ClangdSearch search = frost::lang::defaultClangdSearch("x");
    EXPECT_EQ(search.configuredPath, "x");
    const bool namesPinned = std::ranges::any_of(search.sdkDirectories, [](const std::string& d) {
        return d.find(std::string("llvm-") + std::string(frost::lang::pinnedLlvmVersion())) != std::string::npos;
    });
    EXPECT_TRUE(namesPinned);
}

TEST(ClangdArguments, CarryThePolicyFlags) {
    const frost::lang::CompileCommandsLocation location{
        .directory = dir("db"),
        .file = frost::io::joinPath(dir("db"), "compile_commands.json").value(),
        .source = frost::lang::CompileCommandsSource::Existing};
    const auto arguments = frost::lang::clangdArguments(
        frost::lang::ClangdLaunchOptions{
            .backgroundIndex = true, .jobs = 3, .logLevel = "error", .extraArguments = {"--x"}},
        location);
    EXPECT_EQ(arguments,
              (std::vector<std::string>{"--background-index", "--header-insertion=never", "--log=error",
                                        "--pch-storage=memory", "-j=3", "--compile-commands-dir=" + dir("db"), "--x"}));
    const auto plain = frost::lang::clangdArguments(frost::lang::ClangdLaunchOptions{}, std::nullopt);
    EXPECT_EQ(
        std::ranges::count_if(plain, [](const std::string& a) { return a.starts_with("--compile-commands-dir"); }), 0);
    EXPECT_EQ(std::ranges::count_if(plain, [](const std::string& a) { return a.starts_with("-j="); }), 1);
}

TEST(ClangdArguments, JobsFollowTheCoreCount) {
    EXPECT_EQ(frost::lang::defaultClangdJobs(0), 1U);
    EXPECT_EQ(frost::lang::defaultClangdJobs(1), 1U);
    EXPECT_EQ(frost::lang::defaultClangdJobs(8), 4U);
    EXPECT_EQ(frost::lang::defaultClangdJobs(24), 8U);
    EXPECT_GE(frost::lang::defaultClangdJobs(), 1U);
}

TEST(RestartPolicy, BacksOffExponentiallyUpToTheCap) {
    const frost::lang::RestartPolicy policy{
        .initialDelay = 100ms, .maxDelay = 1s, .maxConsecutiveCrashes = 5, .stableAfter = 60s};
    EXPECT_EQ(frost::lang::restartDelay(policy, 1), 100ms);
    EXPECT_EQ(frost::lang::restartDelay(policy, 2), 200ms);
    EXPECT_EQ(frost::lang::restartDelay(policy, 4), 800ms);
    EXPECT_EQ(frost::lang::restartDelay(policy, 5), 1s);
    EXPECT_EQ(frost::lang::restartDelay(policy, 500), 1s);
}

} // namespace
