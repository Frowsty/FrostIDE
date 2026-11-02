// SPDX-License-Identifier: Apache-2.0
//
// Static readers for .vcxproj and .vcxproj.filters files (PLAN.md section 2 "MSBuild", W1 in
// ADR 0032): the properties, configurations, items and references a Solution Explorer needs.
//
/*
 * Static reading only. Imported .props and .targets files are not read and nothing is
 * evaluated, so items or properties that imports add are missing: path (c)'s MSBuild evaluation
 * (ProjectHost, M2) closes that gap. Conditions are not evaluated either: every PropertyGroup
 * and ItemGroup is read whatever its Condition, including those inside <Choose>/<When>/
 * <Otherwise>, and each file is kept once (the first item that names it wins).
 *
 * Properties. ProjectGuid, RootNamespace, ConfigurationType and PlatformToolset keep the first
 * value in document order. A PropertyGroup whose Condition is the standard
 * '$(Configuration)|$(Platform)'=='Debug|x64' form also sets ConfigurationType and
 * PlatformToolset of that ProjectConfiguration. Properties defined without any Condition (on the
 * group or the property) form the static property table used for $(Name) expansion, together
 * with MSBuild's reserved ones (MSBuildProjectDirectory, MSBuildThisFileDirectory,
 * MSBuildProjectName, MSBuildProjectFile, MSBuildProjectFullPath, ProjectDir, ProjectName,
 * ProjectFileName, ProjectPath) and SolutionDir when the caller knows the solution. A name the
 * table does not define is looked up in the environment next, as MSBuild does (its properties
 * start as the environment's variables, which project properties override), so an SDK-style
 * "$(MY_SDK_DIR)\include\sdk.h" resolves; the environment is the process's unless
 * VcxprojOptions::environment replaces it.
 *
 * Items. ClCompile, ClInclude, None, ResourceCompile, Text, CustomBuild, Image and Natvis, plus
 * MASM, Midl, FxCompile, Manifest, CopyFileToFolders and Xml as ItemType::Other. Include may list
 * several paths separated by ';'. $(Name) references are expanded from the table above; an
 * Include that still names an unknown property, an item list @(...) or metadata %(...) is
 * skipped and reported in `unresolvedIncludes`. Paths are resolved against the project's
 * directory with MSBuild's rules ('\' and '/' separate; ".." is resolved lexically on every
 * platform). Wildcards: '*' and '?' within one name and "**" for any number of directories,
 * matched case-insensitively on Windows; the matches are sorted. Symlinked files match, symlinked
 * directories are not entered. A recursive wildcard directly under a file-system root is refused
 * with a warning, and an expansion stops after 20,000 directories or 100,000 files. Exclude
 * (the same syntax) removes matches. Remove and Update are not supported yet.
 *
 * Errors are Corrupt for malformed XML or a root element other than <Project>; everything else
 * becomes a warning, so one odd item never hides a project. Names of elements are matched
 * case-insensitively, as MSBuild does. Thread-safe (no shared state); the cost is O(file size)
 * plus the directories a wildcard walks.
 */
#pragma once

#include "frost/core/error.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::project {

enum class ItemType : unsigned char {
    ClCompile,
    ClInclude,
    None,
    ResourceCompile,
    Text,
    CustomBuild,
    Image,
    Natvis,
    Other, // A file item of another known type; ProjectItem::itemName says which.
};

// The MSBuild item name ("ClCompile"); "Other" for ItemType::Other.
[[nodiscard]] std::string_view toString(ItemType type) noexcept;

// One file item.
struct ProjectItem {
    ItemType type = ItemType::None;
    std::string itemName; // The element name as written ("ClCompile", "MASM").
    std::string include;  // The Include entry this item came from, as written ("src\**\*.cpp").
    std::string path;     // Absolute and normalised.
    bool fromWildcard = false;
};

// One <ProjectReference>.
struct ProjectReference {
    std::string include;     // As written.
    std::string path;        // Absolute and normalised.
    std::string projectGuid; // The <Project> metadata, upper-case with braces; "" when absent.
};

// One <ProjectConfiguration Include="Debug|x64">.
struct ProjectConfiguration {
    std::string configuration;     // "Debug"
    std::string platform;          // "x64", "Win32"
    std::string configurationType; // From a PropertyGroup conditioned on this pair; "" if none.
    std::string platformToolset;   // Likewise.

    // "Debug|x64".
    [[nodiscard]] std::string name() const { return configuration + '|' + platform; }
};

