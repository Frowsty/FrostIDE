// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for finding and launching clangd (clangd.hpp): the search order (setting, pinned
// SDK, PATH), its errors, the command line, the -j default and the restart backoff.

#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/clangd.hpp"

#include "support/test_env.hpp"
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

// FND-10: the bundled clangd beside the executable comes first, then FROST_SDK and FROST_LLVM_ROOT
// when they are set; there is no machine-specific default root (the bootstrap roots) any more.
TEST(FindClangd, DefaultSearchPutsTheBundledCopyFirstAndGuessesNoSdkRoot) {
    const ClangdSearch search = frost::lang::defaultClangdSearch();
    const std::string sdk = frost::test::environment("FROST_SDK");
    const std::string llvmRoot = frost::test::environment("FROST_LLVM_ROOT");
    // <exe>/llvm-<pinned>/bin, <exe>/llvm/bin, <exe>/bin, <exe>; then the environment's roots.
    ASSERT_EQ(search.sdkDirectories.size(), 4U + (sdk.empty() ? 0U : 1U) + (llvmRoot.empty() ? 0U : 1U));
    const std::string exeDir = search.sdkDirectories[3];
    EXPECT_TRUE(frost::io::isAbsolutePath(exeDir)) << exeDir;
    const auto pinnedBin = [](const std::string& root) {
        return frost::io::joinPath(root, std::string("llvm-") + std::string(frost::lang::pinnedLlvmVersion()))
            .and_then([](const std::string& llvm) { return frost::io::joinPath(llvm, "bin"); })
            .value();
    };
    EXPECT_EQ(search.sdkDirectories[0], pinnedBin(exeDir));
    std::size_t next = 4;
    if (!sdk.empty()) {
        EXPECT_EQ(search.sdkDirectories[next++], pinnedBin(sdk));
    }
    if (!llvmRoot.empty()) {
        EXPECT_EQ(search.sdkDirectories[next], frost::io::joinPath(llvmRoot, "bin").value());
    }
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
    // Untrusted (the default): restricted mode.
    EXPECT_EQ(arguments,
              (std::vector<std::string>{"--background-index", "--header-insertion=never", "--log=error",
                                        "--pch-storage=memory", "-j=3", "--rename-file-limit=0",
                                        "--enable-config=false", "--compile-commands-dir=" + dir("db"), "--x"}));
    const auto plain = frost::lang::clangdArguments(frost::lang::ClangdLaunchOptions{}, std::nullopt);
    EXPECT_EQ(
        std::ranges::count_if(plain, [](const std::string& a) { return a.starts_with("--compile-commands-dir"); }), 0);
    EXPECT_EQ(std::ranges::count_if(plain, [](const std::string& a) { return a.starts_with("-j="); }), 1);
}

// clangd.hpp "Trust": a trusted workspace raises the result limits; the restricted mode disables
// project configuration and drops anything that would name a driver or re-enable the config.
TEST(ClangdArguments, TrustSelectsLimitsOrRestrictedMode) {
    const auto has = [](const std::vector<std::string>& args, std::string_view flag) {
        return std::ranges::find(args, flag) != args.end();
    };
    const auto startsWith = [](const std::vector<std::string>& args, std::string_view prefix) {
        return std::ranges::count_if(args, [&](const std::string& a) { return a.starts_with(prefix); });
    };
    const std::vector<std::string> extras{"--query-driver=C:/x/**/g++*", "--enable-config", "-query-driver=/usr/**",
                                          "--clang-tidy"};
    const auto trusted = frost::lang::clangdArguments(
        frost::lang::ClangdLaunchOptions{.trusted = true, .extraArguments = extras}, std::nullopt);
    EXPECT_TRUE(has(trusted, "--rename-file-limit=0"));
    EXPECT_TRUE(has(trusted, "--limit-references=0"));
    EXPECT_TRUE(has(trusted, "--limit-results=1000"));
    EXPECT_FALSE(has(trusted, "--enable-config=false"));
    EXPECT_EQ(startsWith(trusted, "--query-driver"), 1); // A trusted caller's own extras pass through.
    EXPECT_TRUE(has(trusted, "--clang-tidy"));

    const auto restricted = frost::lang::clangdArguments(
        frost::lang::ClangdLaunchOptions{.trusted = false, .extraArguments = extras}, std::nullopt);
    EXPECT_TRUE(has(restricted, "--rename-file-limit=0"));
    EXPECT_TRUE(has(restricted, "--enable-config=false"));
    EXPECT_EQ(startsWith(restricted, "--limit-"), 0);
    EXPECT_EQ(startsWith(restricted, "--query-driver"), 0);
    EXPECT_EQ(startsWith(restricted, "-query-driver"), 0);
    EXPECT_FALSE(has(restricted, "--enable-config"));
    EXPECT_TRUE(has(restricted, "--clang-tidy"));
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
