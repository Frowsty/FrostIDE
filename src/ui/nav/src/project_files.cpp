// SPDX-License-Identifier: Apache-2.0
//
// ProjectFiles and the project scan options (frost/ui/nav/project_files.hpp).

#include "frost/ui/nav/project_files.hpp"

#include "frost/project/directory_roles.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/ui/nav/paths.hpp"

#include <algorithm>
#include <set>
#include <unordered_map>
#include <utility>

namespace frost::ui::nav {
namespace {

[[nodiscard]] std::string folded(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return out;
}

} // namespace

io::ScanOptions projectScanOptions(std::shared_ptr<const project::DirectoryRoles> roles) {
    io::ScanOptions options;
    if (roles) {
        // Budget: B23. One status() per new folder at most (rules 5 and 6), cached in the roles.
        options.descend = [roles = std::move(roles)](std::string_view directory) {
            return roles->traversed(directory);
        };
    } else {
        options.ignore = io::IgnoreStack().pushed(io::defaultProjectExcludes());
    }
    options.readGitignore = true;
    options.extraIgnoreFiles = {".ignore"};
    options.skipNames = io::defaultProjectSkipNames();
    options.recursive = true;
    return options;
}

sched::Task<Result<io::TreeScan>> scanProject(io::IFileSystem& fs, sched::IScheduler& scheduler, std::string root,
                                              std::stop_token token,
                                              std::shared_ptr<const project::DirectoryRoles> roles) {
    return io::scanTree(fs, scheduler, std::move(root), projectScanOptions(std::move(roles)), std::move(token));
}

std::vector<std::string> solutionFiles(const project::SolutionModel& solution) {
    std::vector<std::string> files;
    for (std::size_t i = 0; i < solution.nodeCount(); ++i) {
        const project::Node& node = solution.node(project::NodeId{static_cast<std::uint32_t>(i)});
        if (node.kind == project::NodeKind::File && !node.path.empty()) {
            files.push_back(node.path);
        }
    }
    return files;
}

ProjectFiles::ProjectFiles(std::string root) : m_root(std::move(root)) {
    m_nodes.push_back(Node{std::string(baseName(m_root)), m_root, {}, true, std::nullopt, {}});
    m_byPath.emplace_back(m_root, 0);
}

void ProjectFiles::setScan(const io::TreeScan& scan) { static_cast<void>(setScanCancellable(scan, {})); }

bool ProjectFiles::setScanCancellable(const io::TreeScan& scan, const std::stop_token& token) {
    m_nodes.clear();
    m_byPath.clear();
    m_nodes.push_back(Node{std::string(baseName(m_root)), m_root, {}, true, std::nullopt, {}});
    std::unordered_map<std::string, std::size_t> byRelative;
    byRelative.emplace("", 0);
    m_fileCount = 0;
    for (const io::ScanEntry& entry : scan.entries) {
        if (token.stop_requested())
            return false;
        if (entry.type == io::FileType::Other || entry.ignored) {
            continue;
        }
        const std::size_t slash = entry.relative.rfind('/');
        const std::string parentRelative = slash == std::string::npos ? std::string() : entry.relative.substr(0, slash);
        const auto parent = byRelative.find(parentRelative);
        const std::size_t parentIndex = parent == byRelative.end() ? 0 : parent->second;
        const std::size_t index = m_nodes.size();
        const bool directory = entry.type == io::FileType::Directory;
        m_nodes.push_back(
            Node{std::string(baseName(entry.relative)), entry.path, entry.relative, directory, parentIndex, {}});
        m_nodes[parentIndex].children.push_back(index);
        if (directory) {
            byRelative.emplace(entry.relative, index);
        } else {
            ++m_fileCount;
        }
    }
    std::vector<std::string> foldedNames;
    foldedNames.reserve(m_nodes.size());
    for (const Node& node : m_nodes) {
        if (token.stop_requested())
            return false;
        foldedNames.push_back(folded(node.name));
    }
    for (Node& node : m_nodes) {
        if (token.stop_requested())
            return false;
        std::ranges::sort(node.children, [this, &foldedNames](std::size_t a, std::size_t b) {
            const Node& left = m_nodes[a];
            const Node& right = m_nodes[b];
            if (left.directory != right.directory) {
                return left.directory;
            }
            return foldedNames[a] != foldedNames[b] ? foldedNames[a] < foldedNames[b] : left.name < right.name;
        });
    }
    for (const Node& node : m_nodes) {
        for (std::size_t row = 0; row < node.children.size(); ++row) {
            m_nodes[node.children[row]].row = row;
        }
    }
    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        m_byPath.emplace_back(m_nodes[i].path, i);
    }
    std::ranges::sort(m_byPath);
    return rebuildCandidates(token);
}

void ProjectFiles::setSolutionFiles(std::vector<std::string> files) {
    static_cast<void>(setSolutionFilesCancellable(std::move(files), {}));
}

bool ProjectFiles::setSolutionFilesCancellable(std::vector<std::string> files, const std::stop_token& token) {
    m_solutionFiles = std::move(files);
    return rebuildCandidates(token);
}

std::optional<std::size_t> ProjectFiles::find(std::string_view path) const {
    const auto it =
        std::ranges::lower_bound(m_byPath, path, {}, [](const auto& entry) { return std::string_view(entry.first); });
    if (it == m_byPath.end() || it->first != path) {
        return std::nullopt;
    }
    return it->second;
}

bool ProjectFiles::rebuildCandidates(const std::stop_token& token) {
    m_candidates.clear();
    m_display.clear();
    const bool foldCase = styleOf(m_root) == io::PathStyle::Windows;
    std::set<std::string, std::less<>> seen;
    const auto add = [&](const std::string& path) {
        std::string key = foldCase ? folded(path) : path;
        if (!seen.insert(std::move(key)).second) {
            return;
        }
        m_candidates.push_back(path);
        m_display.push_back(relativeTo(m_root, path));
    };
    for (const Node& node : m_nodes) {
        if (token.stop_requested())
            return false;
        if (!node.directory) {
            add(node.path);
        }
    }
    for (const std::string& path : m_solutionFiles) {
        if (token.stop_requested())
            return false;
        add(path);
    }
    return !token.stop_requested();
}

} // namespace frost::ui::nav
