// SPDX-License-Identifier: Apache-2.0
//
// SolutionModel (solution_model.hpp): building the Solution Explorer tree from the static
// readers, and its lookups.
#include "frost/project/solution_model.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include "paths.hpp"
#include "text_util.hpp"

#include <algorithm>
#include <format>
#include <unordered_set>

namespace frost::project {
namespace {

using detail::iequals;

ProjectKind classify(std::string_view path) {
    const std::string extension = detail::lowerExtension(io::fileName(path));
    if (extension == ".vcxproj") {
        return ProjectKind::Cpp;
    }
    if (extension == ".csproj") {
        return ProjectKind::CSharp;
    }
    if (extension == ".vbproj") {
        return ProjectKind::VisualBasic;
    }
    if (extension == ".fsproj") {
        return ProjectKind::FSharp;
    }
    if (extension == ".vcxitems" || extension == ".shproj" || extension == ".projitems") {
        return ProjectKind::SharedItems;
    }
    return ProjectKind::Unknown;
}

// The sort rank of a node kind: folders and filters, then projects, then files.
int rank(NodeKind kind) noexcept {
    switch (kind) {
    case NodeKind::Solution:
    case NodeKind::SolutionFolder:
    case NodeKind::Filter: return 0;
    case NodeKind::Project:
    case NodeKind::UnsupportedProject:
    case NodeKind::UnavailableProject: return 1;
    case NodeKind::File: return 2;
    }
    return 2;
}

// True when a .slnx rule's Solution pattern ("Debug|*", "*|x64", "Debug", "") matches `config`.
bool ruleMatches(std::string_view pattern, const SolutionConfiguration& config) {
    if (pattern.empty() || pattern == "*") {
        return true;
    }
    const std::size_t bar = pattern.find('|');
    const std::string_view configuration = detail::trim(pattern.substr(0, bar));
    const std::string_view platform =
        bar == std::string_view::npos ? std::string_view("*") : detail::trim(pattern.substr(bar + 1));
    const auto part = [](std::string_view p, std::string_view value) { return p == "*" || iequals(p, value); };
    return part(configuration, config.configuration) && part(platform, config.platform);
}

// The .slnx mapping of one project in one solution configuration (solution_model.hpp).
ProjectConfigurationMapping slnxMapping(const SolutionConfiguration& config, const ProjectInfo& info,
                                        const std::vector<ConfigurationRule>& rules) {
    ProjectConfigurationMapping mapping{
        .configuration = config.configuration, .platform = config.platform, .build = true, .deploy = false};
    if (info.vcxproj && !info.vcxproj->configurations.empty()) {
        const auto& configs = info.vcxproj->configurations;
        const auto hasConfiguration = [&](std::string_view name) {
            return std::ranges::any_of(configs,
                                       [&](const ProjectConfiguration& c) { return iequals(c.configuration, name); });
        };
        const auto hasPlatform = [&](std::string_view name) {
            return std::ranges::any_of(configs,
                                       [&](const ProjectConfiguration& c) { return iequals(c.platform, name); });
        };
        if (!hasConfiguration(mapping.configuration)) {
            mapping.configuration = configs.front().configuration;
        }
        if (!hasPlatform(mapping.platform)) {
            if (iequals(mapping.platform, "x86") && hasPlatform("Win32")) {
                mapping.platform = "Win32";
            } else if ((iequals(mapping.platform, "Any CPU") || iequals(mapping.platform, "AnyCPU")) &&
                       hasPlatform("x64")) {
                mapping.platform = "x64";
            } else {
                mapping.platform = configs.front().platform;
            }
        }
    }
    for (const ConfigurationRule& rule : rules) {
        if (!ruleMatches(rule.solutionPattern, config)) {
            continue;
        }
        switch (rule.kind) {
        case ConfigurationRuleKind::BuildType: mapping.configuration = rule.projectValue; break;
        case ConfigurationRuleKind::Platform: mapping.platform = rule.projectValue; break;
        case ConfigurationRuleKind::Build: mapping.build = iequals(rule.projectValue, "true"); break;
        case ConfigurationRuleKind::Deploy: mapping.deploy = iequals(rule.projectValue, "true"); break;
        }
    }
    return mapping;
}

} // namespace

std::string_view toString(ProjectKind kind) noexcept {
    switch (kind) {
    case ProjectKind::Cpp: return "C++";
    case ProjectKind::CSharp: return "C#";
    case ProjectKind::VisualBasic: return "Visual Basic";
    case ProjectKind::FSharp: return "F#";
    case ProjectKind::SharedItems: return "shared items";
    case ProjectKind::Unknown: return "unknown";
    }
    return "unknown";
}

// Builds a SolutionModel from a SolutionFile; a friend of SolutionModel.
class SolutionModelBuilder {
public:
    SolutionModelBuilder(io::IFileSystem& fs, SolutionModel& model) : m_fs(fs), m_model(model) {}

