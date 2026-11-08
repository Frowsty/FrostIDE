// SPDX-License-Identifier: Apache-2.0
//
// The real cmake as the oracle of FrostIDE's presets reader (CMK-2; label "integration" with the tag
// "cmake"): the pinned cmake that configured FrostIDE lists and configures small fixtures, and what
// FrostIDE resolves must equal what cmake printed.
/*
 * Listing: for each fixture folder (fixtures/cmake/presets, oracle/listing, oracle/conditions),
 * formatCMakePresetListing(loadCMakePresets(...).listing()) equals the stdout of
 * `cmake --list-presets=all` run in that folder: the same presets of every kind, in cmake's order
 * (the user file first, a file's own presets before its includes'), with hidden presets and
 * presets whose condition is false left out, and the same display names. The fixtures cover
 * includes (nested, from the user file, the implicit include of CMakePresets.json), every
 * condition type, macros inside conditions (also the text cmake leaves as written, such as
 * "$${x}", and the $vendor{} macro that makes cmake ignore a preset), inherited conditions,
 * a diamond include and vendor objects. The fixtures under oracle/refused are files cmake refuses
 * to read; FrostIDE must refuse them too.
 *
 * Macros and inheritance: oracle/macros is a project without languages whose CMakeLists.txt prints
 * its ORACLE_* cache variables, its ORACLE_ENV_* environment, the binary directory and the install
 * prefix. For each of its configure presets, `cmake --preset <name> --fresh` prints cmake's own
 * resolution; FrostIDE's resolved preset (cacheVariables, environment, binaryDir, installDir) must
 * equal it, and so must a configure with FrostIDE's own command line (cmakeConfigureArguments and
 * the profile's environment, which is how the CMake service configures).
 *
 * The parent environment is pinned for both sides: FROST_ORACLE_FLAVOUR=blue, and the variables
 * the fixtures read but this test leaves unset are removed from cmake's environment and read as
 * unset by FrostIDE. The configures run in a scratch copy; nothing is written to the source tree.
 */

#include "frost/core/clock.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/project/cmake_presets.hpp"
#include "frost/project/cmake_profile.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::project {
namespace {

using namespace std::chrono_literals;

std::string native(std::string_view path) {
    auto normalized = io::normalizePath(path);
    EXPECT_TRUE(normalized.has_value()) << path;
    return normalized.value_or(std::string());
}

std::string fixture(std::string_view relative) {
    return native(std::string(FROST_PROJECT_FIXTURE_DIR) + "/" + std::string(relative));
}

// The parent environment both sides see (header comment): set, or removed (nullopt).
const std::vector<std::pair<std::string, std::optional<std::string>>>& pinnedEnvironment() {
    static const std::vector<std::pair<std::string, std::optional<std::string>>> pinned = {
        {"FROST_ORACLE_FLAVOUR", "blue"},
        {"FROST_ORACLE_NOT_SET", std::nullopt},
        {"TINY_HOME", std::nullopt},
        {"TINY_OPT", std::nullopt},
    };
    return pinned;
}

CMakePresetsContext pinnedContext() {
    CMakePresetsContext context;
    context.environment = [](std::string_view name) -> std::optional<std::string> {
        for (const auto& [pinnedName, value] : pinnedEnvironment()) {
            if (pinnedName == name) {
                return value;
            }
        }
        return std::nullopt; // The fixtures read no other parent variable.
    };
    return context;
}

// Runs the pinned cmake in `directory` with the pinned environment and `extra` on top of it.
Result<io::ProcessResult> runCMake(std::vector<std::string> arguments, const std::string& directory,
                                   const CMakeEnvironment& extra = {}) {
    io::ProcessSpec spec;
    spec.executable = native(FROST_TEST_CMAKE);
    spec.arguments = std::move(arguments);
    spec.workingDirectory = directory;
    spec.environment = pinnedEnvironment();
    spec.environment.insert(spec.environment.end(), extra.begin(), extra.end());
    return io::runToCompletion(io::realProcessLauncher(), spec, {}, 5min);
}

std::string withoutCarriageReturns(std::string text) {
    std::erase(text, '\r');
    return text;
}

// Copies the directory tree `from` to `to` (real file system).
void copyTree(const std::string& from, const std::string& to) {
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(to).has_value()) << to;
    auto entries = fs.listDirectory(from);
    ASSERT_TRUE(entries.has_value()) << from;
    for (const io::DirEntry& entry : *entries) {
        const std::string source = io::joinPath(from, entry.name).value();
        const std::string target = io::joinPath(to, entry.name).value();
        if (entry.type == io::FileType::Directory) {
            copyTree(source, target);
        } else {
            auto content = fs.readFile(source);
            ASSERT_TRUE(content.has_value()) << source;
            ASSERT_TRUE(fs.writeFileAtomic(target, *content).has_value()) << target;
        }
    }
}

