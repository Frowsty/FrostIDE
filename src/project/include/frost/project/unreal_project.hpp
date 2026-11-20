// SPDX-License-Identifier: Apache-2.0
// Independent public descriptor metadata, ADR0114. No engine code is loaded or evaluated.
#pragma once
#include "frost/core/error.hpp"
#include <string>
#include <string_view>
#include <vector>
namespace frost::project {
struct UnrealModule { std::string name; std::string type; std::string loadingPhase; };
struct UnrealPluginReference { std::string name; bool enabled = false; bool optional = false; };
struct UnrealProject {
    std::string projectFile;
    std::string engineAssociation;
    std::string description;
    std::vector<UnrealModule> modules;
    std::vector<UnrealPluginReference> plugins;
};
struct UnrealPluginDescriptor { std::string friendlyName; std::vector<UnrealModule> modules; };
struct UnrealInstallation { std::string association; std::string root; };
// Pure, bounded parsers; unknown fields are retained by the user's original file, never rewritten.
// Known fields have strict types and duplicate JSON keys/names are refused (Corrupt).
// Unsupported descriptor versions return Unsupported. No Pre/PostBuildSteps are interpreted.
[[nodiscard]] Result<UnrealProject> parseUnrealProject(std::string_view json, std::string_view absoluteProject);
[[nodiscard]] Result<UnrealPluginDescriptor> parseUnrealPluginDescriptor(std::string_view json);
// Exact association lookup in user-supplied installed metadata. Missing/ambiguous associations
// never guess a version or inspect an engine. An empty association requires explicit selection.
[[nodiscard]] Result<UnrealInstallation> resolveUnrealInstallation(const UnrealProject& project,
    const std::vector<UnrealInstallation>& installations);
} // namespace frost::project
