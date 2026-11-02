// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the CMake presets reader (cmake_presets.hpp): the fixture folder under
// fixtures/cmake/presets (an include, a user file, inherits chains, conditions, $env/$penv and
// ${...} macros, build presets), every condition type, and the errors (cycles, unknown parents and
// macros, old versions).

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/cmake_presets.hpp"

#include "project_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace frost::project {
namespace {

using test::fixture;
using test::MemTree;
using test::native;

// A fixed host and parent environment, so the conditions do not depend on the machine.
CMakePresetsContext linuxContext() {
    CMakePresetsContext context;
    context.hostSystemName = "Linux";
    context.environment = [](std::string_view name) -> std::optional<std::string> {
        static const std::map<std::string, std::string, std::less<>> kVariables = {{"TINY_HOME", "/opt/tiny"},
                                                                                   {"HOME", "/home/me"}};
        const auto found = kVariables.find(name);
        return found == kVariables.end() ? std::nullopt : std::optional<std::string>(found->second);
    };
    return context;
}

const CMakeCacheVariable* cacheVariable(const CMakeConfigurePreset& preset, std::string_view name) {
    const auto found = std::ranges::find(preset.cacheVariables, name, &CMakeCacheVariable::name);
    return found == preset.cacheVariables.end() ? nullptr : &*found;
}

std::optional<std::string> environmentValue(const CMakeEnvironment& environment, std::string_view name) {
    const auto found =
        std::ranges::find(environment, name, [](const auto& entry) -> const std::string& { return entry.first; });
    return found == environment.end() ? std::nullopt : found->second;
}

std::string under(std::string_view root, std::string_view relative) {
    return io::joinPath(root, native(relative)).value();
}

TEST(CMakePresets, ReadsTheFixtureFolder) {
    const std::string root = fixture("cmake/presets");
    auto loaded = loadCMakePresets(io::realFileSystem(), root, linuxContext());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    ASSERT_TRUE(loaded->has_value());
    const CMakePresets& presets = **loaded;
    EXPECT_EQ(presets.version, 6);
    ASSERT_EQ(presets.files.size(), 3U); // The project file, its include and the user file.
    EXPECT_NE(std::ranges::find(presets.files, under(root, "presets/common.json")), presets.files.end());
    EXPECT_NE(std::ranges::find(presets.files, under(root, "CMakeUserPresets.json")), presets.files.end());

    // An included hidden preset: ${fileDir} is the included file's directory.
    const CMakeConfigurePreset* base = presets.findConfigurePreset("base");
    ASSERT_NE(base, nullptr);
    EXPECT_TRUE(base->hidden);
    EXPECT_EQ(base->file, under(root, "presets/common.json"));
    EXPECT_EQ(base->installDir, under(root, "install"));

    // debug inherits base: the generator, the binaryDir with its own ${presetName}, merged cache variables.
    const CMakeConfigurePreset* debug = presets.findConfigurePreset("debug");
    ASSERT_NE(debug, nullptr);
    EXPECT_EQ(debug->label(), "Debug (Ninja)");
    EXPECT_FALSE(debug->hidden);
    EXPECT_TRUE(debug->enabled);
    EXPECT_FALSE(debug->user);
    EXPECT_EQ(debug->generator, "Ninja");
    EXPECT_EQ(debug->binaryDir, under(root, "build/debug"));
    ASSERT_NE(cacheVariable(*debug, "CMAKE_BUILD_TYPE"), nullptr);
    EXPECT_EQ(cacheVariable(*debug, "CMAKE_BUILD_TYPE")->value, "Debug");
    ASSERT_NE(cacheVariable(*debug, "TINY_FEATURE"), nullptr);
    EXPECT_EQ(cacheVariable(*debug, "TINY_FEATURE")->value, "TRUE"); // Its own value beats base's false.
    EXPECT_EQ(cacheVariable(*debug, "TINY_FEATURE")->type, "BOOL");
    ASSERT_NE(cacheVariable(*debug, "BASE_ONLY"), nullptr);
    EXPECT_EQ(cacheVariable(*debug, "REMOVED")->value, "x");
    EXPECT_EQ(environmentValue(debug->environment, "TINY_ROOT"), std::optional<std::string>("presets"));
    EXPECT_EQ(debug->cacheVariables.front().name, "CMAKE_BUILD_TYPE"); // Own entries first.

    // release: two parents, a null that removes an inherited variable, $env{} and $penv{}.
    const CMakeConfigurePreset* release = presets.findConfigurePreset("release");
    ASSERT_NE(release, nullptr);
    EXPECT_EQ(release->binaryDir, under(root, "out/release"));
    EXPECT_EQ(cacheVariable(*release, "REMOVED"), nullptr);
    ASSERT_NE(cacheVariable(*release, "TINY_WARNINGS"), nullptr);
    ASSERT_NE(cacheVariable(*release, "TINY_FLAGS"), nullptr);
    EXPECT_EQ(cacheVariable(*release, "TINY_FLAGS")->type, "STRING");
    EXPECT_EQ(cacheVariable(*release, "TINY_FLAGS")->value, "-O2 -DX");
    EXPECT_EQ(cacheVariable(*release, "TINY_FEATURE")->value, "FALSE");
    EXPECT_EQ(environmentValue(release->environment, "TINY_PATH"), std::optional<std::string>("/opt/tiny/bin"));
    EXPECT_EQ(environmentValue(release->environment, "TINY_ROOT"), std::optional<std::string>("presets"));

    // Conditions on a Linux host.
    EXPECT_FALSE(presets.findConfigurePreset("windows-only")->enabled);
    EXPECT_TRUE(presets.findConfigurePreset("any")->enabled);
    EXPECT_TRUE(presets.findConfigurePreset("match")->enabled);

    // The user file includes the project file implicitly.
    const CMakeConfigurePreset* mine = presets.findConfigurePreset("mine");
    ASSERT_NE(mine, nullptr);
    EXPECT_TRUE(mine->user);
    EXPECT_EQ(mine->label(), "My debug");
    EXPECT_EQ(mine->binaryDir, under(root, "build/mine"));
    ASSERT_NE(cacheVariable(*mine, "MINE"), nullptr);
    EXPECT_EQ(cacheVariable(*mine, "MINE")->value, "presets-home");
    EXPECT_EQ(cacheVariable(*mine, "CMAKE_BUILD_TYPE")->value, "Debug"); // Through debug.
    EXPECT_EQ(presets.configurePresets.back().name, "mine");

    // Build presets: targets, jobs and the environment overlay.
    const auto debugBuilds = presets.buildPresetsFor("debug");
    ASSERT_EQ(debugBuilds.size(), 1U);
    EXPECT_EQ(debugBuilds.front()->name, "debug-build");
    EXPECT_EQ(debugBuilds.front()->targets, std::vector<std::string>{"app"});
    EXPECT_EQ(debugBuilds.front()->jobs, std::optional<int>(4));
    EXPECT_EQ(environmentValue(debugBuilds.front()->environment, "TINY_ROOT"), std::optional<std::string>("presets"));
    const auto releaseBuilds = presets.buildPresetsFor("release");
    ASSERT_EQ(releaseBuilds.size(), 1U);
    EXPECT_EQ(releaseBuilds.front()->configuration, "Release");
    EXPECT_EQ(environmentValue(releaseBuilds.front()->environment, "TINY_BUILD"), std::optional<std::string>("1"));
    EXPECT_EQ(environmentValue(releaseBuilds.front()->environment, "TINY_OPT"), std::optional<std::string>("-O2"));
    EXPECT_TRUE(presets.buildPresetsFor("mine").empty());
}

TEST(CMakePresets, NoFilesIsNotAnError) {
    MemTree tree;
    auto loaded = loadCMakePresets(tree.fs, MemTree::root(), linuxContext());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_FALSE(loaded->has_value());
    EXPECT_FALSE(loadCMakePresets(tree.fs, "relative", linuxContext()).has_value());
}

TEST(CMakePresets, EvaluatesEveryConditionType) {
    MemTree tree;
    tree.write("CMakePresets.json", R"({
  "version": 3,
  "configurePresets": [
    {"name": "const-true", "condition": {"type": "const", "value": true}},
    {"name": "literal-false", "condition": false},
    {"name": "null", "condition": null},
    {"name": "equals", "condition": {"type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux"}},
    {"name": "not-equals", "condition": {"type": "notEquals", "lhs": "$env{HOME}", "rhs": "/home/me"}},
    {"name": "in-list", "condition": {"type": "inList", "string": "b", "list": ["a", "b"]}},
    {"name": "not-in-list", "condition": {"type": "notInList", "string": "b", "list": ["a", "b"]}},
    {"name": "matches", "condition": {"type": "matches", "string": "x86_64-linux", "regex": "linux$"}},
    {"name": "not-matches", "condition": {"type": "notMatches", "string": "abc", "regex": "^b"}},
    {"name": "all-of", "condition": {"type": "allOf", "conditions": [true, {"type": "const", "value": false}]}},
    {"name": "any-of", "condition": {"type": "anyOf", "conditions": [false, true]}},
    {"name": "not", "condition": {"type": "not", "condition": false}},
    {"name": "inherited", "inherits": "literal-false"},
    {"name": "vendor", "cacheVariables": {"V": "$vendor{frost}", "D": "${dollar}x", "S": "a${pathListSep}b"}}
  ]
})");
    auto loaded = loadCMakePresets(tree.fs, MemTree::root(), linuxContext());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    ASSERT_TRUE(loaded->has_value());
    const std::map<std::string, bool> expected = {
        {"const-true", true},  {"literal-false", false}, {"null", true},         {"equals", true},
        {"not-equals", false}, {"in-list", true},        {"not-in-list", false}, {"matches", true},
        {"not-matches", true}, {"all-of", false},        {"any-of", true},       {"not", true},
        {"inherited", false},  {"vendor", true}};
    for (const auto& [name, enabled] : expected) {
        const CMakeConfigurePreset* preset = (*loaded)->findConfigurePreset(name);
        ASSERT_NE(preset, nullptr) << name;
        EXPECT_EQ(preset->enabled, enabled) << name;
    }
    const CMakeConfigurePreset* vendor = (*loaded)->findConfigurePreset("vendor");
    EXPECT_EQ(cacheVariable(*vendor, "V")->value, "$vendor{frost}");
    EXPECT_EQ(cacheVariable(*vendor, "D")->value, "$x");
    EXPECT_EQ(cacheVariable(*vendor, "S")->value,
              io::kNativePathStyle == io::PathStyle::Windows ? std::string("a;b") : std::string("a:b"));
}

