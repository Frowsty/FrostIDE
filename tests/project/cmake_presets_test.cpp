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
#include <format>
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
    // CMake reads the user file first: its presets lead (cmake --list-presets; CMakePresetsOracle).
    EXPECT_EQ(presets.configurePresets.front().name, "mine");

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
        {"const-true", true},
        {"literal-false", false},
        {"null", true},
        {"equals", true},
        {"not-equals", false},
        {"in-list", true},
        {"not-in-list", false},
        {"matches", true},
        {"not-matches", true},
        {"all-of", false},
        {"any-of", true},
        {"not", true},
        // cmake ignores a preset that uses $vendor{} (CMakePresetsOracle, conditions fixture).
        {"inherited", false},
        {"vendor", false}};
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

    // Version gates, as in CMake: include (4), conditions (3), package and workflow presets (6).
    EXPECT_EQ(loadError(R"({"version": 3, "include": ["more.json"]})", R"({"version": 6})"), ErrorCode::Corrupt);
    EXPECT_EQ(loadError(R"({"version": 2, "configurePresets": [{"name": "a", "condition": true}]})"),
              ErrorCode::Corrupt);
    EXPECT_EQ(loadError(R"({"version": 5, "configurePresets": [{"name": "a"}],
        "packagePresets": [{"name": "p", "configurePreset": "a"}]})"),
              ErrorCode::Corrupt);
    EXPECT_EQ(loadError(R"({"version": 5, "configurePresets": [{"name": "a"}],
        "workflowPresets": [{"name": "w", "steps": [{"type": "configure", "name": "a"}]}]})"),
              ErrorCode::Corrupt);
    // Test and package presets need a known configure preset; duplicates are refused per kind.
    EXPECT_EQ(loadError(R"({"version": 6, "configurePresets": [{"name": "a"}],
        "testPresets": [{"name": "t", "configurePreset": "nope"}]})"),
              ErrorCode::Corrupt);
    EXPECT_EQ(loadError(R"({"version": 6, "configurePresets": [{"name": "a"}],
        "testPresets": [{"name": "t", "configurePreset": "a"}, {"name": "t", "configurePreset": "a"}]})"),
              ErrorCode::Corrupt);
    // A name may repeat across kinds (FrostEngine's windows-debug is a configure, build and test preset).
    EXPECT_EQ(loadError(R"({"version": 6, "configurePresets": [{"name": "a"}],
        "buildPresets": [{"name": "a", "configurePreset": "a"}], "testPresets": [{"name": "a", "configurePreset": "a"}]})"),
              ErrorCode::Internal);
}

// Workflow presets: CMake's step rules (first a configure step, only then other kinds, every
// step visible and on the first step's configure preset).
/*
 * CMake's macro scan (CMakePresetsOracle checks the same cases against cmake): text after '$'
 * that cannot begin a known namespace is copied as written up to and including the character
 * that ended it, and the scan goes on, so an inner ${...} after "$a{" still expands. A preset
 * may refer only to presets of its own file or of files it includes.
 */
TEST(CMakePresets, ScansMacrosAndReachabilityAsCMakeDoes) {
    MemTree tree;
    tree.write("CMakePresets.json", R"({"version": 6, "configurePresets": [
      {"name": "p", "cacheVariables": {"A": "$${x}", "B": "$$env{X}", "C": "$a{${presetName}}", "D": "$e{x}",
                                       "E": "$ {x}-$x-end$", "F": "${dollar}{x}"}}]})");
    auto loaded = loadCMakePresets(tree.fs, MemTree::root(), linuxContext());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    ASSERT_TRUE(loaded->has_value());
    const CMakeConfigurePreset* p = (*loaded)->findConfigurePreset("p");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(cacheVariable(*p, "A")->value, "$${x}");
    EXPECT_EQ(cacheVariable(*p, "B")->value, "$$env{X}");
    EXPECT_EQ(cacheVariable(*p, "C")->value, "$a{p}");
    EXPECT_EQ(cacheVariable(*p, "D")->value, "$e{x}");
    EXPECT_EQ(cacheVariable(*p, "E")->value, "$ {x}-$x-end$");
    EXPECT_EQ(cacheVariable(*p, "F")->value, "${x}");
    EXPECT_TRUE(p->enabled);

    EXPECT_EQ(loadError(R"({"version": 6, "configurePresets": [{"name": "a", "cacheVariables": {"A": "$env{X"}}]})"),
              ErrorCode::Corrupt); // Unterminated.
    EXPECT_EQ(loadError(R"({"version": 6, "configurePresets": [{"name": "a", "cacheVariables": {"A": "$vendor{x"}}]})"),
              ErrorCode::Corrupt);
    // more.json's preset inherits one of the file that includes it: unreachable from more.json.
    EXPECT_EQ(loadError(R"({"version": 6, "include": ["more.json"], "configurePresets": [{"name": "root"}]})",
                        R"({"version": 6, "configurePresets": [{"name": "child", "inherits": "root"}]})"),
              ErrorCode::Corrupt);
    EXPECT_EQ(loadError(R"({"version": 6, "include": ["more.json"], "configurePresets": [{"name": "root"}]})",
                        R"({"version": 6, "buildPresets": [{"name": "b", "configurePreset": "root"}]})"),
              ErrorCode::Corrupt);
    // The other way round (and through the user file's implicit include) is fine.
    EXPECT_EQ(
        loadError(R"({"version": 6, "include": ["more.json"], "configurePresets": [{"name": "root", "inherits": "m"}],
        "buildPresets": [{"name": "b", "configurePreset": "m"}]})",
                  R"({"version": 6, "configurePresets": [{"name": "m"}]})"),
        ErrorCode::Internal);
}

