// SPDX-License-Identifier: Apache-2.0
//
// The static .slnx reader (solution_file.hpp): the XML solution format, read with pugixml.
#include "frost/io/path.hpp"
#include "frost/project/solution_file.hpp"

#include "paths.hpp"
#include "text_util.hpp"
#include "xml.hpp"

#include <format>
#include <unordered_map>

namespace frost::project {
namespace {

using detail::attribute;
using detail::isElement;

// True for "{8-4-4-4-12}" or the same without braces.
bool looksLikeGuid(std::string_view text) {
    const std::string normalized = detail::normalizeGuid(text);
    return normalized.size() == 38 && normalized.front() == '{';
}

// The last component of a path written with '/' or '\'.
std::string_view lastComponent(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

class SlnxParser {
public:
    explicit SlnxParser(SolutionFile& file) : m_file(file) {}

    void parse(const pugi::xml_node& root) {
        m_file.formatVersion = std::string(attribute(root, "Version"));
        std::vector<std::string> buildTypes;
        std::vector<std::string> platforms;
        for (pugi::xml_node child = root.first_child(); child; child = child.next_sibling()) {
            if (isElement(child, "Configurations")) {
                for (pugi::xml_node c = child.first_child(); c; c = c.next_sibling()) {
                    const std::string_view name = detail::trim(attribute(c, "Name"));
                    if (name.empty()) {
                        continue;
                    }
                    if (isElement(c, "BuildType")) {
                        addUnique(buildTypes, name);
                    } else if (isElement(c, "Platform")) {
                        addUnique(platforms, name);
                    }
                }
            } else if (isElement(child, "Folder")) {
                folder(child, "");
            } else if (isElement(child, "Project")) {
                project(child, "");
            } else if (isElement(child, "File")) {
                m_file.warnings.emplace_back("a <File> outside any <Folder> was ignored");
            }
        }
        if (buildTypes.empty()) {
            buildTypes = {"Debug", "Release"};
        }
        if (platforms.empty()) {
            platforms = {"Any CPU"};
        }
        for (const std::string& buildType : buildTypes) {
            for (const std::string& platform : platforms) {
                m_file.configurations.push_back({.configuration = buildType, .platform = platform});
            }
        }
    }

private:
    static void addUnique(std::vector<std::string>& list, std::string_view value) {
        for (const std::string& existing : list) {
            if (detail::iequals(existing, value)) {
                return;
            }
        }
        list.emplace_back(value);
    }

    // Returns the id ("/a/b/") of the folder named `fullName`, creating it and its parents.
    std::string ensureFolder(std::string_view fullName) {
        std::string id = "/";
        std::string parent;
        for (std::string_view component : detail::splitTrimmed(fullName, '/')) {
            id += component;
            id += '/';
            const std::string key = detail::toLowerAscii(id);
            if (!m_folders.contains(key)) {
                m_folders.emplace(key, m_file.folders.size());
                m_file.folders.push_back({.id = id, .name = std::string(component), .parentId = parent, .files = {}});
            }
            parent = m_file.folders[m_folders.at(key)].id;
        }
        return parent;
    }

    void folder(const pugi::xml_node& node, std::string_view parentId) {
        std::string_view name = detail::trim(attribute(node, "Name"));
        std::string fullName;
        if (!name.empty() && name.front() != '/' && !parentId.empty()) {
            fullName = std::string(parentId) + std::string(name); // A nested <Folder> with a relative name.
        } else {
            fullName = std::string(name);
        }
        const std::string id = ensureFolder(fullName);
        if (id.empty()) {
            m_file.warnings.emplace_back("a <Folder> without a Name was ignored");
            return;
        }
        for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
            if (isElement(child, "Project")) {
                project(child, id);
            } else if (isElement(child, "File")) {
                const std::string_view path = attribute(child, "Path");
                auto resolved = detail::resolvePath(m_file.directory, path);
                if (resolved) {
                    m_file.folders[m_folders.at(detail::toLowerAscii(id))].files.push_back(std::move(*resolved));
                } else {
                    m_file.warnings.push_back(
                        std::format("solution item '{}' skipped: {}", path, resolved.error().message()));
                }
            } else if (isElement(child, "Folder")) {
                folder(child, id);
            }
        }
    }

    void project(const pugi::xml_node& node, std::string_view parentId) {
        const std::string_view path = detail::trim(attribute(node, "Path"));
        if (path.empty()) {
            m_file.warnings.emplace_back("a <Project> without a Path was ignored");
            return;
        }
        SolutionProjectEntry entry;
        entry.relativePath = std::string(path);
        entry.parentId = std::string(parentId);
        if (auto resolved = detail::resolvePath(m_file.directory, path)) {
            entry.path = std::move(*resolved);
        } else {
            m_file.warnings.push_back(
                std::format("project '{}' has an unusable path: {}", path, resolved.error().message()));
        }
        const std::string_view type = detail::trim(attribute(node, "Type"));
        if (looksLikeGuid(type)) {
            entry.typeGuid = detail::normalizeGuid(type);
        } else {
            entry.typeName = std::string(type);
        }
        const std::string_view displayName = detail::trim(attribute(node, "DisplayName"));
        entry.name = displayName.empty() ? std::string(detail::stem(lastComponent(path))) : std::string(displayName);
        if (const std::string_view id = attribute(node, "Id"); !detail::trim(id).empty()) {
            entry.projectGuid = detail::normalizeGuid(id);
        }
        for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
            if (isElement(child, "BuildDependency")) {
                const std::string_view dependency = attribute(child, "Project");
                if (auto resolved = detail::resolvePath(m_file.directory, dependency)) {
                    entry.dependencies.push_back(std::move(*resolved));
                }
                continue;
            }
            ConfigurationRuleKind kind{};
            if (isElement(child, "BuildType")) {
                kind = ConfigurationRuleKind::BuildType;
            } else if (isElement(child, "Platform")) {
                kind = ConfigurationRuleKind::Platform;
            } else if (isElement(child, "Build")) {
                kind = ConfigurationRuleKind::Build;
            } else if (isElement(child, "Deploy")) {
                kind = ConfigurationRuleKind::Deploy;
            } else {
                continue;
            }
            entry.slnxRules.push_back({.kind = kind,
                                       .solutionPattern = std::string(detail::trim(attribute(child, "Solution"))),
                                       .projectValue = std::string(detail::trim(attribute(child, "Project")))});
        }
        m_file.projects.push_back(std::move(entry));
    }

    SolutionFile& m_file;
    std::unordered_map<std::string, std::size_t> m_folders; // Lower-cased folder id -> index.
};

} // namespace

Result<SolutionFile> parseSlnx(std::string_view content, std::string_view solutionPath) {
    auto path = detail::normalizeAbsolute(solutionPath);
    if (!path) {
        return std::unexpected(std::move(path.error()));
    }
    pugi::xml_document doc;
    if (auto parsed = detail::parseXml(doc, content, *path, "Solution"); !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    SolutionFile file;
    file.format = SolutionFormat::Slnx;
    file.directory = std::string(io::parentPath(*path));
    file.path = std::move(*path);
    SlnxParser(file).parse(doc.document_element());
    return file;
}

} // namespace frost::project