    void build(SolutionFile file) {
        m_model.m_format = file.format;
        m_model.m_path = file.path;
        m_model.m_directory = file.directory;
        m_model.m_name = std::string(detail::stem(io::fileName(file.path)));
        m_model.m_warnings = std::move(file.warnings);
        m_model.m_configurations = file.configurations;
        addNode({.kind = NodeKind::Solution,
                 .parent = std::nullopt,
                 .children = {},
                 .displayName = m_model.m_name,
                 .path = file.path,
                 .key = "solution",
                 .project = std::nullopt,
                 .itemType = ItemType::None});
        addFolders(file);
        for (std::size_t i = 0; i < file.projects.size(); ++i) {
            addProject(file.projects[i], i);
        }
        resolveDependencies(file);
        resolveConfigurations(file);
        sortChildren();
    }

private:
    NodeId addNode(Node node) {
        const NodeId id{static_cast<std::uint32_t>(m_model.m_nodes.size())};
        std::string key = node.key;
        for (unsigned n = 2; m_model.m_nodesByKey.contains(key); ++n) {
            key = std::format("{}#{}", node.key, n); // The same entry listed twice.
        }
        node.key = key;
        m_model.m_nodesByKey.emplace(std::move(key), id);
        if (node.parent) {
            m_model.m_nodes[node.parent->value()].children.push_back(id);
        }
        if (node.kind == NodeKind::File) {
            m_model.m_filesByPath[detail::pathKey(node.path)].push_back(id);
        }
        m_model.m_nodes.push_back(std::move(node));
        return id;
    }

    NodeId addFile(NodeId parent, const std::string& parentKey, const std::string& path, ItemType type,
                   std::optional<ProjectId> project) {
        return addNode({.kind = NodeKind::File,
                        .parent = parent,
                        .children = {},
                        .displayName = std::string(io::fileName(path)),
                        .path = path,
                        .key = parentKey + "/file:" + detail::pathKey(path),
                        .project = project,
                        .itemType = type});
    }

    void warn(std::string message) { m_model.m_warnings.push_back(std::move(message)); }