TEST(CMakePresets, ChecksWorkflowSteps) {
    constexpr std::string_view kKinds =
        R"("configurePresets": [{"name": "a"}, {"name": "b"}, {"name": "h", "hidden": true}],
        "buildPresets": [{"name": "ba", "configurePreset": "a"}, {"name": "bb", "configurePreset": "b"}],
        "testPresets": [{"name": "ta", "configurePreset": "a"}],
        "packagePresets": [{"name": "pa", "configurePreset": "a"}],)";
    const auto workflow = [&](std::string_view steps) {
        return std::format(R"({{"version": 6, {} "workflowPresets": [{{"name": "w", "steps": [{}]}}]}})", kKinds,
                           steps);
    };
    MemTree tree;
    tree.write("CMakePresets.json", workflow(R"({"type": "configure", "name": "a"}, {"type": "build", "name": "ba"},
                           {"type": "test", "name": "ta"}, {"type": "package", "name": "pa"})"));
    auto loaded = loadCMakePresets(tree.fs, MemTree::root(), linuxContext());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    ASSERT_TRUE(loaded->has_value());
    ASSERT_EQ((*loaded)->workflowPresets.size(), 1U);
    EXPECT_EQ((*loaded)->workflowPresets[0].steps, (std::vector<CMakeWorkflowStep>{{CMakePresetKind::Configure, "a"},
                                                                                   {CMakePresetKind::Build, "ba"},
                                                                                   {CMakePresetKind::Test, "ta"},
                                                                                   {CMakePresetKind::Package, "pa"}}));

    EXPECT_EQ(loadError(workflow(R"({"type": "build", "name": "ba"})")), ErrorCode::Corrupt); // No configure first.
    EXPECT_EQ(loadError(workflow(R"({"type": "configure", "name": "a"}, {"type": "configure", "name": "b"})")),
              ErrorCode::Corrupt); // Two configures.
    EXPECT_EQ(loadError(workflow(R"({"type": "configure", "name": "a"}, {"type": "build", "name": "bb"})")),
              ErrorCode::Corrupt); // Another configure preset.
    EXPECT_EQ(loadError(workflow(R"({"type": "configure", "name": "h"})")), ErrorCode::Corrupt);  // Hidden.
    EXPECT_EQ(loadError(workflow(R"({"type": "configure", "name": "zz"})")), ErrorCode::Corrupt); // Unknown.
    EXPECT_EQ(loadError(workflow(R"({"type": "install", "name": "a"})")), ErrorCode::Corrupt);    // Unknown type.
    EXPECT_EQ(loadError(workflow("")), ErrorCode::Corrupt);                                       // No steps.
}

