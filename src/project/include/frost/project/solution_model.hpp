// SPDX-License-Identifier: Apache-2.0
//
// SolutionModel: an opened .sln or .slnx as the Solution Explorer shows it (W1, ADR 0032):
// solution -> solution folders -> projects -> filters -> files, plus configurations, the
// per-project configuration mapping, file -> project lookup and project dependencies.
//
/*
 * Loading (SolutionModel::load) reads the solution with the static readers of solution_file.hpp,
 * then every .vcxproj and its .vcxproj.filters with those of vcxproj.hpp. Nothing is evaluated
 * or launched: imported .props/.targets are not read (path (c)'s MSBuild evaluation is M2), so
 * the model is safe to build before the folder is trusted (PLAN.md section 3.5).
 *
 * Only a problem with the solution file itself fails the load. A project file that is missing
 * or unreadable becomes an UnavailableProject node carrying the error; .csproj, .vbproj,
 * .fsproj, .vcxitems, .shproj and unknown project types become UnsupportedProject nodes ("not
 * supported yet"); a missing .filters file puts every file at the project root, an unreadable
 * one does the same with a warning. Warnings collects everything non-fatal for the log.
 *
 * Tree. Node 0 is the solution. Its children are solution folders and projects; a folder holds
 * folders, projects and solution-item files; a loaded C++ project holds its filter tree (empty
 * filters included) and files; files without a filter sit at the project root. Children are
 * sorted folders/filters first, then projects, then files, each by display name (ASCII case
 * ignored). A NestedProjects entry that names an unknown folder, or would make a cycle, leaves
 * the child at the solution root with a warning.
 *
 * Ids. NodeId and ProjectId are dense indices, fixed for the model's lifetime. Node::key is
 * stable across reloads of the same files (it is derived from paths, folder GUIDs or names and
 * filter paths, never from load order), so a tree view can restore expansion and selection after
 * a reload; findNode(key) maps it back.
 *
 * Configurations. .sln: each project's ProjectConfigurationPlatforms entries. .slnx: each
 * solution configuration maps to the project's configuration of the same name and the matching
 * platform (x86 -> Win32 and "Any CPU" -> x64 when the project has no such platform; else its
 * first one), built and not deployed, and then the project's rules apply in document order.
 *
 * Dependencies. A project depends on what its ProjectReference items name (matched by path, then
 * by the <Project> GUID) and on the solution's ProjectDependencies (.sln) or BuildDependency
 * (.slnx) entries. References to projects outside the solution are listed per project.
 *
 * Paths. Lookups take absolute paths and compare them normalised, with ASCII case ignored on
 * Windows. Thread safety: a loaded model is immutable; every const member may be called from any
 * thread. load() is blocking (file reads, wildcard walks) and belongs on a worker thread.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/strong_type.hpp"
#include "frost/project/solution_file.hpp"
#include "frost/project/vcxproj.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::project {

struct NodeIdTag {};
struct ProjectIdTag {};
// A node of the solution tree; NodeId{0} is the solution.
using NodeId = StrongIndex<NodeIdTag, std::uint32_t>;
// A project of the solution, in solution file order.
using ProjectId = StrongIndex<ProjectIdTag, std::uint32_t>;

enum class NodeKind : unsigned char {
    Solution,
    SolutionFolder,
    Project,            // A loaded C++ project (.vcxproj).
    UnsupportedProject, // A project type FrostIDE cannot open yet; shown, not loaded.
    UnavailableProject, // A project whose file is missing or unreadable; ProjectInfo::error says why.
    Filter,             // A .vcxproj.filters folder.
    File,               // A project item, or a solution item when `project` is empty.
};

enum class ProjectKind : unsigned char { Cpp, CSharp, VisualBasic, FSharp, SharedItems, Unknown };

// "C++", "C#", "Visual Basic", "F#", "shared items", "unknown".
[[nodiscard]] std::string_view toString(ProjectKind kind) noexcept;

struct Node {
    NodeKind kind = NodeKind::File;
    std::optional<NodeId> parent;       // Empty for the solution.
    std::vector<NodeId> children;       // Sorted as described above.
    std::string displayName;            // The name to show; the file name for files.
    std::string path;                   // Absolute for the solution, projects and files; "" for folders and filters.
    std::string key;                    // Stable across reloads (see above); unique in the model.
    std::optional<ProjectId> project;   // The project of project, filter and project-file nodes.
    ItemType itemType = ItemType::None; // For project files.
};

struct ProjectInfo {
    std::string name;        // Display name from the solution.
    std::string path;        // Absolute; "" when the solution's path could not be resolved.
    std::string projectGuid; // From the solution, else from the .vcxproj; may be "".
    std::string typeGuid;    // From the solution; may be "".
    ProjectKind kind = ProjectKind::Unknown;
    NodeKind nodeKind = NodeKind::UnsupportedProject; // Project, UnsupportedProject or UnavailableProject.
    NodeId node{};
    std::optional<Error> error;                  // Why an UnavailableProject could not be read.
    std::optional<VcxProject> vcxproj;           // The parsed project, for loaded C++ projects.
    std::vector<ProjectId> dependencies;         // Projects this one builds after, without duplicates.
    std::vector<std::string> externalReferences; // ProjectReference paths naming no project in the solution.
    // Index = solution configuration index; empty when the project is not part of that configuration.
    std::vector<std::optional<ProjectConfigurationMapping>> configurationMap;
};

class SolutionModel {
public:
    /*
     * Loads `solutionPath` (absolute, .sln or .slnx) and every project it lists (see above).
     * Errors: those of readSolutionFile (InvalidArgument, NotFound, PermissionDenied, IoError,
     * Corrupt, Unsupported); project problems never fail the load.
     */
    [[nodiscard]] static Result<SolutionModel> load(io::IFileSystem& fs, std::string_view solutionPath);

    [[nodiscard]] SolutionFormat format() const noexcept { return m_format; }
    [[nodiscard]] const std::string& path() const noexcept { return m_path; }
    [[nodiscard]] const std::string& directory() const noexcept { return m_directory; }
    // The solution's name: its file name without the extension.
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }

    [[nodiscard]] static constexpr NodeId root() noexcept { return NodeId{0}; }
    [[nodiscard]] std::size_t nodeCount() const noexcept { return m_nodes.size(); }
    // Precondition (FROST_PRE): id < nodeCount().
    [[nodiscard]] const Node& node(NodeId id) const;
    [[nodiscard]] std::span<const NodeId> children(NodeId id) const { return node(id).children; }
    [[nodiscard]] std::optional<NodeId> parent(NodeId id) const { return node(id).parent; }
    // The node with this Node::key, if any. O(1).
    [[nodiscard]] std::optional<NodeId> findNode(std::string_view key) const;

    [[nodiscard]] std::span<const ProjectInfo> projects() const noexcept { return m_projects; }
    // Precondition (FROST_PRE): id < projects().size().
    [[nodiscard]] const ProjectInfo& project(ProjectId id) const;
    [[nodiscard]] std::optional<ProjectId> findProjectByPath(std::string_view path) const;
    // `guid` in any case, with or without braces.
    [[nodiscard]] std::optional<ProjectId> findProjectByGuid(std::string_view guid) const;

    [[nodiscard]] std::span<const SolutionConfiguration> configurations() const noexcept { return m_configurations; }
    // The index of "Debug|x64" (ASCII case ignored), if the solution has it.
    [[nodiscard]] std::optional<std::size_t> findConfiguration(std::string_view name) const;
    // What `project` builds as in configurations()[configurationIndex]; empty when the project does
    // not take part. Precondition (FROST_PRE): both indices are in range.
    [[nodiscard]] const std::optional<ProjectConfigurationMapping>&
    projectConfiguration(ProjectId project, std::size_t configurationIndex) const;

    // Every project with an item at `filePath`, in project order. O(1) plus the result.
    [[nodiscard]] std::vector<ProjectId> projectsContaining(std::string_view filePath) const;
    // The first of projectsContaining(filePath), if any.
    [[nodiscard]] std::optional<ProjectId> owningProject(std::string_view filePath) const;
    // The first file node (project item or solution item) at `filePath`, for "select in Solution Explorer".
    [[nodiscard]] std::optional<NodeId> findFileNode(std::string_view filePath) const;

    [[nodiscard]] std::span<const std::string> warnings() const noexcept { return m_warnings; }

private:
    friend class SolutionModelBuilder;
    SolutionModel() = default;

    SolutionFormat m_format = SolutionFormat::Sln;
    std::string m_path;
    std::string m_directory;
    std::string m_name;
    std::vector<Node> m_nodes;
    std::vector<ProjectInfo> m_projects;
    std::vector<SolutionConfiguration> m_configurations;
    std::vector<std::string> m_warnings;
    std::unordered_map<std::string, NodeId> m_nodesByKey;
    std::unordered_map<std::string, ProjectId> m_projectsByPath; // Path key -> project.
    std::unordered_map<std::string, ProjectId> m_projectsByGuid;
    // Path key -> the file nodes at that path, in creation order.
    std::unordered_map<std::string, std::vector<NodeId>> m_filesByPath;
};

} // namespace frost::project