    // Folders get nodes first, then parents (with a cycle check), then their solution items.
    void addFolders(const SolutionFile& file) {
        std::unordered_map<std::string, std::size_t> index;
        for (std::size_t i = 0; i < file.folders.size(); ++i) {
            index.emplace(file.folders[i].id, i);
        }
        std::vector<NodeId> nodes;
        nodes.reserve(file.folders.size());
        for (const SolutionFolderEntry& folder : file.folders) {
            // Parents are linked below, once every folder has a node.
            const NodeId id{static_cast<std::uint32_t>(m_model.m_nodes.size())};
            std::string key = "folder:" + detail::toLowerAscii(folder.id);
            for (unsigned n = 2; m_model.m_nodesByKey.contains(key); ++n) {
                key = std::format("folder:{}#{}", detail::toLowerAscii(folder.id), n);
            }
            m_model.m_nodesByKey.emplace(key, id);
            m_model.m_nodes.push_back({.kind = NodeKind::SolutionFolder,
                                       .parent = std::nullopt,
                                       .children = {},
                                       .displayName = folder.name,
                                       .path = {},
                                       .key = std::move(key),
                                       .project = std::nullopt,
                                       .itemType = ItemType::None});
            nodes.push_back(id);
            m_folderNodes.emplace(folder.id, id);
        }
        for (std::size_t i = 0; i < file.folders.size(); ++i) {
            NodeId parent = SolutionModel::root();
            const std::string& parentId = file.folders[i].parentId;
            if (!parentId.empty()) {
                // Follow the chain upwards; meeting this folder again means it is part of a cycle.
                // A chain that loops above it (without it) is cut where that loop's members go
                // to the root, so this folder keeps its parent.
                bool cycle = false;
                std::string current = parentId;
                for (std::size_t steps = 0; !current.empty() && steps <= file.folders.size(); ++steps) {
                    auto it = index.find(current);
                    if (it == index.end()) {
                        break;
                    }
                    if (it->second == i) {
                        cycle = true;
                        break;
                    }
                    current = file.folders[it->second].parentId;
                }
                auto parentNode = m_folderNodes.find(parentId);
                if (cycle) {
                    warn(std::format("solution folder '{}' is nested in a cycle; shown at the solution root",
                                     file.folders[i].name));
                } else if (parentNode != m_folderNodes.end()) {
                    parent = parentNode->second;
                }
            }
            m_model.m_nodes[nodes[i].value()].parent = parent;
            m_model.m_nodes[parent.value()].children.push_back(nodes[i]);
        }
        for (std::size_t i = 0; i < file.folders.size(); ++i) {
            const std::string key = m_model.m_nodes[nodes[i].value()].key;
            for (const std::string& item : file.folders[i].files) {
                addFile(nodes[i], key, item, ItemType::None, std::nullopt);
            }
        }
    }

    void addProject(const SolutionProjectEntry& entry, std::size_t index) {
        const ProjectId id{static_cast<std::uint32_t>(index)};
        ProjectInfo info;
        info.name = entry.name;
        info.path = entry.path;
        info.projectGuid = entry.projectGuid;
        info.typeGuid = entry.typeGuid;
        info.kind = classify(entry.path.empty() ? entry.relativePath : entry.path);
        if (entry.path.empty()) {
            info.nodeKind = NodeKind::UnavailableProject;
            info.error = Error(ErrorCode::InvalidArgument,
                               std::format("the project path '{}' cannot be resolved", entry.relativePath));
        } else if (info.kind == ProjectKind::Cpp) {
            auto project = readVcxproj(m_fs, entry.path, VcxprojOptions{.solutionDirectory = m_model.m_directory});
            if (project) {
                info.nodeKind = NodeKind::Project;
                if (info.projectGuid.empty()) {
                    info.projectGuid = project->projectGuid;
                }
                info.vcxproj = std::move(*project);
            } else {
                info.nodeKind = NodeKind::UnavailableProject;
                info.error = std::move(project.error());
            }
        } else {
            info.nodeKind = NodeKind::UnsupportedProject;
        }

        NodeId parent = SolutionModel::root();
        if (!entry.parentId.empty()) {
            if (auto it = m_folderNodes.find(entry.parentId); it != m_folderNodes.end()) {
                parent = it->second;
            }
        }
        const std::string pathKey = entry.path.empty() ? std::format("#{}", index) : detail::pathKey(entry.path);
        info.node = addNode({.kind = info.nodeKind,
                             .parent = parent,
                             .children = {},
                             .displayName = entry.name,
                             .path = entry.path,
                             .key = "project:" + pathKey,
                             .project = id,
                             .itemType = ItemType::None});
        if (!entry.path.empty()) {
            m_model.m_projectsByPath.emplace(pathKey, id);
        }
        if (!info.projectGuid.empty()) {
            m_model.m_projectsByGuid.emplace(info.projectGuid, id);
        }
        if (info.vcxproj) {
            addProjectTree(info, id);
        }
        m_model.m_projects.push_back(std::move(info));
    }