class CMakePresetsOracle : public ::testing::TestWithParam<std::string_view> {};

TEST_P(CMakePresetsOracle, ListsWhatCMakeLists) {
    const std::string folder = fixture(GetParam());
    auto loaded = loadCMakePresets(io::realFileSystem(), folder, pinnedContext());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    ASSERT_TRUE(loaded->has_value()) << "no presets in " << folder;

    auto listed = runCMake({"--list-presets=all"}, folder);
    ASSERT_TRUE(listed.has_value()) << listed.error().describe();
    ASSERT_EQ(listed->exitCode, 0) << listed->stderrBytes;
    const std::string expected = withoutCarriageReturns(listed->stdoutBytes);
    ASSERT_FALSE(expected.empty()) << "cmake listed nothing for " << folder;
    EXPECT_EQ(formatCMakePresetListing((*loaded)->listing()), expected) << "in " << folder;
}

INSTANTIATE_TEST_SUITE_P(Fixtures, CMakePresetsOracle,
                         ::testing::Values("cmake/presets", "cmake/oracle/listing", "cmake/oracle/conditions"),
                         [](const auto& info) {
                             std::string name(info.param);
                             std::ranges::replace(name, '/', '_');
                             return name;
                         });

/*
 * Files cmake refuses ("Could not read presets"), FrostIDE refuses too (Corrupt) instead of
 * showing presets cmake would not configure: a preset inheriting, or a build preset naming, one of
 * a file it does not include, an unknown or unterminated macro, an inheritance cycle and a
 * workflow that does not start with a configure step.
 */
class CMakePresetsRefusedOracle : public ::testing::TestWithParam<std::string_view> {};

TEST_P(CMakePresetsRefusedOracle, RefusesWhatCMakeRefuses) {
    const std::string folder = fixture(std::string("cmake/oracle/refused/") + std::string(GetParam()));
    auto listed = runCMake({"--list-presets=all"}, folder);
    ASSERT_TRUE(listed.has_value()) << listed.error().describe();
    EXPECT_NE(listed->exitCode, 0) << "cmake accepted " << folder << ":\n" << listed->stdoutBytes;
    EXPECT_NE(listed->stderrBytes.find("Could not read presets"), std::string::npos) << listed->stderrBytes;

    auto loaded = loadCMakePresets(io::realFileSystem(), folder, pinnedContext());
    ASSERT_FALSE(loaded.has_value()) << "FrostIDE accepted what cmake refused: " << listed->stderrBytes;
    EXPECT_EQ(loaded.error().code(), ErrorCode::Corrupt) << loaded.error().describe();
}

INSTANTIATE_TEST_SUITE_P(Fixtures, CMakePresetsRefusedOracle,
                         ::testing::Values("inherits-unreachable", "configure-unreachable", "unknown-macro",
                                           "unterminated-macro", "inherits-itself", "workflow-without-configure"),
                         [](const auto& info) {
                             std::string name(info.param);
                             std::ranges::replace(name, '-', '_');
                             return name;
                         });

// What the macros fixture printed: its ORACLE-* lines.
struct Printed {
    std::map<std::string, std::string> cache;
    std::map<std::string, std::string> environment;
    std::string binaryDir;
    std::string installDir;
    friend bool operator==(const Printed&, const Printed&) = default;
};

std::string describe(const Printed& printed) {
    std::string text;
    for (const auto& [name, value] : printed.cache) {
        text += "\n  cache " + name + "=" + value;
    }
    for (const auto& [name, value] : printed.environment) {
        text += "\n  env " + name + "=" + value;
    }
    return text + "\n  binary " + printed.binaryDir + "\n  install " + printed.installDir;
}

