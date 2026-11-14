// SPDX-License-Identifier: Apache-2.0
//
// SolutionModel: an opened .sln or .slnx as the Solution Explorer shows it (W1, ADR 0032):
// solution -> solution folders -> projects -> filters -> files, plus configurations, the
// per-project configuration mapping, file -> project lookup and project dependencies.
//
/*
 * Loading (SolutionModel::load) reads the solution with the static readers of solution_file.hpp,
 * then every .vcxproj and its .vcxproj.filters with those of vcxproj.hpp. Nothing is evaluated
 * or launched: imported .props/.targets are not read, so the model is safe to build before the
 * folder is trusted (PLAN.md section 3.5).
 *
 * Evaluated items (SLN-3). SolutionLoadOptions::evaluated carries MSBuild's own evaluation of
 * projects (ProjectHost, or MSBuild.exe as its fallback; project_host.hpp). A loaded C++ project
 * with an entry there gets its file nodes from the evaluated items instead of the static ones:
 * items that property sheets, Directory.Build.props and conditions add or remove are then shown
 * exactly as MSBuild sees them. The .filters file still places them (an evaluated item the
 * .filters file does not name goes to the project root, or to its folder in the folder modes),
 * and ProjectInfo::evaluation records the source, configuration and imports. Loading itself
 * still launches nothing: the caller ran the evaluation after trust.
 *
 * Only a problem with the solution file itself fails the load. A project file that is missing
 * or unreadable becomes an UnavailableProject node carrying the error; .vbproj, .fsproj,
 * .vcxitems, .shproj and unknown project types become UnsupportedProject nodes ("not
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
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
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
    Project,            // A loaded C++ (.vcxproj) or C# (.csproj) project.
    UnsupportedProject, // A project type FrostIDE cannot open yet; shown, not loaded.
    UnavailableProject, // A project whose file is missing or unreadable; ProjectInfo::error says why.
    Filter,             // A .vcxproj.filters folder or physical C# project subfolder.
    File,               // A project item, or a solution item when `project` is empty.
};

enum class ProjectKind : unsigned char { Cpp, CSharp, VisualBasic, FSharp, SharedItems, Unknown };

enum class CppHierarchyMode : unsigned char {
    Filters,           // .vcxproj.filters tree only (Visual Studio default).
    FiltersAndFolders, // .vcxproj.filters tree, plus physical subfolders for unfiltered items.
    PhysicalFolders,   // Physical folder structure relative to the .vcxproj directory.
};

// Who evaluated a project's items (SLN-2): the ProjectHost helper, or MSBuild.exe as its fallback.
enum class EvaluationSource : unsigned char { ProjectHost, MsBuildCli };

// "ProjectHost", "MSBuild.exe".
[[nodiscard]] std::string_view toString(EvaluationSource source) noexcept;

// MSBuild's evaluated file items of one project in one project configuration (see above).
struct EvaluatedProjectItems {
    std::string projectPath;   // Absolute.
    std::string configuration; // The project configuration evaluated, "Debug".
    std::string platform;      // The project platform evaluated, "x64".
    EvaluationSource source = EvaluationSource::ProjectHost;
    std::string msbuildVersion = {};        // The evaluating Microsoft.Build's file version, or "".
    std::vector<ProjectItem> items = {};    // Evaluation order, each path once, absolute and normalised.
    std::vector<std::string> imports = {};  // The imports in evaluation order; empty for MsBuildCli.
    std::vector<std::string> warnings = {}; // Non-fatal problems of the evaluation, for the log.
};

// What a project's node was built from when it was evaluated (ProjectInfo::evaluation).
struct ProjectEvaluationSummary {
    EvaluationSource source = EvaluationSource::ProjectHost;
    std::string configuration;
    std::string platform;
    std::string msbuildVersion;
    std::vector<std::string> imports;
    std::size_t itemCount = 0; // Distinct item paths.
};

// NativeStatic is a literal Windows-reader model, not evaluated MSBuild authority. Linux defaults
// to an explicitly approximate, read-only project model; source text remains ordinary editable text.
enum class SolutionModelMode { NativeStatic, ApproximateReadOnly };
struct SolutionLoadProgress {
    std::size_t completedProjects = 0;
    std::size_t totalProjects = 0;
    std::string projectPath;
};
struct SolutionLoadProfile {
    std::size_t projects = 0;
    std::size_t cppProjectsRead = 0;
    std::size_t managedProjectsRead = 0;
    std::size_t managedDirectoryLists = 0; // Default C# walk calls, not all filesystem operations.
    std::size_t nodes = 0;
};

struct SolutionLoadOptions {
    CppHierarchyMode cppHierarchy = CppHierarchyMode::Filters;
    std::vector<std::string> unloadedProjectPaths = {};
    // Evaluated items per project (see the header comment); projects not listed stay static.
    std::vector<EvaluatedProjectItems> evaluated = {};
#ifdef _WIN32
    SolutionModelMode mode = SolutionModelMode::NativeStatic;
#else
    SolutionModelMode mode = SolutionModelMode::ApproximateReadOnly;
#endif
    std::stop_token stop;
    // Worker-thread progress, never an estimate of time remaining. Must not throw or reenter load.
    std::function<void(const SolutionLoadProgress&)> progress;
};

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
    std::string filterPath;             // Logical filter or relative folder path for Filter nodes.
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
    // Set when the project's items came from MSBuild's evaluation (SolutionLoadOptions::evaluated).
    std::optional<ProjectEvaluationSummary> evaluation = std::nullopt;
};

class SolutionModel {
public:
    /*
     * Loads `solutionPath` (absolute, .sln or .slnx) and every project it lists (see above).
     * Errors: those of readSolutionFile (InvalidArgument, NotFound, PermissionDenied, IoError,
     * Corrupt, Unsupported); project problems never fail the load.
     */
    [[nodiscard]] static Result<SolutionModel> load(io::IFileSystem& fs, std::string_view solutionPath,
                                                    SolutionLoadOptions options = {});

    [[nodiscard]] SolutionModelMode mode() const noexcept { return m_mode; }
    [[nodiscard]] bool approximate() const noexcept { return m_mode == SolutionModelMode::ApproximateReadOnly; }
    [[nodiscard]] const SolutionLoadProfile& loadProfile() const noexcept { return m_loadProfile; }
    // Model edits/execution are unsupported for an approximate solution even after trust. Other
    // services still apply their own trust/tool checks to native static models.
    [[nodiscard]] Result<void> requireProjectMutation() const;
    [[nodiscard]] Result<void> requireExecution() const;
    [[nodiscard]] std::string_view approximationNotice() const noexcept;

    // Pure worker-thread overlay for one successfully evaluated project's item type. Unlisted
    // nodes are disconnected; returning items regain their original hierarchy and indices.
    // New items follow the loaded filter/folder policy; saved layout never grants membership.
    // Other projects/types remain unchanged. O(model size + changed sibling sorting), no file IO.
    [[nodiscard]] SolutionModel withEvaluatedFiles(ProjectId project, ItemType type,
                                                   std::span<const std::string> files) const;

    [[nodiscard]] SolutionFormat format() const noexcept { return m_format; }
    [[nodiscard]] CppHierarchyMode cppHierarchyMode() const noexcept { return m_cppHierarchy; }
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

    // The distinct paths of the file nodes reachable below `project`'s node: the item count the
    // Solution Explorer shows for it (SLN-3's oracle comparison). O(nodes below the project).
    // Precondition (FROST_PRE): project < projects().size().
    [[nodiscard]] std::vector<std::string> projectFilePaths(ProjectId project) const;

private:
    friend class SolutionModelBuilder;
    SolutionModel() = default;

    SolutionModelMode m_mode = SolutionModelMode::NativeStatic;
    SolutionLoadProfile m_loadProfile;
    SolutionFormat m_format = SolutionFormat::Sln;
    CppHierarchyMode m_cppHierarchy = CppHierarchyMode::Filters;
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
    // Resolved .filters layout only, never membership. Shared immutable receipts keep worker
    // overlays independent of the filesystem without copying every assignment on each publish.
    std::unordered_map<std::uint32_t, std::shared_ptr<const std::unordered_map<std::string, std::string>>>
        m_cppFilterAssignments;
};

} // namespace frost::project