    // The filter tree and the files of a loaded C++ project.
    void addProjectTree(const ProjectInfo& info, ProjectId id) {
        const VcxProject& project = *info.vcxproj;
        const std::string projectKey = m_model.m_nodes[info.node.value()].key;
        for (const std::string& message : project.warnings) {
            warn(std::format("{}: {}", info.name, message));
        }
        for (const std::string& include : project.unresolvedIncludes) {
            warn(std::format("{}: Include '{}' names a property defined outside the project file (imports are not "
                             "read yet); skipped",
                             info.name, include));
        }
        FiltersFile filters;
        if (auto read = readVcxprojFilters(m_fs, project)) {
            filters = std::move(*read);
        } else {
            warn(std::format("{}: filters ignored: {}", info.name, read.error().message()));
        }
        for (const std::string& message : filters.warnings) {
            warn(std::format("{} (filters): {}", info.name, message));
        }
        std::unordered_map<std::string, NodeId> filterNodes; // Lower-cased filter path -> node.
        const auto ensureFilter = [&](std::string_view filter) {
            NodeId parent = info.node;
            std::string prefix;
            for (std::string_view component : detail::splitTrimmed(filter, '\\')) {
                if (!prefix.empty()) {
                    prefix += '\\';
                }
                prefix += component;
                const std::string key = detail::toLowerAscii(prefix);
                if (auto it = filterNodes.find(key); it != filterNodes.end()) {
                    parent = it->second;
                    continue;
                }
                parent = addNode({.kind = NodeKind::Filter,
                                  .parent = parent,
                                  .children = {},
                                  .displayName = std::string(component),
                                  .path = {},
                                  .key = projectKey + "/filter:" + key,
                                  .project = id,
                                  .itemType = ItemType::None});
                filterNodes.emplace(key, parent);
            }
            return parent;
        };
        for (const FilterDefinition& filter : filters.filters) {
            ensureFilter(filter.path);
        }
        std::unordered_map<std::string, std::string> assignment; // Item path key -> filter path.
        for (const FilterAssignment& a : filters.assignments) {
            assignment.emplace(detail::pathKey(a.itemPath), a.filter);
        }
        for (const ProjectItem& item : project.items) {
            NodeId parent = info.node;
            if (auto it = assignment.find(detail::pathKey(item.path)); it != assignment.end() && !it->second.empty()) {
                parent = ensureFilter(it->second);
            }
            addFile(parent, projectKey, item.path, item.type, id);
        }
    }

    void resolveDependencies(const SolutionFile& file) {
        for (std::size_t i = 0; i < m_model.m_projects.size(); ++i) {
            ProjectInfo& info = m_model.m_projects[i];
            const ProjectId self{static_cast<std::uint32_t>(i)};
            std::unordered_set<std::uint32_t> seen;
            const auto add = [&](ProjectId dependency) {
                if (dependency != self && seen.insert(dependency.value()).second) {
                    info.dependencies.push_back(dependency);
                }
            };
            for (const std::string& dependency : file.projects[i].dependencies) {
                auto found = file.format == SolutionFormat::Sln ? m_model.findProjectByGuid(dependency)
                                                                : m_model.findProjectByPath(dependency);
                if (found) {
                    add(*found);
                } else {
                    warn(
                        std::format("{}: build dependency {} names no project in the solution", info.name, dependency));
                }
            }
            if (!info.vcxproj) {
                continue;
            }
            for (const ProjectReference& reference : info.vcxproj->references) {
                auto found = m_model.findProjectByPath(reference.path);
                if (!found && !reference.projectGuid.empty()) {
                    found = m_model.findProjectByGuid(reference.projectGuid);
                }
                if (found) {
                    add(*found);
                } else {
                    info.externalReferences.push_back(reference.path);
                }
            }
        }
    }