// Test and package presets: inheritance member by member, macros with the configure preset's
// generator, the environment overlay and conditions.
TEST(CMakePresets, ReadsTestAndPackagePresets) {
    MemTree tree;
    tree.write("CMakePresets.json", R"({"version": 6,
      "configurePresets": [{"name": "dev", "generator": "Ninja", "environment": {"FROM_CONFIGURE": "c"}}],
      "testPresets": [
        {"name": "base", "hidden": true, "output": {"outputOnFailure": true},
         "execution": {"noTestsAction": "error", "jobs": 2}, "filter": {"exclude": {"label": "slow"}}},
        {"name": "dev-test", "inherits": "base", "configurePreset": "dev", "configuration": "Debug",
         "execution": {"stopOnFailure": true, "timeout": 30},
         "filter": {"include": {"name": "${presetName}-${generator}", "label": "fast"}},
         "environment": {"OWN": "$env{FROM_CONFIGURE}x"}},
        {"name": "isolated", "configurePreset": "dev", "inheritConfigureEnvironment": false},
        {"name": "off", "configurePreset": "dev", "condition": {"type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows"}}
      ],
      "packagePresets": [
        {"name": "zip", "configurePreset": "dev", "generators": ["ZIP", "TGZ"], "configurations": ["Release"],
         "packageName": "${presetName}-pkg", "packageVersion": "1.2", "packageDirectory": "${sourceDir}/dist"}
      ]})");
    auto loaded = loadCMakePresets(tree.fs, MemTree::root(), linuxContext());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    ASSERT_TRUE(loaded->has_value());
    const CMakePresets& presets = **loaded;
    ASSERT_EQ(presets.testPresets.size(), 4U);
    const CMakeTestPreset& test = presets.testPresets[1];
    EXPECT_EQ(test.name, "dev-test");
    EXPECT_EQ(test.configurePreset, "dev");
    EXPECT_EQ(test.configuration, "Debug");
    EXPECT_TRUE(test.outputOnFailure);      // From base's output.
    EXPECT_EQ(test.noTestsAction, "error"); // From base's execution, beside its own members.
    EXPECT_EQ(test.jobs, std::optional<int>(2));
    EXPECT_TRUE(test.stopOnFailure);
    EXPECT_EQ(test.timeout, std::optional<int>(30));
    EXPECT_EQ(test.includeName, "dev-test-Ninja");
    EXPECT_EQ(test.includeLabel, "fast");
    EXPECT_EQ(test.excludeLabel, "slow");
    EXPECT_EQ(environmentValue(test.environment, "OWN"), std::optional<std::string>("cx"));
    EXPECT_EQ(environmentValue(test.environment, "FROM_CONFIGURE"), std::optional<std::string>("c"));
    EXPECT_EQ(environmentValue(presets.testPresets[2].environment, "FROM_CONFIGURE"), std::nullopt);
    EXPECT_FALSE(presets.testPresets[3].enabled);
    const auto forDev = presets.testPresetsFor("dev");
    ASSERT_EQ(forDev.size(), 2U); // Not the hidden base, not the disabled one.
    EXPECT_EQ(forDev[0]->name, "dev-test");

    ASSERT_EQ(presets.packagePresets.size(), 1U);
    const CMakePackagePreset& zip = presets.packagePresets[0];
    EXPECT_EQ(zip.generators, (std::vector<std::string>{"ZIP", "TGZ"}));
    EXPECT_EQ(zip.configurations, std::vector<std::string>{"Release"});
    EXPECT_EQ(zip.packageName, "zip-pkg");
    EXPECT_EQ(zip.packageVersion, "1.2");
    std::string dist = MemTree::root() + "/dist";
    std::ranges::replace(dist, '\\', '/');
    EXPECT_EQ(zip.packageDirectory, dist); // Not a path field: expanded, not normalised.
}

/*
 * The order and the listing text of `cmake --list-presets=all` (CMakePresetsOracle compares them
 * with the real cmake): the user file first, a file's own presets before its includes', hidden
 * and disabled presets left out (a build preset of a disabled configure preset stays), names
 * padded to the longest of their kind before " - displayName".
 */
TEST(CMakePresets, ListsLikeCMake) {
    MemTree tree;
    tree.write("CMakePresets.json", R"({"version": 6, "include": ["inc/more.json"],
      "configurePresets": [{"name": "a", "displayName": "Alpha"}, {"name": "h", "hidden": true},
                           {"name": "off", "condition": false}, {"name": "c2", "inherits": "a"}],
      "buildPresets": [{"name": "a-1", "configurePreset": "a", "displayName": "Build one"},
                       {"name": "off-b", "configurePreset": "off"}, {"name": "hb", "configurePreset": "a", "hidden": true},
                       {"name": "cb", "configurePreset": "a", "condition": false}],
      "workflowPresets": [{"name": "wf", "displayName": "Flow", "steps": [{"type": "configure", "name": "a"}]}]})");
    tree.write("inc/more.json",
               R"({"version": 6, "include": ["deeper.json"], "configurePresets": [{"name": "inc1"}]})");
    tree.write("inc/deeper.json", R"({"version": 6, "configurePresets": [{"name": "deep"}]})");
    tree.write("CMakeUserPresets.json", R"({"version": 6, "include": ["uinc.json"],
      "configurePresets": [{"name": "user1", "inherits": "a", "displayName": "Mine"}]})");
    tree.write("uinc.json", R"({"version": 6, "configurePresets": [{"name": "uinc"}]})");
    auto loaded = loadCMakePresets(tree.fs, MemTree::root(), linuxContext());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    ASSERT_TRUE(loaded->has_value());
    const CMakePresetListing listing = (*loaded)->listing();
    std::vector<std::string> configure;
    for (const auto& entry : listing.configure) {
        configure.push_back(entry.name);
    }
    EXPECT_EQ(configure, (std::vector<std::string>{"user1", "uinc", "a", "c2", "inc1", "deep"}));
    EXPECT_TRUE((*loaded)->findConfigurePreset("user1")->user);
    EXPECT_TRUE((*loaded)->findConfigurePreset("uinc")->user);
    EXPECT_FALSE((*loaded)->findConfigurePreset("a")->user);
    EXPECT_EQ(formatCMakePresetListing(listing), "Available configure presets:\n"
                                                 "\n"
                                                 "  \"user1\" - Mine\n"
                                                 "  \"uinc\"\n"
                                                 "  \"a\"     - Alpha\n"
                                                 "  \"c2\"\n"
                                                 "  \"inc1\"\n"
                                                 "  \"deep\"\n"
                                                 "\n"
                                                 "Available build presets:\n"
                                                 "\n"
                                                 "  \"a-1\"   - Build one\n"
                                                 "  \"off-b\"\n"
                                                 "\n"
                                                 "Available workflow presets:\n"
                                                 "\n"
                                                 "  \"wf\" - Flow\n");
    EXPECT_EQ(formatCMakePresetListing({}), "");
}

} // namespace
} // namespace frost::project
