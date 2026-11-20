// SPDX-License-Identifier: Apache-2.0
// Independent public descriptor metadata, ADR0114. No engine code is loaded or evaluated.
#pragma once
#include "frost/core/error.hpp"
#include <string>
#include <string_view>
#include <vector>
namespace frost::io { class IFileSystem; }
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
struct UnrealRuleFile { std::string name; std::string path; std::string owner; };
struct UnrealLocalPlugin {
    std::string name;
    std::string descriptorFile;
    UnrealPluginDescriptor descriptor;
};
struct UnrealWorkspace {
    UnrealProject project;
    std::vector<UnrealRuleFile> targetRules;
    std::vector<UnrealRuleFile> moduleRules;
    std::vector<UnrealLocalPlugin> localPlugins;
};
// Worker-only, bounded project-local Source/Plugins metadata inventory. Never reads rule contents,
// evaluates C#, follows directory links or searches installed engine/plugin directories.
// Targets are rule-file names, not evaluated target capabilities/configurations.
[[nodiscard]] Result<UnrealWorkspace> readUnrealWorkspace(io::IFileSystem& fs, std::string_view absoluteProject);
struct UnrealInstallation { std::string association; std::string root; };
// Pure, bounded parsers; unknown fields are retained by the user's original file, never rewritten.
// Known fields have strict types and duplicate JSON keys/names are refused (Corrupt).
// Unsupported descriptor versions return Unsupported. No Pre/PostBuildSteps are interpreted.
// Bounded retained regular-file read; blocking, worker-only. No whole-file fallback.
[[nodiscard]] Result<UnrealProject> readUnrealProject(io::IFileSystem& fs, std::string_view absoluteProject);
[[nodiscard]] Result<UnrealProject> parseUnrealProject(std::string_view json, std::string_view absoluteProject);
[[nodiscard]] Result<UnrealPluginDescriptor> parseUnrealPluginDescriptor(std::string_view json);
// Exact association lookup in user-supplied installed metadata. Missing/ambiguous associations
// never guess a version or inspect an engine. An empty association requires explicit selection.
[[nodiscard]] Result<UnrealInstallation> resolveUnrealInstallation(const UnrealProject& project,
    const std::vector<UnrealInstallation>& installations);
} // namespace frost::project
