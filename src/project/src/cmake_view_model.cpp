// SPDX-License-Identifier: Apache-2.0
//
// CMakeViewModel (frost/project/cmake_view_model.hpp): the CMake target tree for the Solution
// Explorer.

#include "frost/project/cmake_view_model.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/path.hpp"
#include "frost/project/cmake_profile.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace frost::project {

namespace {

constexpr char kSeparator = io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';

constexpr char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), lowerAscii);
    return out;
}

// The comparison key of a path: lower-cased on Windows.
std::string pathKey(std::string_view path) {
    return io::kNativePathStyle == io::PathStyle::Windows ? lowered(path) : std::string(path);
}

// `path` relative to `base` (both normalised) when it lies strictly below it; nullopt otherwise.
std::optional<std::string> relativeTo(std::string_view base, std::string_view path) {
    if (!isPathWithin(base, path) || path.size() <= base.size()) {
        return std::nullopt;
    }
    std::string_view rest = path.substr(base.size());
    while (!rest.empty() && rest.front() == kSeparator) {
        rest.remove_prefix(1);
    }
    return std::string(rest);
}

bool isFolderKind(CMakeViewNodeKind kind) noexcept {
    return kind == CMakeViewNodeKind::Folder || kind == CMakeViewNodeKind::Target;
}

} // namespace

CMakeViewModel::CMakeViewModel(std::string rootLabel) {
    add(CMakeViewNode{.kind = CMakeViewNodeKind::Project,
                      .label = std::move(rootLabel),
                      .path = {},
                      .target = std::nullopt,
                      .targetType = CMakeTargetType::Utility,
                      .generated = false,
                      .parent = std::nullopt,
                      .children = {},
                      .key = "project"});
}

std::size_t CMakeViewModel::add(CMakeViewNode node) {
    const std::size_t id = m_nodes.size();
    if (node.parent) {
        m_nodes[*node.parent].children.push_back(id);
    }
    m_keys.emplace_back(node.key, id);
    m_nodes.push_back(std::move(node));
    return id;
}

const CMakeViewNode& CMakeViewModel::node(std::size_t id) const {
    FROST_PRE(id < m_nodes.size());
    return m_nodes[id];
}

std::optional<std::size_t> CMakeViewModel::findKey(std::string_view key) const {
    const auto found =
        std::ranges::lower_bound(m_keys, key, std::less<>{}, &std::pair<std::string, std::size_t>::first);
    if (found == m_keys.end() || found->first != key) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<std::size_t> CMakeViewModel::findPath(std::string_view path) const {
    std::vector<std::size_t> out;
    const std::string wanted = pathKey(path);
    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        const CMakeViewNode& node = m_nodes[i];
        if ((node.kind == CMakeViewNodeKind::Source || node.kind == CMakeViewNodeKind::CMakeFile) &&
            pathKey(node.path) == wanted) {
            out.push_back(i);
        }
    }
    return out;
}

