// SPDX-License-Identifier: Apache-2.0
//
// CMakePresets.json and CMakeUserPresets.json (cmake-presets(7), versions 2 and later): the native
// presets reader behind CMake profiles (PLAN.md section 2, "CMake"; ADR 0046). Reading presets
// runs nothing, so it is allowed before the folder is trusted (PLAN.md section 3.5).
//
/*
 * Files. <sourceDir>/CMakePresets.json and <sourceDir>/CMakeUserPresets.json, either of which may
 * be missing; the user file implicitly includes the project file. "include" (version 4 and later)
 * names more files, relative to the including file's directory; macros ${sourceDir},
 * ${sourceParentDir}, ${sourceDirName}, ${hostSystemName}, ${fileDir}, ${pathListSep}, ${dollar}
 * and $penv{} are expanded in include paths. A file included twice is read once; an include cycle
 * or a missing included file is an error. Every file read is listed in CMakePresets::files, so the
 * CMake service can reconfigure when one changes.
 *
 * Inheritance. "inherits" (a string or an array) names presets of the same kind from any file
 * read. A preset takes every field it does not set itself from its parents in order (the first
 * parent that has it, recursively), except name, hidden, inherits, displayName and description;
 * "condition" is inherited like any other field. cacheVariables and environment are merged: the
 * preset's own entries win, then the parents' in order. A cycle, an unknown parent or a parent of
 * the other kind is an error; so is a duplicate preset name within one kind.
 *
 * Macros (expanded in generator, binaryDir, installDir, toolchainFile, cacheVariables values,
 * environment values, build preset fields and condition operands): ${sourceDir} (the source
 * directory with '/' separators, as CMake spells it), ${sourceParentDir}, ${sourceDirName},
 * ${presetName}, ${generator}, ${hostSystemName}, ${fileDir}, ${dollar}, ${pathListSep};
 * $env{NAME} (the preset's own environment entry, itself expanded, else the parent environment);
 * $penv{NAME} (the parent environment only). $vendor{...} is left as written. An unknown ${...}
 * macro or an environment reference cycle is an error. A missing variable expands to "".
 *
 * Conditions (version 3 and later): true, false, null, and objects of type const, equals,
 * notEquals, inList, notInList, matches, notMatches (ECMAScript regex search), anyOf, allOf and
 * not. A preset whose condition is false is kept with enabled = false.
 *
 * Paths. binaryDir, installDir and toolchainFile are made absolute (relative to the source
 * directory) and normalised to the native style; empty when the preset does not set them.
 *
 * Errors carry the file and the preset: Corrupt for malformed files and the rule violations above,
 * Unsupported for a "version" below 2. All functions are pure apart from the file reads through
 * `fs` and the environment lookups through the context; thread-compatible.
 */
#pragma once

#include "frost/core/error.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::project {

// Environment changes in order: a value sets the variable, nullopt unsets it (io::ProcessSpec's form).
using CMakeEnvironment = std::vector<std::pair<std::string, std::optional<std::string>>>;

// One -D entry: NAME[:TYPE]=VALUE.
struct CMakeCacheVariable {
    std::string name;
    std::string type{}; // "BOOL", "STRING", "PATH", "FILEPATH", "INTERNAL" or "" (untyped).
    std::string value{};

    friend bool operator==(const CMakeCacheVariable&, const CMakeCacheVariable&) = default;
};

// A resolved configure preset (inheritance applied, macros expanded).
struct CMakeConfigurePreset {
    std::string name;
    std::string displayName{}; // As written (not inherited); "" when absent.
    std::string description{};
    std::string file{}; // The absolute file that defines it.
    bool user = false;  // Defined in CMakeUserPresets.json or a file only it includes.
    bool hidden = false;
    bool enabled = true; // Its condition holds.
    std::string generator{};
    std::string binaryDir{};           // Absolute and native; "" when not set.
    std::string installDir{};          // Absolute and native; "" when not set.
    std::string toolchainFile{};       // Absolute and native; "" when not set.
    std::string architecture{};        // -A value, or the vcvars architecture when architectureExternal.
    bool architectureExternal = false; // strategy "external": the IDE sets up the environment.
    std::string toolset{};
    bool toolsetExternal = false;
    // A null cacheVariables entry removes an inherited one, so it never appears here.
    std::vector<CMakeCacheVariable> cacheVariables{};
    CMakeEnvironment environment{}; // Expanded; nullopt entries unset the variable.

    // The name shown in profile choosers: displayName, else name.
    [[nodiscard]] const std::string& label() const noexcept { return displayName.empty() ? name : displayName; }
};

// A resolved build preset.
struct CMakeBuildPreset {
    std::string name;
    std::string displayName{};
    std::string description{};
    std::string file{};
    bool user = false;
    bool hidden = false;
    bool enabled = true;
    std::string configurePreset{}; // The configure preset it builds; "" only for hidden presets.
    std::string configuration{};   // --config.
    std::vector<std::string> targets{};
    std::optional<int> jobs{};
    bool cleanFirst = false;
    std::vector<std::string> nativeToolOptions{};
    // The environment for `cmake --build`: the configure preset's (unless inheritConfigureEnvironment
    // is false) overlaid with the build preset's own.
    CMakeEnvironment environment{};
};

struct CMakePresets {
    int version = 0; // The project file's version (the user file's when only it exists).
    // Every preset, hidden and disabled ones included: the project file's (and its includes') in
    // document order, then the user file's.
    std::vector<CMakeConfigurePreset> configurePresets{};
    std::vector<CMakeBuildPreset> buildPresets{};
    std::vector<std::string> files{}; // Every file read, absolute.

    // The configure preset named `name`, or nullptr.
    [[nodiscard]] const CMakeConfigurePreset* findConfigurePreset(std::string_view name) const noexcept;
    // The non-hidden, enabled build presets of configure preset `configurePreset`, in order.
    [[nodiscard]] std::vector<const CMakeBuildPreset*> buildPresetsFor(std::string_view configurePreset) const;
};

// What macros and conditions see besides the files.
struct CMakePresetsContext {
    // ${hostSystemName}: "Windows", "Linux" or "Darwin"; empty uses cmakeHostSystemName().
    std::string hostSystemName{};
    // The parent environment ($env{}, $penv{}); empty uses this process's environment.
    std::function<std::optional<std::string>(std::string_view name)> environment{};
};

// CMake's name of the host system: "Windows", "Linux" or "Darwin".
[[nodiscard]] std::string_view cmakeHostSystemName() noexcept;

// "CMakePresets.json" and "CMakeUserPresets.json".
inline constexpr std::string_view kCMakePresetsFile = "CMakePresets.json";
inline constexpr std::string_view kCMakeUserPresetsFile = "CMakeUserPresets.json";

/*
 * Reads and resolves the presets of the source directory `sourceDir` (absolute). nullopt when
 * neither file exists. Errors: InvalidArgument (sourceDir), the read errors of `fs` other than a
 * missing top-level file, Corrupt and Unsupported (header comment). O(total size + presets^2).
 */
[[nodiscard]] Result<std::optional<CMakePresets>> loadCMakePresets(io::IFileSystem& fs, std::string_view sourceDir,
                                                                   const CMakePresetsContext& context = {});

} // namespace frost::project
