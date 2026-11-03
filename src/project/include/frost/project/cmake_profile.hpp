// SPDX-License-Identifier: Apache-2.0
//
// CMake profiles (CLion's term; ADR 0046): how one folder is configured and built. A profile comes
// from a configure preset, from the `cmake.profiles` setting, or is the default Debug profile, and
// turns into the command lines of `cmake` (configure) and `cmake --build`.
//
/*
 * Choice. When the folder has presets (cmake_presets.hpp) with at least one visible (not hidden,
 * enabled) configure preset, the profiles are those presets, in file order, each with its first
 * visible build preset. Otherwise they are the `cmake.profiles` setting's records that apply to the
 * folder; without any, the single default profile: "Debug", build directory cmake-build-debug,
 * generator Ninja, CMAKE_BUILD_TYPE=Debug (CLion's default).
 *
 * The trust rule (PLAN.md section 3.5). A profile's build directory may lie outside the folder only
 * when the user chose it in an application-scope profile for that folder (a record whose `root`
 * names it). A preset or a project-scope setting naming a directory outside the folder makes the
 * profile unusable (CMakeProfile::problem), so a repository cannot aim configure or build output
 * at an arbitrary directory.
 *
 * Command lines (cmakeConfigureArguments): -S <source> -B <build> [-G <generator>]
 * [-T <toolset>, unless the strategy is external]
 * [-A <architecture>, unless the strategy is external] [-DCMAKE_BUILD_TYPE=<type>]
 * [-DCMAKE_TOOLCHAIN_FILE=...] [-DCMAKE_INSTALL_PREFIX=...] [-DCMAKE_MAKE_PROGRAM=<ninja>]
 * -D<cache variables>... -DCMAKE_EXPORT_COMPILE_COMMANDS=ON (always last, so it wins). The
 * presets are resolved natively and passed as these switches rather than with --preset, so what
 * FrostIDE shows and checks is exactly what runs, whatever CMake version is installed.
 * cmakeBuildArguments: --build <build> [--config <c>] [--target <t>...] [-j <n>] [-- <native
 * options>]; Clean is --target clean.
 *
 * Pure functions over plain values; thread-compatible.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/project/cmake_presets.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::project {

enum class CMakeProfileSource : std::uint8_t {
    Default,             // The built-in Debug profile.
    ProjectSettings,     // A `cmake.profiles` record from a project-scope settings file.
    ApplicationSettings, // A `cmake.profiles` record from the application settings.
    Preset,              // A configure preset.
};

// "default", "project settings", "application settings" or "preset".
[[nodiscard]] std::string_view toString(CMakeProfileSource source) noexcept;

struct CMakeProfile {
    std::string name;          // Unique among a folder's profiles ("Debug", a preset name).
    std::string displayName{}; // For the profile selector.
    CMakeProfileSource source = CMakeProfileSource::Default;
    std::string buildType{};                          // CMAKE_BUILD_TYPE; "" leaves it to the preset or CMake.
    std::string generator{};                          // "Ninja"; "" lets CMake choose.
    std::string buildDirectory{};                     // Absolute, native, normalised.
    std::vector<CMakeCacheVariable> cacheVariables{}; // Extra -D switches, in order.
    CMakeEnvironment environment{};                   // For configure (and builds, see buildEnvironment).
    std::string toolchainFile{};
    std::string installDir{};
    std::string toolset{};
    bool toolsetExternal = false; // The environment selects the toolset, not CMake's generator.
    std::string architecture{};
    bool architectureExternal = false; // The IDE sets up the architecture (MSVC's vcvars on Windows).

    // `cmake --build` settings.
    std::string buildConfiguration{};                   // --config for multi-config generators; "" uses buildType.
    std::vector<std::string> buildTargets{};            // The build preset's targets; empty builds "all".
    int buildJobs = 0;                                  // -j; 0 leaves it to the build tool.
    std::vector<std::string> buildToolOptions{};        // After "--".
    std::optional<CMakeEnvironment> buildEnvironment{}; // A build preset's; nullopt uses `environment`.

    std::string configurePreset{}; // The preset names, for presets.
    std::string buildPreset{};
    std::string problem{}; // Why the profile cannot be used (the trust rule); "" when usable.

    [[nodiscard]] bool usable() const noexcept { return problem.empty(); }
    // The --config value for multi-config generators: buildConfiguration, else buildType, else "Debug".
    [[nodiscard]] std::string effectiveConfiguration() const;

    friend bool operator==(const CMakeProfile&, const CMakeProfile&) = default;
};

// One `cmake.profiles` settings record (frost/settings/keys.hpp, key::cmakeProfiles).
struct CMakeProfileRecord {
    std::string name;
    std::string buildType{};      // Default "Debug".
    std::string generator{};      // Default "Ninja".
    std::string buildDirectory{}; // Absolute, or relative to the folder; default cmake-build-<name lower-cased>.
    std::vector<std::string> cacheVariables{}; // "NAME=VALUE" or "NAME:TYPE=VALUE".
    std::vector<std::string> environment{};    // "NAME=VALUE"; "NAME" alone unsets it.
    std::vector<std::string> buildOptions{};   // Native build tool options.
    int jobs = 0;
    std::string root{}; // The folder the record applies to (absolute); "" applies to every folder.
};

// The default profile for `sourceDir` (absolute, normalised): see the header comment.
[[nodiscard]] CMakeProfile defaultCMakeProfile(std::string_view sourceDir);

// The profile of configure preset `preset` built with `buildPreset` (may be null).
[[nodiscard]] CMakeProfile cmakeProfileFromPreset(std::string_view sourceDir, const CMakeConfigurePreset& preset,
                                                  const CMakeBuildPreset* buildPreset);

// The profiles of `records` that apply to `sourceDir`, read from settings scope `source`
// (ProjectSettings or ApplicationSettings). Records with an empty or duplicate name are skipped.
[[nodiscard]] std::vector<CMakeProfile> cmakeProfilesFromRecords(std::string_view sourceDir,
                                                                 std::span<const CMakeProfileRecord> records,
                                                                 CMakeProfileSource source);

// The folder's profiles (header comment, "Choice"); never empty.
[[nodiscard]] std::vector<CMakeProfile> cmakeProfilesFor(std::string_view sourceDir,
                                                         const std::optional<CMakePresets>& presets,
                                                         std::span<const CMakeProfileRecord> records,
                                                         CMakeProfileSource recordSource);

// Whether `generator` is a multi-config one ("Ninja Multi-Config", "Visual Studio ...", "Xcode").
[[nodiscard]] bool isMultiConfigGenerator(std::string_view generator) noexcept;
// Whether `generator` is a Ninja generator.
[[nodiscard]] bool isNinjaGenerator(std::string_view generator) noexcept;
// Whether `generator` is a Visual Studio generator (it finds MSVC itself, without vcvars).
[[nodiscard]] bool isVisualStudioGenerator(std::string_view generator) noexcept;

// Whether `path` is `root` or lies below it (both absolute and collapsed with collapseCMakePath
// first; ASCII case ignored on Windows), so "<root>/../x" is outside on every OS.
[[nodiscard]] bool isPathWithin(std::string_view root, std::string_view path);

/*
 * `path` (absolute) normalised (io::normalizePath) with ".." resolved lexically, stopping at the
 * root, on every OS: what CMake does to the paths it is given (cmSystemTools::CollapseFullPath).
 * io::normalizePath keeps ".." on POSIX; CMake's paths follow CMake's rule. "" when not absolute.
 */
[[nodiscard]] std::string collapseCMakePath(std::string_view path);

/*
 * The arguments of the configure run (header comment). `makeProgram` (absolute) becomes
 * CMAKE_MAKE_PROGRAM for Ninja generators when the profile does not set it; "" leaves it out.
 * Errors: InvalidArgument (an unusable profile, a relative path).
 */
[[nodiscard]] Result<std::vector<std::string>>
cmakeConfigureArguments(const CMakeProfile& profile, std::string_view sourceDir, std::string_view makeProgram);

// The arguments of one `cmake --build` run: build `targets` (empty: the profile's buildTargets,
// else all), or clean when `clean` is set. `jobs` > 0 overrides the profile's.
[[nodiscard]] std::vector<std::string>
cmakeBuildArguments(const CMakeProfile& profile, std::span<const std::string> targets, bool clean, int jobs = 0);

} // namespace frost::project
