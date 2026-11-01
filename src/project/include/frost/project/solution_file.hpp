// SPDX-License-Identifier: Apache-2.0
//
// SolutionFile: what the static .sln and .slnx readers extract from a Visual Studio solution
// (PLAN.md section 2 ".sln / .slnx", W1 in ADR 0032). SolutionModel (solution_model.hpp) builds
// the Solution Explorer tree from it; most callers want that, not these readers.
//
/*
 * Static reading only. The readers parse the file's own text and never run MSBuild: path (c)'s
 * MSBuild evaluation is M2. Nothing here launches a process, so reading is safe in restricted
 * mode (PLAN.md section 3.5).
 *
 * .sln (parseSln): the "Format Version 12.00" text format (earlier versions with the same syntax
 * are accepted too). UTF-8 with or without a BOM, UTF-16 with a BOM, CRLF or LF line ends. Read:
 * Project(...) entries (type GUID, name, path, project GUID), their ProjectDependencies and
 * SolutionItems sections, and the global sections SolutionConfigurationPlatforms,
 * ProjectConfigurationPlatforms (ActiveCfg, Build.0, Deploy.0) and NestedProjects. Other sections
 * are skipped. A missing header line is Corrupt; an unterminated section or a NestedProjects line
 * naming an unknown GUID only adds a warning, as Visual Studio is lenient there too.
 *
 * .slnx (parseSlnx): the XML format (root <Solution>). Read: <Configurations> (<BuildType Name>,
 * <Platform Name>; solution configurations are their cross product, build types outer), <Folder
 * Name="/a/b/"> (the name is the full folder path, so "/a/b/" nests under "/a/", which is created
 * when missing), <File Path> solution items, <Project Path Type DisplayName Id> with
 * <BuildDependency Project> and the per-project rules <BuildType>, <Platform>, <Build> and
 * <Deploy> (Solution="Debug|*" patterns, Project=value). Without <Configurations>, the build
 * types are Debug and Release and the platform is "Any CPU". Element and attribute names are
 * matched case-insensitively. Malformed XML or another root element is Corrupt.
 *
 * Paths: relative paths are resolved against the solution's directory with MSBuild's rules
 * ('\' and '/' both separate; ".." is resolved lexically on every platform, like
 * Path.GetFullPath). GUIDs are stored upper-case with braces.
 *
 * Every function is pure apart from readSolutionFile's one read, thread-safe, and O(size of the
 * file).
 */
#pragma once

#include "frost/core/error.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::project {

enum class SolutionFormat : unsigned char { Sln, Slnx };

// Project type GUIDs from the .sln format, upper-case with braces.
inline constexpr std::string_view kSolutionFolderTypeGuid = "{2150E333-8FDC-42A3-9474-1A3956D46DE8}";
inline constexpr std::string_view kCppProjectTypeGuid = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}";

// One solution configuration such as Debug|x64.
struct SolutionConfiguration {
    std::string configuration; // "Debug"
    std::string platform;      // "x64", "Any CPU"

    // "Debug|x64".
    [[nodiscard]] std::string name() const { return configuration + '|' + platform; }

    friend bool operator==(const SolutionConfiguration&, const SolutionConfiguration&) = default;
};

// What a project builds as in one solution configuration.
struct ProjectConfigurationMapping {
    std::string configuration; // The project's configuration, "Debug".
    std::string platform;      // The project's platform, "Win32".
    bool build = false;        // .sln: a Build.0 entry exists; .slnx: not switched off by a <Build> rule.
    bool deploy = false;       // .sln: a Deploy.0 entry exists; .slnx: switched on by a <Deploy> rule.

    friend bool operator==(const ProjectConfigurationMapping&, const ProjectConfigurationMapping&) = default;
};

// A .slnx per-project rule: <BuildType|Platform|Build|Deploy Solution="pattern" Project="value"/>.
enum class ConfigurationRuleKind : unsigned char { BuildType, Platform, Build, Deploy };

struct ConfigurationRule {
    ConfigurationRuleKind kind = ConfigurationRuleKind::BuildType;
    // "Debug|x64", "Debug|*", "*|x64" or "Debug" (meaning "Debug|*"); empty matches every configuration.
    std::string solutionPattern;
    std::string projectValue; // A configuration, a platform, or "true"/"false".

    friend bool operator==(const ConfigurationRule&, const ConfigurationRule&) = default;
};

// A solution folder (a virtual folder in the Solution Explorer).
struct SolutionFolderEntry {
    std::string id;       // .sln: the folder's GUID; .slnx: its full name, "/Libraries/Core/".
    std::string name;     // The display name, "Core".
    std::string parentId; // The parent folder's id; "" for the solution root.
    // Solution items (files shown in the folder), absolute and normalised, in file order.
    std::vector<std::string> files;
};

// A project listed in the solution (any type except solution folders).
struct SolutionProjectEntry {
    std::string name;         // The display name.
    std::string relativePath; // As written, "Core\Core.vcxproj".
    std::string path;         // Absolute and normalised; "" when relativePath cannot be resolved.
    std::string typeGuid;     // Upper-case with braces; .slnx: the Type attribute's GUID, or "".
    std::string typeName;     // .slnx: a Type attribute that is not a GUID ("C++"); "" otherwise.
    std::string projectGuid;  // Upper-case with braces; "" when the .slnx gives no Id.
    std::string parentId;     // The containing folder's id; "" for the solution root.
    // .sln: GUIDs from ProjectDependencies; .slnx: absolute paths from <BuildDependency Project>.
    std::vector<std::string> dependencies;
    // .sln: the ProjectConfigurationPlatforms entries, keyed by the solution configuration as
    // written ("Debug|x64"), in file order.
    std::vector<std::pair<std::string, ProjectConfigurationMapping>> slnConfigurations;
    // .slnx: the per-project rules in document order (later rules win).
    std::vector<ConfigurationRule> slnxRules;
};

struct SolutionFile {
    SolutionFormat format = SolutionFormat::Sln;
    std::string path;                                  // Absolute and normalised.
    std::string directory;                             // parentPath(path).
    std::string formatVersion;                         // .sln: "12.00"; .slnx: the Version attribute or "".
    std::string visualStudioVersion;                   // .sln: the VisualStudioVersion line, or "".
    std::vector<SolutionConfiguration> configurations; // File order, without duplicates.
    std::vector<SolutionFolderEntry> folders;          // File order.
    std::vector<SolutionProjectEntry> projects;        // File order; solution folders excluded.
    std::vector<std::string> warnings;                 // Non-fatal problems, for the log.
};

// Parses .sln text read from `solutionPath` (absolute; used to resolve relative paths).
// Errors: InvalidArgument (solutionPath), Corrupt (no solution header, or undecodable text).
[[nodiscard]] Result<SolutionFile> parseSln(std::string_view content, std::string_view solutionPath);

// Parses .slnx XML read from `solutionPath`. Errors: InvalidArgument (solutionPath), Corrupt
// (malformed XML, or a root element other than <Solution>).
[[nodiscard]] Result<SolutionFile> parseSlnx(std::string_view content, std::string_view solutionPath);

// Reads the file and parses it by extension (.sln or .slnx, any case). Errors: those of
// IFileSystem::readFile, parseSln and parseSlnx; Unsupported for any other extension.
[[nodiscard]] Result<SolutionFile> readSolutionFile(io::IFileSystem& fs, std::string_view solutionPath);

} // namespace frost::project
