// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the CMake toolchain (cmake_toolchain.hpp) over a MemFs and the scripted launcher:
// cmake and ninja discovery (settings, Visual Studio's bundled tools, PATH), PATH splitting, the
// version probe, the vcvarsall command line and its refusals, the capture's difference, UTF-16LE
// decoding, the cache file format and MsvcEnvironmentCache's memory, file and version rules.

#include "frost/build/cmake_toolchain.hpp"
#include "frost/io/scripted_process_launcher.hpp"

#include "build_test_util.hpp"
#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace frost::build {
namespace {

using test::kWindows;
using test::MemTree;
using test::native;

std::string exe(std::string_view name) { return kWindows ? std::string(name) + ".exe" : std::string(name); }

std::string pathList(std::initializer_list<std::string> entries) {
    std::string out;
    for (const std::string& entry : entries) {
        if (!out.empty()) {
            out += kWindows ? ';' : ':';
        }
        out += entry;
    }
    return out;
}

TEST(CMakeTools, SettingsThenVisualStudioThenPath) {
    MemTree tree;
    tree.write("bin/" + exe("cmake"), "x");
    tree.write("bin/" + exe("ninja"), "x");
    tree.write("custom/" + exe("cmake"), "x");
    const std::string path = pathList({"relative", MemTree::path("bin"), MemTree::path("bin")});

    auto fromPath =
        findCMakeTools(tree.fs, {.cmakePath = {}, .ninjaPath = {}, .vsInstances = {}, .pathVariable = path});
    ASSERT_TRUE(fromPath.has_value()) << fromPath.error().describe();
    EXPECT_EQ(fromPath->cmake, MemTree::path("bin/" + exe("cmake")));
    EXPECT_EQ(fromPath->cmakeOrigin, ToolOrigin::Path);
    EXPECT_EQ(fromPath->ninja, MemTree::path("bin/" + exe("ninja")));

    auto configured = findCMakeTools(tree.fs, {.cmakePath = MemTree::path("custom/" + exe("cmake")),
                                               .ninjaPath = {},
                                               .vsInstances = {},
                                               .pathVariable = path});
    ASSERT_TRUE(configured.has_value());
    EXPECT_EQ(configured->cmake, MemTree::path("custom/" + exe("cmake")));
    EXPECT_EQ(configured->cmakeOrigin, ToolOrigin::Settings);
    EXPECT_EQ(configured->ninjaOrigin, ToolOrigin::Path);

    // A settings path that is wrong is reported, not skipped.
    EXPECT_EQ(
        findCMakeTools(tree.fs,
                       {.cmakePath = MemTree::path("nope"), .ninjaPath = {}, .vsInstances = {}, .pathVariable = path})
            .error()
            .code(),
        ErrorCode::NotFound);
    EXPECT_EQ(findCMakeTools(tree.fs, {.cmakePath = "cmake", .ninjaPath = {}, .vsInstances = {}, .pathVariable = path})
                  .error()
                  .code(),
              ErrorCode::InvalidArgument);
    // Nothing anywhere; the relative PATH entry is never searched.
    tree.write("relative/" + exe("cmake"), "x");
    EXPECT_EQ(findCMakeTools(tree.fs, {.cmakePath = {}, .ninjaPath = {}, .vsInstances = {}, .pathVariable = "relative"})
                  .error()
                  .code(),
              ErrorCode::NotFound);

    if constexpr (kWindows) {
        VsInstance instance;
        instance.installationPath = MemTree::path("VS");
        tree.write("VS/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe", "x");
        tree.write("VS/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe", "x");
        auto bundled = findCMakeTools(
            tree.fs, {.cmakePath = {}, .ninjaPath = {}, .vsInstances = {instance}, .pathVariable = path});
        ASSERT_TRUE(bundled.has_value());
        EXPECT_EQ(bundled->cmakeOrigin, ToolOrigin::VisualStudio);
        EXPECT_EQ(bundled->cmake, vsBundledCMakePath(instance.installationPath).value());
        EXPECT_EQ(bundled->ninjaOrigin, ToolOrigin::VisualStudio);
    }
}

TEST(CMakeTools, SplitsPathAndComparesVersions) {
    const std::string a = MemTree::path("a");
    const std::string b = MemTree::path("b");
    EXPECT_EQ(splitPathVariable(pathList({a, "", "rel", b, a})), (std::vector<std::string>{a, b}));
    if constexpr (kWindows) {
        EXPECT_EQ(splitPathVariable("\"C:\\Program Files\\CMake\\bin\";c:\\program files\\cmake\\bin"),
                  std::vector<std::string>{"C:\\Program Files\\CMake\\bin"});
    }
    EXPECT_TRUE(versionAtLeast("4.4.5", 3, 21));
    EXPECT_TRUE(versionAtLeast("3.21.0-rc1", 3, 21));
    EXPECT_FALSE(versionAtLeast("3.20.6", 3, 21));
    EXPECT_FALSE(versionAtLeast("garbage", 3, 21));
}

TEST(CMakeTools, AsksCMakeForItsVersion) {
    io::ScriptedProcessLauncher launcher;
    const std::string cmake = MemTree::path("bin/" + exe("cmake"));
    launcher.expect({.expectExecutable = cmake, .stdoutBytes = "cmake version 4.4.5\n\nCMake suite maintained...\n"});
    EXPECT_EQ(cmakeVersion(launcher, cmake).value(), "4.4.5");
    launcher.expect({.expectExecutable = cmake, .stdoutBytes = "hello\n"});
    EXPECT_EQ(cmakeVersion(launcher, cmake).error().code(), ErrorCode::Corrupt);
    launcher.expect({.expectExecutable = cmake, .exitCode = 1});
    EXPECT_EQ(cmakeVersion(launcher, cmake).error().code(), ErrorCode::IoError);
    ASSERT_EQ(launcher.launches().size(), 3U);
    EXPECT_EQ(launcher.launches()[0].spec.arguments, std::vector<std::string>{"--version"});
}

TEST(MsvcEnvironment, BuildsASafeCommandLine) {
    const std::string cmd = "C:\\Windows\\System32\\cmd.exe";
    const std::string vcvars =
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat";
    auto spec = vcvarsProcessSpec(cmd, vcvars, "X64");
    ASSERT_TRUE(spec.has_value()) << spec.error().describe();
    EXPECT_EQ(spec->executable, cmd);
    EXPECT_EQ(spec->arguments,
              (std::vector<std::string>{"/d", "/u", "/c", "set", "&&", "echo", std::string(kVcvarsMarker), "&&", "call",
                                        vcvars, "x64", ">nul", "&&", "set"}));
    EXPECT_EQ(spec->workingDirectory,
              "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build");
    EXPECT_EQ(vcvarsProcessSpec(cmd, "C:\\100%\\vcvarsall.bat", "x64").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(vcvarsProcessSpec(cmd, "C:\\a\"b\\vcvarsall.bat", "x64").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(vcvarsProcessSpec(cmd, vcvars, "x64 & calc").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(vcvarsProcessSpec(cmd, "vcvarsall.bat", "x64").error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(isVcvarsArchitecture("x64_arm64"));
    EXPECT_FALSE(isVcvarsArchitecture("mips"));
    EXPECT_EQ(vcvarsallPath("C:\\VS").value(), "C:\\VS\\VC\\Auxiliary\\Build\\vcvarsall.bat");
}

TEST(MsvcEnvironment, TakesTheDifferenceOfTheTwoListings) {
    const std::string output = "=C:=C:\\work\r\nPath=C:\\Windows\r\nKEEP=same\r\nGONE=1\r\n" +
                               std::string(kVcvarsMarker) +
                               "\r\nPath=C:\\VC\\bin;C:\\Windows\r\nKEEP=same\r\nINCLUDE=C:\\VC\\include\r\n";
    auto changes = parseVcvarsCapture(output);
    ASSERT_TRUE(changes.has_value()) << changes.error().describe();
    std::map<std::string, std::optional<std::string>> byName(changes->begin(), changes->end());
    EXPECT_EQ(byName.size(), 3U);
    EXPECT_EQ(byName["Path"], std::optional<std::string>("C:\\VC\\bin;C:\\Windows"));
    EXPECT_EQ(byName["INCLUDE"], std::optional<std::string>("C:\\VC\\include"));
    ASSERT_TRUE(byName.contains("GONE"));
    EXPECT_EQ(byName["GONE"], std::nullopt);
    EXPECT_EQ(parseVcvarsCapture("A=1\r\n").error().code(), ErrorCode::Corrupt);
    EXPECT_EQ(parseVcvarsCapture("A=1\r\n" + std::string(kVcvarsMarker) + "\r\n").error().code(), ErrorCode::Corrupt);
}

// `text` (ASCII) as UTF-16LE bytes.
std::string utf16(std::string_view text) {
    std::string out;
    for (const char c : text) {
        out += c;
        out += '\0';
    }
    return out;
}

TEST(MsvcEnvironment, DecodesUtf16) {
    EXPECT_EQ(decodeUtf16Le(utf16("A=1\r\n")), "A=1\r\n");
    EXPECT_EQ(decodeUtf16Le(std::string("\xE9\x00", 2)), "\xC3\xA9");                 // U+00E9.
    EXPECT_EQ(decodeUtf16Le(std::string("\x3D\xD8\x00\xDE", 4)), "\xF0\x9F\x98\x80"); // U+1F600.
    EXPECT_EQ(decodeUtf16Le(std::string("\x00\xD8", 2)), "\xEF\xBF\xBD");             // A lone surrogate.
    EXPECT_EQ(decodeUtf16Le(std::string("A\x00Z", 3)), "A");                          // An odd byte is dropped.
}

TEST(MsvcEnvironment, CacheFileRoundTripsAndFollowsTheVersion) {
    const MsvcDevEnvironment environment{
        .instanceId = "abc123",
        .installationVersion = "17.14.1",
        .architecture = "x64",
        .changes = {{"INCLUDE", std::string("C:\\VC\\include")}, {"GONE", std::nullopt}}};
    const std::string text = formatMsvcEnvironmentFile(environment);
    auto parsed = parseMsvcEnvironmentFile(text);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->installationVersion, "17.14.1");
    EXPECT_EQ(parsed->changes, environment.changes);
    EXPECT_FALSE(parseMsvcEnvironmentFile("# something else\n").has_value());
    EXPECT_FALSE(parseMsvcEnvironmentFile("# FrostIDE MSVC environment v1\nset\tnoequals\n").has_value());

    MemTree tree;
    io::ScriptedProcessLauncher launcher;
    MsvcEnvironmentCache cache(tree.fs, launcher, MemTree::path("cache"));
    const std::string file = cache.cacheFile("abc123", "X64");
    EXPECT_EQ(file, MemTree::path("cache/msvc-env-abc123-x64.txt"));
    tree.write("cache/msvc-env-abc123-x64.txt", text);
    VsInstance instance;
    instance.instanceId = "abc123";
    instance.installationVersion = "17.14.1";
    instance.installationPath = MemTree::path("VS");
    auto cached = cache.get(instance, "x64");
    ASSERT_TRUE(cached.has_value()) << cached.error().describe();
    EXPECT_EQ(cached->changes, environment.changes);
    EXPECT_EQ(cache.captures(), 0U); // From the file; nothing ran.
    EXPECT_TRUE(launcher.launches().empty());

    // Visual Studio was updated: the entry is stale and the environment is captured again.
    instance.installationVersion = "17.14.2";
    if constexpr (kWindows) {
        tree.write("VS/VC/Auxiliary/Build/vcvarsall.bat", "@echo off");
        launcher.expect(
            {.stdoutBytes = utf16("A=1\r\n" + std::string(kVcvarsMarker) + "\r\nA=1\r\nINCLUDE=C:\\new\r\n")});
        auto fresh = cache.get(instance, "x64");
        ASSERT_TRUE(fresh.has_value()) << fresh.error().describe();
        EXPECT_EQ(fresh->changes, (project::CMakeEnvironment{{"INCLUDE", std::string("C:\\new")}}));
        EXPECT_EQ(cache.captures(), 1U);
        ASSERT_EQ(launcher.launches().size(), 1U);
        EXPECT_TRUE(launcher.launches()[0].spec.executable.ends_with("cmd.exe"));
        EXPECT_EQ(parseMsvcEnvironmentFile(tree.fs.readFile(file).value())->installationVersion, "17.14.2");
        EXPECT_TRUE(cache.get(instance, "x64").has_value()); // From memory now.
        EXPECT_EQ(cache.captures(), 1U);
    } else {
        EXPECT_EQ(cache.get(instance, "x64").error().code(), ErrorCode::Unsupported);
    }
}

} // namespace
} // namespace frost::build
