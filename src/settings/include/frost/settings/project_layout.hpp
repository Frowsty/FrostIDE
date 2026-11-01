// SPDX-License-Identifier: Apache-2.0
//
// Where settings live on disk (PLAN.md section 3.6, ADR 0021): the project's .frost/ directory with
// its shared and personal scope files and the .gitignore that keeps the personal scope out of version
// control, and the per-user application settings file.
/*
 * <root>/.frost/
 *   settings.json            project-shared scope (committed)
 *   workspace.json           project-personal scope: personal settings plus other per-user state
 *                            (window layout, selected configuration, breakpoints) kept by other features
 *   .gitignore               written by FrostIDE; lists workspace.json
 *   runConfigurations/       *.json run configurations (M4)
 *   inspectionProfiles/      *.json inspection profiles (M7)
 * Nothing is created when a project is opened: ensureProjectLayout() runs before FrostIDE first
 * writes into .frost/, so browsing a folder never leaves files behind.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace frost::settings {

// The .gitignore FrostIDE writes into .frost/.
inline constexpr std::string_view kFrostGitignoreText =
    "# Written by FrostIDE. workspace.json is the project-personal settings scope (window layout,\n"
    "# selected configuration, breakpoints, personal settings): it stays out of version control.\n"
    "workspace.json\n";

// The paths of one project's .frost/ layout. A plain value.
struct ProjectLayout {
    std::string root;               // The project root, normalised.
    std::string frostDirectory;     // <root>/.frost
    std::string sharedSettings;     // .frost/settings.json
    std::string personalSettings;   // .frost/workspace.json
    std::string gitignore;          // .frost/.gitignore
    std::string runConfigurations;  // .frost/runConfigurations
    std::string inspectionProfiles; // .frost/inspectionProfiles
    io::PathStyle style = io::kNativePathStyle;

    // The layout under `root` (absolute). Errors: InvalidArgument (normalizePath). Pure.
    [[nodiscard]] static Result<ProjectLayout> forRoot(std::string_view root,
                                                       io::PathStyle style = io::kNativePathStyle);
};

/*
 * Creates .frost/ and its .gitignore when missing. An existing .gitignore that does not list
 * workspace.json (a line "workspace.json" or "/workspace.json") gets that line appended; any other
 * content is kept. Errors: createDirectories', readFile's and writeFileAtomic's. Idempotent.
 */
[[nodiscard]] Result<void> ensureProjectLayout(io::IFileSystem& fs, const ProjectLayout& layout);

// Looks an environment variable up: its value, or nullopt when unset.
using EnvironmentLookup = std::function<std::optional<std::string>(std::string_view name)>;

/*
 * The application settings file for an environment: Windows style %APPDATA%\FrostIDE\settings.json;
 * POSIX style $XDG_CONFIG_HOME/frostide/settings.json, else $HOME/.config/frostide/settings.json.
 * Variables that are empty or not absolute are skipped. Errors: NotFound when none applies. Pure.
 */
[[nodiscard]] Result<std::string> applicationSettingsFileFor(const EnvironmentLookup& environment,
                                                             io::PathStyle style = io::kNativePathStyle);

// applicationSettingsFileFor over the process environment (GetEnvironmentVariableW on Windows).
[[nodiscard]] Result<std::string> applicationSettingsFile();

} // namespace frost::settings