    void resolveConfigurations(const SolutionFile& file) {
        const auto& configs = m_model.m_configurations;
        for (std::size_t i = 0; i < m_model.m_projects.size(); ++i) {
            ProjectInfo& info = m_model.m_projects[i];
            const SolutionProjectEntry& entry = file.projects[i];
            info.configurationMap.resize(configs.size());
            for (std::size_t c = 0; c < configs.size(); ++c) {
                if (file.format == SolutionFormat::Slnx) {
                    info.configurationMap[c] = slnxMapping(configs[c], info, entry.slnxRules);
                    continue;
                }
                const std::string name = configs[c].name();
                for (const auto& [solutionConfig, mapping] : entry.slnConfigurations) {
                    if (iequals(solutionConfig, name)) {
                        info.configurationMap[c] = mapping;
                    }
                }
            }
        }
    }

    void sortChildren() {
        const auto& nodes = m_model.m_nodes;
        const auto before = [&nodes](NodeId a, NodeId b) {
            const Node& x = nodes[a.value()];
            const Node& y = nodes[b.value()];
            if (rank(x.kind) != rank(y.kind)) {
                return rank(x.kind) < rank(y.kind);
            }
            if (const int c = detail::icompare(x.displayName, y.displayName); c != 0) {
                return c < 0;
            }
            return x.key < y.key;
        };
        for (Node& node : m_model.m_nodes) {
            std::ranges::sort(node.children, before);
        }
    }