// Parses "-- ORACLE-CACHE NAME=value", "-- ORACLE-ENV NAME=value", "-- ORACLE-BINARY dir" and
// "-- ORACLE-INSTALL dir" from cmake's stdout; directories are normalised to native paths.
Printed parsePrinted(std::string_view output) {
    Printed printed;
    const auto keyValue = [](std::string_view rest, std::map<std::string, std::string>& into) {
        const auto equals = rest.find('=');
        if (equals != std::string_view::npos) {
            into[std::string(rest.substr(0, equals))] = std::string(rest.substr(equals + 1));
        }
    };
    while (!output.empty()) {
        const auto end = output.find('\n');
        std::string_view line = output.substr(0, end);
        output = end == std::string_view::npos ? std::string_view{} : output.substr(end + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (!line.starts_with("-- ORACLE-")) {
            continue;
        }
        line.remove_prefix(10);
        if (line.starts_with("CACHE ")) {
            keyValue(line.substr(6), printed.cache);
        } else if (line.starts_with("ENV ")) {
            keyValue(line.substr(4), printed.environment);
        } else if (line.starts_with("BINARY ")) {
            printed.binaryDir = native(line.substr(7));
        } else if (line.starts_with("INSTALL ")) {
            printed.installDir = native(line.substr(8));
        }
    }
    return printed;
}

// FrostIDE's resolution of `preset`, in the shape the fixture prints.
Printed resolved(const CMakeConfigurePreset& preset) {
    Printed printed;
    for (const CMakeCacheVariable& variable : preset.cacheVariables) {
        if (variable.name.starts_with("ORACLE_")) {
            printed.cache[variable.name] = variable.value;
        }
    }
    for (const auto& [name, value] : preset.environment) {
        if (name.starts_with("ORACLE_ENV_") && value) {
            printed.environment[name] = *value;
        }
    }
    printed.binaryDir = preset.binaryDir;
    printed.installDir = preset.installDir;
    return printed;
}

TEST(CMakePresetsOracle, ResolvesMacrosAndInheritanceAsCMakeConfigures) {
    test::ScratchDir scratch("cmake-presets-oracle");
    const std::string folder = scratch.join("macros");
    copyTree(fixture("cmake/oracle/macros"), folder);
    auto loaded = loadCMakePresets(io::realFileSystem(), folder, pinnedContext());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    ASSERT_TRUE(loaded->has_value());
    const std::string ninja = native(FROST_TEST_NINJA);

    int checked = 0;
    for (const CMakeConfigurePreset& preset : (*loaded)->configurePresets) {
        if (preset.hidden || !preset.enabled) {
            continue;
        }
        SCOPED_TRACE(preset.name);
        ++checked;
        auto configured =
            runCMake({"--preset", preset.name, "--fresh", "-DCMAKE_MAKE_PROGRAM:FILEPATH=" + ninja}, folder);
        ASSERT_TRUE(configured.has_value()) << configured.error().describe();
        ASSERT_EQ(configured->exitCode, 0) << configured->stdoutBytes << configured->stderrBytes;
        const Printed oracle = parsePrinted(configured->stdoutBytes);
        ASSERT_FALSE(oracle.cache.empty()) << configured->stdoutBytes;
        const Printed ours = resolved(preset);
        EXPECT_EQ(ours, oracle) << "FrostIDE:" << describe(ours) << "\ncmake --preset:" << describe(oracle);

        // The service's own configure: FrostIDE's command line and environment, not --preset.
        const CMakeProfile profile = cmakeProfileFromPreset(folder, preset, nullptr);
        auto arguments = cmakeConfigureArguments(profile, folder, ninja);
        ASSERT_TRUE(arguments.has_value()) << arguments.error().describe();
        arguments->push_back("--fresh");
        auto direct = runCMake(*arguments, folder, profile.environment);
        ASSERT_TRUE(direct.has_value()) << direct.error().describe();
        ASSERT_EQ(direct->exitCode, 0) << direct->stdoutBytes << direct->stderrBytes;
        const Printed viaCommandLine = parsePrinted(direct->stdoutBytes);
        EXPECT_EQ(viaCommandLine, oracle)
            << "FrostIDE's configure:" << describe(viaCommandLine) << "\ncmake --preset:" << describe(oracle);
    }
    EXPECT_EQ(checked, 2) << "the fixture has two visible configure presets";
}

} // namespace
} // namespace frost::project