// Loads one CMakePresets.json and returns the error code (Internal when it loaded).
ErrorCode loadError(std::string_view content, std::string_view include = {}) {
    MemTree tree;
    tree.write("CMakePresets.json", content);
    if (!include.empty()) {
        tree.write("more.json", include);
    }
    auto loaded = loadCMakePresets(tree.fs, MemTree::root(), linuxContext());
    return loaded ? ErrorCode::Internal : loaded.error().code();
}

TEST(CMakePresets, RefusesBrokenFiles) {
    EXPECT_EQ(loadError("{"), ErrorCode::Corrupt);
    EXPECT_EQ(loadError(R"({"version": 1, "configurePresets": []})"), ErrorCode::Unsupported);
    EXPECT_EQ(loadError(R"({"version": 3, "configurePresets": [
        {"name": "a", "inherits": "b"}, {"name": "b", "inherits": "a"}]})"),
              ErrorCode::Corrupt); // An inherits cycle.
    EXPECT_EQ(loadError(R"({"version": 3, "configurePresets": [{"name": "a", "inherits": "missing"}]})"),
              ErrorCode::Corrupt);
    EXPECT_EQ(loadError(R"({"version": 3, "configurePresets": [{"name": "a"}, {"name": "a"}]})"), ErrorCode::Corrupt);
    EXPECT_EQ(loadError(R"({"version": 3, "configurePresets": [{"name": "a", "binaryDir": "${nope}"}]})"),
              ErrorCode::Corrupt); // An unknown macro.
    EXPECT_EQ(loadError(R"({"version": 3, "configurePresets": [{"name": "a",
        "environment": {"A": "$env{B}", "B": "$env{A}"}}]})"),
              ErrorCode::Corrupt); // An environment cycle.
    EXPECT_EQ(loadError(R"({"version": 3, "configurePresets": [{"name": "a"}],
        "buildPresets": [{"name": "b", "configurePreset": "nope"}]})"),
              ErrorCode::Corrupt);
    EXPECT_EQ(
        loadError(R"({"version": 6, "include": ["more.json"]})", R"({"version": 6, "include": ["CMakePresets.json"]})"),
        ErrorCode::Corrupt); // An include cycle.
    EXPECT_EQ(loadError(R"({"version": 6, "include": ["missing.json"]})"), ErrorCode::NotFound);
    EXPECT_EQ(
        loadError(R"({"version": 6, "include": ["more.json"], "configurePresets": [{"name": "a", "inherits": "m"}]})",
                  R"({"version": 6, "configurePresets": [{"name": "m", "hidden": true, "generator": "Ninja"}]})"),
        ErrorCode::Internal); // A preset inheriting from an included file loads.
}

} // namespace
} // namespace frost::project