    io::IFileSystem& m_fs;
    SolutionModel& m_model;
    std::unordered_map<std::string, NodeId> m_folderNodes; // Folder id -> node.
};

Result<SolutionModel> SolutionModel::load(io::IFileSystem& fs, std::string_view solutionPath) {
    auto file = readSolutionFile(fs, solutionPath);
    if (!file) {
        return std::unexpected(std::move(file.error()));
    }
    SolutionModel model;
    SolutionModelBuilder(fs, model).build(std::move(*file));
    return model;
}

const Node& SolutionModel::node(NodeId id) const {
    FROST_PRE(id.value() < m_nodes.size());
    return m_nodes[id.value()];
}

std::optional<NodeId> SolutionModel::findNode(std::string_view key) const {
    auto it = m_nodesByKey.find(std::string(key));
    if (it == m_nodesByKey.end()) {
        return std::nullopt;
    }
    return it->second;
}

SolutionModel SolutionModel::withEvaluatedFiles(ProjectId projectId, ItemType type,
                                                std::span<const std::string> files) const {
    SolutionModel out = *this;
    const ProjectInfo& info = out.project(projectId);
    std::unordered_set<std::string> wanted;
    for (const auto& file : files) {
        if (auto normalized = detail::normalizeAbsolute(file))
            wanted.insert(detail::pathKey(*normalized));
    }
    std::unordered_set<std::string> present;
    for (std::size_t i = 0; i < out.m_nodes.size(); ++i) {
        Node& node = out.m_nodes[i];
        if (node.kind != NodeKind::File || node.project != projectId || node.itemType != type)
            continue;
        const auto key = detail::pathKey(node.path);
        if (wanted.contains(key)) {
            present.insert(key);
            continue;
        }
        const NodeId id{static_cast<std::uint32_t>(i)};
        if (node.parent)
            std::erase(out.m_nodes[node.parent->value()].children, id);
        out.m_nodesByKey.erase(node.key);
        auto it = out.m_filesByPath.find(key);
        if (it != out.m_filesByPath.end()) {
            std::erase(it->second, id);
            if (it->second.empty())
                out.m_filesByPath.erase(it);
        }
    }
    const std::string projectKey = out.node(info.node).key;
    for (const auto& file : files) {
        auto normalized = detail::normalizeAbsolute(file);
        if (!normalized)
            continue;
        const auto pathKey = detail::pathKey(*normalized);
        if (!present.insert(pathKey).second)
            continue;
        const NodeId id{static_cast<std::uint32_t>(out.m_nodes.size())};
        std::string key = projectKey + "/file:" + pathKey;
        const std::string base = key;
        for (unsigned n = 2; out.m_nodesByKey.contains(key); ++n)
            key = std::format("{}#{}", base, n);
        out.m_nodes.push_back({.kind = NodeKind::File,
                               .parent = info.node,
                               .children = {},
                               .displayName = std::string(io::fileName(*normalized)),
                               .path = *normalized,
                               .key = key,
                               .project = projectId,
                               .itemType = type});
        out.m_nodes[info.node.value()].children.push_back(id);
        out.m_nodesByKey.emplace(std::move(key), id);
        auto& matches = out.m_filesByPath[pathKey];
        matches.push_back(id);
        std::ranges::stable_sort(matches, [&out](NodeId a, NodeId b) {
            const auto left = out.node(a).project;
            const auto right = out.node(b).project;
            if (!left || !right)
                return !left && right.has_value();
            return left->value() < right->value();
        });
    }
    auto& children = out.m_nodes[info.node.value()].children;
    std::ranges::sort(children, [&out](NodeId a, NodeId b) {
        const Node& x = out.node(a);
        const Node& y = out.node(b);
        if (rank(x.kind) != rank(y.kind))
            return rank(x.kind) < rank(y.kind);
        if (const int c = detail::icompare(x.displayName, y.displayName); c != 0)
            return c < 0;
        return x.key < y.key;
    });
    return out;
}

const ProjectInfo& SolutionModel::project(ProjectId id) const {
    FROST_PRE(id.value() < m_projects.size());
    return m_projects[id.value()];
}

std::optional<ProjectId> SolutionModel::findProjectByPath(std::string_view path) const {
    auto normalized = detail::normalizeAbsolute(path);
    if (!normalized) {
        return std::nullopt;
    }
    auto it = m_projectsByPath.find(detail::pathKey(*normalized));
    if (it == m_projectsByPath.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<ProjectId> SolutionModel::findProjectByGuid(std::string_view guid) const {
    auto it = m_projectsByGuid.find(detail::normalizeGuid(guid));
    if (it == m_projectsByGuid.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::size_t> SolutionModel::findConfiguration(std::string_view name) const {
    for (std::size_t i = 0; i < m_configurations.size(); ++i) {
        if (iequals(m_configurations[i].name(), name)) {
            return i;
        }
    }
    return std::nullopt;
}

const std::optional<ProjectConfigurationMapping>&
SolutionModel::projectConfiguration(ProjectId project, std::size_t configurationIndex) const {
    const ProjectInfo& info = this->project(project);
    FROST_PRE(configurationIndex < info.configurationMap.size());
    return info.configurationMap[configurationIndex];
}

std::vector<ProjectId> SolutionModel::projectsContaining(std::string_view filePath) const {
    std::vector<ProjectId> out;
    auto normalized = detail::normalizeAbsolute(filePath);
    if (!normalized) {
        return out;
    }
    auto it = m_filesByPath.find(detail::pathKey(*normalized));
    if (it == m_filesByPath.end()) {
        return out;
    }
    for (NodeId id : it->second) {
        const auto& project = m_nodes[id.value()].project;
        if (project && std::ranges::find(out, *project) == out.end()) {
            out.push_back(*project);
        }
    }
    return out;
}

std::optional<ProjectId> SolutionModel::owningProject(std::string_view filePath) const {
    auto projects = projectsContaining(filePath);
    if (projects.empty()) {
        return std::nullopt;
    }
    return projects.front();
}

std::optional<NodeId> SolutionModel::findFileNode(std::string_view filePath) const {
    auto normalized = detail::normalizeAbsolute(filePath);
    if (!normalized) {
        return std::nullopt;
    }
    auto it = m_filesByPath.find(detail::pathKey(*normalized));
    if (it == m_filesByPath.end() || it->second.empty()) {
        return std::nullopt;
    }
    return it->second.front();
}

} // namespace frost::project
