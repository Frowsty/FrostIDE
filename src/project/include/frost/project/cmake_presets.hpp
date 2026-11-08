// SPDX-License-Identifier: Apache-2.0
//
// CMakePresets.json and CMakeUserPresets.json (cmake-presets(7), versions 2 and later): the native
// presets reader behind CMake profiles (PLAN.md section 2, "CMake"; ADR 0046). Reading presets
// runs nothing, so it is allowed before the folder is trusted (PLAN.md section 3.5).
//
/*
 * Files. <sourceDir>/CMakePresets.json and <sourceDir>/CMakeUserPresets.json, either of which may
 * be missing. "include" (version 4 and later) names more files, relative to the including file's
 * directory; macros ${sourceDir}, ${sourceParentDir}, ${sourceDirName}, ${hostSystemName},
 * ${fileDir}, ${pathListSep}, ${dollar} and $penv{} are expanded in include paths. A file included
 * twice is read once; an include cycle or a missing included file is an error. Every file read is
 * listed in CMakePresets::files, so the CMake service can reconfigure when one changes.
 *
 * Order (what `cmake --list-presets` prints, checked against the real cmake by the oracle tests):
 * a file's own presets come first, then those of its includes in order, depth first. When the
 * user file exists it is read first and includes the project file implicitly, after its own
 * includes; otherwise the project file is read alone. So the user's presets lead every list.
 *
 * Kinds. Configure, build and test presets (version 2), package and workflow presets (version 6).
 * Include needs version 4, a condition version 3; an older file that uses one is an error, as in
 * CMake. Every kind except workflow has hidden, inherits and condition; build, test and package
 * presets name their configure preset ("configurePreset"), which must exist for a visible preset.
 * A workflow preset's steps start with a configure step, name existing visible presets of their
 * type, and every later step's preset uses the first step's configure preset.
 *
 * Inheritance. "inherits" (a string or an array) names presets of the same kind from the
 * preset's own file or a file it includes, directly or not (the user file reaches the project
 * file through its implicit include); a preset of another file is an error, as in CMake ("is
 * unreachable from preset's file"), and so is a build, test or package preset whose
 * configurePreset is not reachable that way. A preset takes every field it does not set itself from its parents in
 * order (the first parent that has it, recursively), except name, hidden, inherits, displayName and description;
 * "condition" is inherited like any other field. Object fields CMake merges member by member
 * (warnings, errors, debug; a test preset's filter, execution and output) are inherited per
 * member. cacheVariables and environment are merged: the preset's own entries win, then the
 * parents' in order. A cycle, an unknown parent or a parent of another kind is an error; so is a
 * duplicate preset name within one kind.
 *
 * Macros (expanded in generator, binaryDir, installDir, toolchainFile, cacheVariables values,
 * environment values, the string fields of build, test and package presets and condition
 * operands): ${sourceDir} (the source directory with '/' separators, as CMake spells it),
 * ${sourceParentDir}, ${sourceDirName}, ${presetName}, ${generator} (a build, test or package
 * preset's: its configure preset's), ${hostSystemName}, ${fileDir}, ${dollar}, ${pathListSep};
 * $env{NAME} (the preset's own environment entry, itself expanded, else the parent environment);
 * $penv{NAME} (the parent environment only). $vendor{...} is left as written, and the preset
 * that uses it is ignored as CMake ignores it (enabled = false: not listed, not an error). An
 * unknown ${...} macro, an unterminated macro or an environment reference cycle is an error. A
 * missing variable expands to "". Text after '$' that cannot begin "{", "env{", "penv{" or
 * "vendor{" (another '$', a space, "e{") is copied as written with the character that ended it,
 * as CMake does: "$${x}" stays "$${x}", "$a{${dollar}}" becomes "$a{$}".
 *
 * Conditions (version 3 and later): true, false, null, and objects of type const, equals,
 * notEquals, inList, notInList, matches, notMatches (ECMAScript regex search), anyOf, allOf and
 * not. A preset whose condition is false is kept with enabled = false.
 *
 * Paths. binaryDir, installDir and toolchainFile are made absolute (relative to the source
 * directory) and normalised to the native style; empty when the preset does not set them.
 *
 * Unknown fields, "vendor" objects (file and preset level) and "$schema"/"cmakeMinimumRequired"
 * are ignored. Deviation: CMake itself refuses an unknown field ("Invalid extra field"); FrostIDE
 * reads past it, so a typo there shows in `cmake --preset`, not here. The configure preset fields
 * "cmakeExecutable" and "trace" are read past too: FrostIDE runs its own resolved cmake.
 *
 * Errors carry the file and the preset: Corrupt for malformed files and the rule violations above,
 * Unsupported for a "version" below 2. All functions are pure apart from the file reads through
 * `fs` and the environment lookups through the context; thread-compatible.
 */
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
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
    /*
     * The switches of "warnings", "errors" and "debug", in that order: -Wdev/-Wno-dev,
     * -Wdeprecated/-Wno-deprecated, --warn-uninitialized, --no-warn-unused-cli,
     * --check-system-vars, -Werror=dev/-Wno-error=dev, -Werror=deprecated/-Wno-error=deprecated,
     * --debug-output, --debug-trycompile, --debug-find (what `cmake --preset` adds for them).
     */
    std::vector<std::string> extraArguments{};

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
    bool cleanFirst = false; // --clean-first.
    bool verbose = false;    // --verbose.
    std::vector<std::string> nativeToolOptions{};
    // The environment for `cmake --build`: the configure preset's (unless inheritConfigureEnvironment
    // is false) overlaid with the build preset's own.
    CMakeEnvironment environment{};

    [[nodiscard]] const std::string& label() const noexcept { return displayName.empty() ? name : displayName; }
};