CMakeViewModel CMakeViewModel::build(const CMakeProject& project, std::span<const std::string> presetFiles,
                                     std::string_view rootLabel) {
    std::string label(rootLabel);
    if (label.empty()) {
        label = !project.name.empty() ? project.name : std::string(io::fileName(project.sourceDirectory));
    }
    CMakeViewModel model(label);
    model.m_nodes.front().path = project.sourceDirectory;
    const std::size_t rootId = root();
    std::set<std::string> usedKeys; // File keys added so far (folders are deduplicated per target).

    for (std::size_t t = 0; t < project.targets.size(); ++t) {
        const CMakeTarget& target = project.targets[t];
        if (target.generatorProvided) {
            continue;
        }
        const std::string targetKey = "target:" + target.name;
        const std::size_t targetId = model.add(CMakeViewNode{.kind = CMakeViewNodeKind::Target,
                                                             .label = target.name,
                                                             .path = target.sourceDirectory,
                                                             .target = t,
                                                             .targetType = target.type,
                                                             .generated = false,
                                                             .parent = rootId,
                                                             .children = {},
                                                             .key = targetKey});
        std::map<std::string, std::size_t> folders; // Folder key -> node.
        const auto folder = [&](std::size_t parent, const std::string& key, std::string folderLabel,
                                std::string path) -> std::size_t {
            if (const auto found = folders.find(key); found != folders.end()) {
                return found->second;
            }
            const std::size_t id = model.add(CMakeViewNode{.kind = CMakeViewNodeKind::Folder,
                                                           .label = std::move(folderLabel),
                                                           .path = std::move(path),
                                                           .target = t,
                                                           .targetType = CMakeTargetType::Utility,
                                                           .generated = false,
                                                           .parent = parent,
                                                           .children = {},
                                                           .key = key});
            folders.emplace(key, id);
            return id;
        };
        for (const CMakeSource& source : target.sources) {
            std::size_t parent = targetId;
            const auto relative = relativeTo(project.sourceDirectory, source.path);
            if (source.generated) {
                parent = folder(targetId, targetKey + "/generated", "Generated", {});
            } else if (!relative) {
                parent = folder(targetId, targetKey + "/external", "External", {});
            } else {
                std::string_view rest = *relative;
                std::string sofar = project.sourceDirectory;
                std::string keyPath;
                for (std::size_t cut = rest.find(kSeparator); cut != std::string_view::npos;
                     cut = rest.find(kSeparator)) {
                    const std::string component(rest.substr(0, cut));
                    sofar += kSeparator;
                    sofar += component;
                    keyPath += '/';
                    keyPath += pathKey(component);
                    parent = folder(parent, targetKey + "/dir:" + keyPath, component, sofar);
                    rest.remove_prefix(cut + 1);
                }
            }
            const std::string key = targetKey + "/file:" + pathKey(source.path);
            if (!usedKeys.insert(key).second) {
                continue; // Listed twice by the File API (rare): one node.
            }
            model.add(CMakeViewNode{.kind = CMakeViewNodeKind::Source,
                                    .label = std::string(io::fileName(source.path)),
                                    .path = source.path,
                                    .target = t,
                                    .targetType = CMakeTargetType::Utility,
                                    .generated = source.generated,
                                    .parent = parent,
                                    .children = {},
                                    .key = key});
        }
    }

    const std::size_t cmakeId = model.add(CMakeViewNode{.kind = CMakeViewNodeKind::CMakeFiles,
                                                        .label = "CMake",
                                                        .path = {},
                                                        .target = std::nullopt,
                                                        .targetType = CMakeTargetType::Utility,
                                                        .generated = false,
                                                        .parent = rootId,
                                                        .children = {},
                                                        .key = "cmake"});
    std::vector<std::string> files = project.projectInputs();
    files.insert(files.end(), presetFiles.begin(), presetFiles.end());
    for (const std::string& file : files) {
        const std::string key = "cmake:" + pathKey(file);
        if (!usedKeys.insert(key).second) {
            continue;
        }
        const auto relative = relativeTo(project.sourceDirectory, file);
        std::string fileLabel = relative ? *relative : file;
        if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
            std::ranges::replace(fileLabel, '\\', '/');
        }
        model.add(CMakeViewNode{.kind = CMakeViewNodeKind::CMakeFile,
                                .label = std::move(fileLabel),
                                .path = file,
                                .target = std::nullopt,
                                .targetType = CMakeTargetType::Utility,
                                .generated = false,
                                .parent = cmakeId,
                                .children = {},
                                .key = key});
    }

    // Sorting (header comment): targets by name with "CMake" last; folders before files.
    for (CMakeViewNode& node : model.m_nodes) {
        std::ranges::stable_sort(node.children, [&](std::size_t a, std::size_t b) {
            const CMakeViewNode& x = model.m_nodes[a];
            const CMakeViewNode& y = model.m_nodes[b];
            const bool xLast = x.kind == CMakeViewNodeKind::CMakeFiles;
            const bool yLast = y.kind == CMakeViewNodeKind::CMakeFiles;
            if (xLast != yLast) {
                return yLast;
            }
            const bool xFolder = isFolderKind(x.kind);
            const bool yFolder = isFolderKind(y.kind);
            if (xFolder != yFolder) {
                return xFolder;
            }
            // "Generated" and "External" come after the real folders.
            const bool xSynthetic = x.kind == CMakeViewNodeKind::Folder && x.path.empty();
            const bool ySynthetic = y.kind == CMakeViewNodeKind::Folder && y.path.empty();
            if (xSynthetic != ySynthetic) {
                return ySynthetic;
            }
            return lowered(x.label) < lowered(y.label);
        });
    }
    std::ranges::sort(model.m_keys);
    return model;
}

} // namespace frost::project