struct VcxProject {
    std::string path;              // Absolute and normalised.
    std::string directory;         // parentPath(path).
    std::string projectGuid;       // Upper-case with braces; "" when absent.
    std::string rootNamespace;     // "" when absent.
    std::string configurationType; // First value: "Application", "StaticLibrary", "DynamicLibrary", ...
    std::string platformToolset;   // First value: "v143", "ClangCL", ...
    std::vector<ProjectConfiguration> configurations; // Document order, without duplicates.
    std::vector<ProjectItem> items;                   // Document order, each path once.
    std::vector<ProjectReference> references;         // Document order, each path once.
    // The static property table: lower-case names to expanded values (see above).
    std::map<std::string, std::string, std::less<>> properties;
    std::vector<std::string> unresolvedIncludes; // Include entries skipped for unknown references.
    std::vector<std::string> warnings;           // Non-fatal problems, for the log.
};

// Looks up one environment variable by name; nullopt when it is not set.
using EnvironmentLookup = std::function<std::optional<std::string>(std::string_view name)>;

// The process environment (GetEnvironmentVariableW on Windows, getenv on POSIX); thread-safe as
// long as nothing changes the environment meanwhile. Names are case-insensitive on Windows only.
[[nodiscard]] std::optional<std::string> processEnvironmentVariable(std::string_view name);

struct VcxprojOptions {
    // The solution's directory, for $(SolutionDir) (absolute, normalised); "" leaves it undefined.
    std::string solutionDirectory;
    // Where $(Name) references the property table does not define are looked up (see above);
    // empty uses processEnvironmentVariable. Values are used literally, never expanded again.
    EnvironmentLookup environment{};
};

// One <Filter Include="Source Files\Core"> of a .filters file.
struct FilterDefinition {
    std::string path; // '\'-separated, without leading, trailing or repeated separators.
    std::string uniqueIdentifier;
    std::string extensions;
};

// An item of the .filters file and the filter it is shown in.
struct FilterAssignment {
    std::string itemPath; // Absolute and normalised, resolved like the project's items.
    std::string filter;   // A FilterDefinition::path; "" puts the item at the project root.
};

struct FiltersFile {
    std::vector<FilterDefinition> filters;     // Document order, without duplicates.
    std::vector<FilterAssignment> assignments; // Document order, each path once.
    std::vector<std::string> warnings;
};

// Parses .vcxproj XML read from `projectPath` (absolute). `fs` lists directories for wildcard
// Includes. Errors: InvalidArgument (projectPath), Corrupt (malformed XML, root not <Project>).
[[nodiscard]] Result<VcxProject> parseVcxproj(io::IFileSystem& fs, std::string_view content,
                                              std::string_view projectPath, const VcxprojOptions& options = {});

// Reads and parses `projectPath`. Errors: those of IFileSystem::readFile and parseVcxproj.
[[nodiscard]] Result<VcxProject> readVcxproj(io::IFileSystem& fs, std::string_view projectPath,
                                             const VcxprojOptions& options = {});

/*
 * The raw (unexpanded) values of the properties an MSBuild project file (a .vcxproj or its
 * .vcxproj.user) sets for `configuration`|`platform`: those of PropertyGroups without a Condition
 * or with the standard '$(Configuration)|$(Platform)'=='Debug|x64' form naming that pair (ASCII
 * case ignored), in document order, a later value replacing an earlier one; properties with a
 * Condition of their own, other conditions and <Choose> are skipped. Keys are lower-case. For the
 * run service's fallback when MSBuild cannot report properties (frost/build/run_target.hpp).
 * Errors: Corrupt (malformed XML, root not <Project>). O(file size).
 */
[[nodiscard]] Result<std::map<std::string, std::string, std::less<>>>
parseConfigurationProperties(std::string_view content, std::string_view path, std::string_view configuration,
                             std::string_view platform);

// Parses the .vcxproj.filters XML that belongs to `project`; Include paths resolve against the
// project's directory with its property table, then options.environment. Errors: Corrupt
// (malformed XML, root not <Project>).
[[nodiscard]] Result<FiltersFile> parseVcxprojFilters(io::IFileSystem& fs, std::string_view content,
                                                      const VcxProject& project, const VcxprojOptions& options = {});

// Reads "<project.path>.filters" and parses it. A missing file is an empty FiltersFile, not an
// error. Errors: those of IFileSystem::readFile other than NotFound, and parseVcxprojFilters.
[[nodiscard]] Result<FiltersFile> readVcxprojFilters(io::IFileSystem& fs, const VcxProject& project,
                                                     const VcxprojOptions& options = {});

} // namespace frost::project