// A resolved test preset (`ctest --preset`): the fields FrostIDE reads; the others are kept by ctest.
struct CMakeTestPreset {
    std::string name;
    std::string displayName{};
    std::string description{};
    std::string file{};
    bool user = false;
    bool hidden = false;
    bool enabled = true;
    std::string configurePreset{};
    std::string configuration{};    // -C.
    std::string includeName{};      // filter.include.name (-R).
    std::string includeLabel{};     // filter.include.label (-L).
    std::string excludeName{};      // filter.exclude.name (-E).
    std::string excludeLabel{};     // filter.exclude.label (-LE).
    std::optional<int> jobs{};      // execution.jobs.
    bool stopOnFailure = false;     // execution.stopOnFailure.
    std::optional<int> timeout{};   // execution.timeout, seconds.
    std::string noTestsAction{};    // execution.noTestsAction: "default", "error" or "ignore".
    bool outputOnFailure = false;   // output.outputOnFailure.
    CMakeEnvironment environment{}; // As for build presets.
};

// A resolved package preset (`cpack --preset`).
struct CMakePackagePreset {
    std::string name;
    std::string displayName{};
    std::string description{};
    std::string file{};
    bool user = false;
    bool hidden = false;
    bool enabled = true;
    std::string configurePreset{};
    std::vector<std::string> generators{};
    std::vector<std::string> configurations{};
    std::string packageName{};
    std::string packageVersion{};
    std::string packageDirectory{};
    CMakeEnvironment environment{};
};

enum class CMakePresetKind : std::uint8_t { Configure, Build, Test, Package, Workflow };

// "configure", "build", "test", "package" or "workflow" (the spelling of a workflow step's type).
[[nodiscard]] std::string_view toString(CMakePresetKind kind) noexcept;

struct CMakeWorkflowStep {
    CMakePresetKind kind = CMakePresetKind::Configure; // Never Workflow.
    std::string name{};

    friend bool operator==(const CMakeWorkflowStep&, const CMakeWorkflowStep&) = default;
};

// A workflow preset (`cmake --workflow --preset`): no hidden, inherits or condition (cmake-presets(7)).
struct CMakeWorkflowPreset {
    std::string name;
    std::string displayName{};
    std::string description{};
    std::string file{};
    bool user = false;
    std::vector<CMakeWorkflowStep> steps{}; // Validated (header comment, "Kinds").
};

// One line of `cmake --list-presets`: a preset's name and its displayName ("" when it has none).
struct CMakePresetListEntry {
    std::string name;
    std::string displayName{};

    friend bool operator==(const CMakePresetListEntry&, const CMakePresetListEntry&) = default;
};

// What `cmake --list-presets=all` lists: per kind, the presets that are not hidden and whose
// condition holds, in order. A build, test or package preset is listed even when its configure
// preset's condition is false, as CMake does.
struct CMakePresetListing {
    std::vector<CMakePresetListEntry> configure{};
    std::vector<CMakePresetListEntry> build{};
    std::vector<CMakePresetListEntry> test{};
    std::vector<CMakePresetListEntry> package{};
    std::vector<CMakePresetListEntry> workflow{};

    friend bool operator==(const CMakePresetListing&, const CMakePresetListing&) = default;
};

struct CMakePresets {
    int version = 0; // The project file's version (the user file's when only it exists).
    // Every preset, hidden and disabled ones included, in CMake's order (header comment, "Order").
    std::vector<CMakeConfigurePreset> configurePresets{};
    std::vector<CMakeBuildPreset> buildPresets{};
    std::vector<CMakeTestPreset> testPresets{};
    std::vector<CMakePackagePreset> packagePresets{};
    std::vector<CMakeWorkflowPreset> workflowPresets{};
    std::vector<std::string> files{}; // Every file read, absolute, in the order read.

    // The configure preset named `name`, or nullptr.
    [[nodiscard]] const CMakeConfigurePreset* findConfigurePreset(std::string_view name) const noexcept;
    // The build preset named `name`, or nullptr.
    [[nodiscard]] const CMakeBuildPreset* findBuildPreset(std::string_view name) const noexcept;
    // The non-hidden, enabled build presets of configure preset `configurePreset`, in order.
    [[nodiscard]] std::vector<const CMakeBuildPreset*> buildPresetsFor(std::string_view configurePreset) const;
    // The non-hidden, enabled test presets of configure preset `configurePreset`, in order.
    [[nodiscard]] std::vector<const CMakeTestPreset*> testPresetsFor(std::string_view configurePreset) const;
    // What `cmake --list-presets=all` lists (CMakePresetListing). O(presets).
    [[nodiscard]] CMakePresetListing listing() const;
};

/*
 * The text `cmake --list-presets=all` prints for `listing`: per kind with entries, a heading
 * ("Available configure presets:"), a blank line and one line per preset, `  "name"`, padded to
 * the longest quoted name of that kind and followed by ` - displayName` when there is one; kinds
 * are separated by a blank line. "" for an empty listing. Lines end with '\n'. Pure.
 */
[[nodiscard]] std::string formatCMakePresetListing(const CMakePresetListing& listing);

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
