// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for CMake profiles (cmake_profile.hpp): the default Debug profile, profiles from
// settings records (defaults, cache variables, environment, the root filter and the trust rule for
// build directories), profiles from the fixture presets, and the configure and build command lines.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/cmake_presets.hpp"
#include "frost/project/cmake_profile.hpp"

#include "project_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace frost::project {
namespace {

using test::MemTree;
using test::native;

std::string under(std::string_view relative) { return MemTree::path(relative); }

TEST(CMakeProfiles, DefaultIsDebugNinjaInCMakeBuildDebug) {
    const CMakeProfile profile = defaultCMakeProfile(MemTree::root());
    EXPECT_EQ(profile.name, "Debug");
    EXPECT_EQ(profile.source, CMakeProfileSource::Default);
    EXPECT_EQ(profile.buildType, "Debug");
    EXPECT_EQ(profile.generator, "Ninja");
    EXPECT_EQ(profile.buildDirectory, under("cmake-build-debug"));
    EXPECT_TRUE(profile.usable());
    EXPECT_EQ(profile.effectiveConfiguration(), "Debug");

    // Without presets or records the folder has exactly the default profile.
    const auto profiles = cmakeProfilesFor(MemTree::root(), std::nullopt, {}, CMakeProfileSource::ApplicationSettings);
    ASSERT_EQ(profiles.size(), 1U);
    EXPECT_EQ(profiles.front(), profile);
}

TEST(CMakeProfiles, FromSettingsRecords) {
    const std::string other = io::kNativePathStyle == io::PathStyle::Windows ? "D:\\builds\\tiny" : "/builds/tiny";
    const std::vector<CMakeProfileRecord> records = {
        CMakeProfileRecord{.name = "Release",
                           .buildType = "Release",
                           .generator = {},
                           .buildDirectory = {},
                           .cacheVariables = {"TINY_FEATURE:BOOL=ON", "PLAIN=1", "CMAKE_BUILD_TYPE=RelWithDebInfo"},
                           .environment = {"CC=clang", "UNSET_ME"},
                           .buildOptions = {"-k", "0"},
                           .jobs = 3,
                           .root = {}},
        CMakeProfileRecord{.name = "Elsewhere", .buildDirectory = other, .root = MemTree::root()},
        CMakeProfileRecord{.name = "OtherFolder",
                           .root =
                               native(io::kNativePathStyle == io::PathStyle::Windows ? "C:/elsewhere" : "/elsewhere")},
        CMakeProfileRecord{.name = "Release"}, // A duplicate name is skipped.
        CMakeProfileRecord{.name = ""},
    };
    const auto profiles = cmakeProfilesFromRecords(MemTree::root(), records, CMakeProfileSource::ApplicationSettings);
    ASSERT_EQ(profiles.size(), 2U);
    const CMakeProfile& release = profiles[0];
    EXPECT_EQ(release.source, CMakeProfileSource::ApplicationSettings);
    EXPECT_EQ(release.generator, "Ninja");
    EXPECT_EQ(release.buildType, "RelWithDebInfo"); // The cache variable wins.
    EXPECT_EQ(release.buildDirectory, under("cmake-build-release"));
    ASSERT_EQ(release.cacheVariables.size(), 2U);
    EXPECT_EQ(release.cacheVariables[0], (CMakeCacheVariable{.name = "TINY_FEATURE", .type = "BOOL", .value = "ON"}));
    EXPECT_EQ(release.cacheVariables[1], (CMakeCacheVariable{.name = "PLAIN", .type = "", .value = "1"}));
    ASSERT_EQ(release.environment.size(), 2U);
    EXPECT_EQ(release.environment[0].second, std::optional<std::string>("clang"));
    EXPECT_EQ(release.environment[1].second, std::nullopt);
    EXPECT_EQ(release.buildJobs, 3);
    EXPECT_TRUE(release.usable());

    // An application-scope record for this folder may build outside it...
    EXPECT_EQ(profiles[1].buildDirectory, other);
    EXPECT_TRUE(profiles[1].usable());
    // ...a project-scope one may not (the trust rule).
    const auto project = cmakeProfilesFromRecords(MemTree::root(), records, CMakeProfileSource::ProjectSettings);
    ASSERT_EQ(project.size(), 2U);
    EXPECT_TRUE(project[0].usable());
    EXPECT_FALSE(project[1].usable());
    EXPECT_FALSE(cmakeConfigureArguments(project[1], MemTree::root(), {}).has_value());

    // Records win over the default when there are no presets.
    const auto chosen =
        cmakeProfilesFor(MemTree::root(), std::nullopt, records, CMakeProfileSource::ApplicationSettings);
    EXPECT_EQ(chosen.size(), 2U);
}

TEST(CMakeProfiles, FromTheFixturePresets) {
    const std::string root = test::fixture("cmake/presets");
    CMakePresetsContext context;
    context.hostSystemName = "Linux";
    context.environment = [](std::string_view) -> std::optional<std::string> { return std::nullopt; };
    auto presets = loadCMakePresets(io::realFileSystem(), root, context);
    ASSERT_TRUE(presets.has_value() && presets->has_value());
    const std::vector<CMakeProfileRecord> records = {CMakeProfileRecord{.name = "Ignored"}};
    const auto profiles = cmakeProfilesFor(root, *presets, records, CMakeProfileSource::ApplicationSettings);
    std::vector<std::string> names;
    for (const CMakeProfile& profile : profiles) {
        names.push_back(profile.name);
    }
    // Visible, enabled presets in file order (not windows-only on this Linux host), the user's last.
    EXPECT_EQ(names, (std::vector<std::string>{"debug", "release", "any", "match", "mine"}));
    const CMakeProfile& debug = profiles[0];
    EXPECT_EQ(debug.source, CMakeProfileSource::Preset);
    EXPECT_EQ(debug.displayName, "Debug (Ninja)");
    EXPECT_EQ(debug.buildType, "Debug");
    EXPECT_EQ(debug.buildDirectory, io::joinPath(root, native("build/debug")).value());
    EXPECT_EQ(debug.buildPreset, "debug-build");
    EXPECT_EQ(debug.buildTargets, std::vector<std::string>{"app"});
    EXPECT_EQ(debug.buildJobs, 4);
    EXPECT_TRUE(debug.usable());
    EXPECT_TRUE(std::ranges::none_of(debug.cacheVariables,
                                     [](const CMakeCacheVariable& v) { return v.name == "CMAKE_BUILD_TYPE"; }));
    EXPECT_EQ(profiles[1].buildConfiguration, "Release");
    ASSERT_TRUE(profiles[1].buildEnvironment.has_value());

    // A preset whose binaryDir leaves the folder is unusable.
    CMakeConfigurePreset outside;
    outside.name = "outside";
    outside.binaryDir = io::kNativePathStyle == io::PathStyle::Windows ? "C:\\tmp\\out" : "/tmp/out";
    EXPECT_FALSE(cmakeProfileFromPreset(root, outside, nullptr).usable());
    outside.binaryDir = io::joinPath(root, native("../escape")).value(); // As "${sourceDir}/../escape".
    EXPECT_FALSE(cmakeProfileFromPreset(root, outside, nullptr).usable());
    // Without a binaryDir it builds in cmake-build-<name>.
    outside.binaryDir.clear();
    EXPECT_EQ(cmakeProfileFromPreset(root, outside, nullptr).buildDirectory,
              io::joinPath(root, "cmake-build-outside").value());
}

TEST(CMakeProfiles, CommandLines) {
    CMakeProfile profile = defaultCMakeProfile(MemTree::root());
    profile.cacheVariables = {CMakeCacheVariable{.name = "TINY_FEATURE", .type = "BOOL", .value = "ON"},
                              CMakeCacheVariable{.name = "CMAKE_EXPORT_COMPILE_COMMANDS", .type = "", .value = "OFF"}};
    const std::string ninja = io::kNativePathStyle == io::PathStyle::Windows ? "C:\\sdk\\ninja.exe" : "/sdk/ninja";
    auto configure = cmakeConfigureArguments(profile, MemTree::root(), ninja);
    ASSERT_TRUE(configure.has_value()) << configure.error().describe();
    const std::vector<std::string> expected = {"-S",
                                               MemTree::root(),
                                               "-B",
                                               under("cmake-build-debug"),
                                               "-G",
                                               "Ninja",
                                               "-DCMAKE_BUILD_TYPE=Debug",
                                               "-DCMAKE_MAKE_PROGRAM:FILEPATH=" + ninja,
                                               "-DTINY_FEATURE:BOOL=ON",
                                               "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"};
    EXPECT_EQ(*configure, expected);
    EXPECT_FALSE(cmakeConfigureArguments(profile, "relative", ninja).has_value());

    EXPECT_EQ(cmakeBuildArguments(profile, {}, false),
              (std::vector<std::string>{"--build", under("cmake-build-debug")}));
    EXPECT_EQ(cmakeBuildArguments(profile, {}, true, 6),
              (std::vector<std::string>{"--build", under("cmake-build-debug"), "--target", "clean", "-j", "6"}));
    const std::vector<std::string> targets = {"app"};
    profile.buildToolOptions = {"-k", "0"};
    EXPECT_EQ(cmakeBuildArguments(profile, targets, false),
              (std::vector<std::string>{"--build", under("cmake-build-debug"), "--target", "app", "--", "-k", "0"}));

    // Multi-config generators pass --config; Visual Studio ones take -A unless it is external.
    CMakeProfile multi = profile;
    multi.generator = "Ninja Multi-Config";
    multi.buildConfiguration = "RelWithDebInfo";
    multi.buildToolOptions.clear();
    EXPECT_EQ(cmakeBuildArguments(multi, {}, false),
              (std::vector<std::string>{"--build", under("cmake-build-debug"), "--config", "RelWithDebInfo"}));
    CMakeProfile vs = defaultCMakeProfile(MemTree::root());
    vs.generator = "Visual Studio 17 2022";
    vs.architecture = "x64";
    auto vsArguments = cmakeConfigureArguments(vs, MemTree::root(), ninja);
    ASSERT_TRUE(vsArguments.has_value());
    EXPECT_NE(std::ranges::find(*vsArguments, "-A"), vsArguments->end());
    EXPECT_EQ(
        std::ranges::count_if(*vsArguments, [](const std::string& a) { return a.starts_with("-DCMAKE_MAKE_PROGRAM"); }),
        0);
    vs.architectureExternal = true;
    const std::vector<std::string> external = cmakeConfigureArguments(vs, MemTree::root(), ninja).value();
    EXPECT_EQ(std::ranges::find(external, "-A"), external.end());
    EXPECT_TRUE(isMultiConfigGenerator("Visual Studio 17 2022"));
    EXPECT_FALSE(isMultiConfigGenerator("Ninja"));
    EXPECT_TRUE(isPathWithin(MemTree::root(), under("a/b")));
    EXPECT_FALSE(isPathWithin(under("a"), under("ab")));
    // ".." is resolved lexically, as CMake does, on every OS: "<root>/../x" is outside the root.
    EXPECT_EQ(collapseCMakePath(under("a/../b/./c")), under("b/c"));
    EXPECT_FALSE(isPathWithin(MemTree::root(), under("a/../../outside")));
    EXPECT_EQ(collapseCMakePath("relative"), "");
}

TEST(CMakeProfiles, InheritedExternalToolsetAndArchitectureAreNotPassedToCMake) {
    MemTree tree;
    tree.write("CMakePresets.json", R"({"version": 6, "configurePresets": [
      {"name":"base", "hidden":true, "generator":"Ninja",
       "toolset":{"value":"host=x64", "strategy":"external"},
       "architecture":{"value":"x64", "strategy":"external"}},
      {"name":"debug", "inherits":"base"},
      {"name":"vs", "generator":"Visual Studio 17 2022", "toolset":"v143", "architecture":"x64"}
    ]})");
    CMakePresetsContext context;
    context.hostSystemName = "Windows";
    context.environment = [](std::string_view) -> std::optional<std::string> { return std::nullopt; };
    auto loaded = loadCMakePresets(tree.fs, MemTree::root(), context);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(loaded->has_value());
    const auto* preset = (*loaded)->findConfigurePreset("debug");
    ASSERT_NE(preset, nullptr);
    EXPECT_TRUE(preset->toolsetExternal);
    EXPECT_TRUE(preset->architectureExternal);
    const auto profile = cmakeProfileFromPreset(MemTree::root(), *preset, nullptr);
    const auto arguments = cmakeConfigureArguments(profile, MemTree::root(), {}).value();
    EXPECT_EQ(std::ranges::find(arguments, "-T"), arguments.end());
    EXPECT_EQ(std::ranges::find(arguments, "-A"), arguments.end());
    const auto* vs = (*loaded)->findConfigurePreset("vs");
    ASSERT_NE(vs, nullptr);
    const auto vsArguments =
        cmakeConfigureArguments(cmakeProfileFromPreset(MemTree::root(), *vs, nullptr), MemTree::root(), {}).value();
    EXPECT_NE(std::ranges::find(vsArguments, "-T"), vsArguments.end());
    EXPECT_NE(std::ranges::find(vsArguments, "-A"), vsArguments.end());
}

} // namespace
} // namespace frost::project
