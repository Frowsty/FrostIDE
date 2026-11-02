// SPDX-License-Identifier: Apache-2.0
//
// The static .vcxproj and .vcxproj.filters readers (vcxproj.hpp), over pugixml.
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/vcxproj.hpp"

#include "paths.hpp"
#include "text_util.hpp"
#include "xml.hpp"

#include <array>
#include <cstdlib>
#include <format>
#include <optional>
#include <unordered_set>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#endif

namespace frost::project {
namespace {

using detail::attribute;
using detail::elementText;
using detail::iequals;
using detail::isElement;
using detail::localName;
using detail::trim;

using PropertyTable = std::map<std::string, std::string, std::less<>>;

struct ItemTypeName {
    std::string_view name;
    ItemType type;
};

constexpr std::array kItemTypes{
    ItemTypeName{"ClCompile", ItemType::ClCompile},
    ItemTypeName{"ClInclude", ItemType::ClInclude},
    ItemTypeName{"None", ItemType::None},
    ItemTypeName{"ResourceCompile", ItemType::ResourceCompile},
    ItemTypeName{"Text", ItemType::Text},
    ItemTypeName{"CustomBuild", ItemType::CustomBuild},
    ItemTypeName{"Image", ItemType::Image},
    ItemTypeName{"Natvis", ItemType::Natvis},
    ItemTypeName{"MASM", ItemType::Other},
    ItemTypeName{"Midl", ItemType::Other},
    ItemTypeName{"FxCompile", ItemType::Other},
    ItemTypeName{"Manifest", ItemType::Other},
    ItemTypeName{"CopyFileToFolders", ItemType::Other},
    ItemTypeName{"Xml", ItemType::Other},
};

std::optional<ItemType> fileItemType(std::string_view name) {
    for (const ItemTypeName& entry : kItemTypes) {
        if (iequals(entry.name, name)) {
            return entry.type;
        }
    }
    return std::nullopt;
}

// The pair of the standard "'$(Configuration)|$(Platform)'=='Debug|x64'" condition, else nullopt.
std::optional<std::pair<std::string, std::string>> configurationCondition(std::string_view condition) {
    const std::size_t eq = condition.find("==");
    if (eq == std::string_view::npos) {
        return std::nullopt;
    }
    std::string left;
    for (char c : condition.substr(0, eq)) {
        if (c != ' ' && c != '\t' && c != '\'') {
            left += detail::toLowerAscii(c);
        }
    }
    if (left != "$(configuration)|$(platform)") {
        return std::nullopt;
    }
    std::string_view right = trim(condition.substr(eq + 2));
    if (right.size() < 2 || right.front() != '\'' || right.back() != '\'') {
        return std::nullopt;
    }
    right = right.substr(1, right.size() - 2);
    const std::size_t bar = right.find('|');
    if (bar == std::string_view::npos) {
        return std::nullopt;
    }
    return std::pair{std::string(trim(right.substr(0, bar))), std::string(trim(right.substr(bar + 1)))};
}

// The table and the environment fallback one expansion looks names up in (vcxproj.hpp).
struct Scope {
    const PropertyTable& properties;
    const EnvironmentLookup& environment; // Empty: the process environment.

    [[nodiscard]] std::optional<std::string> lookup(std::string_view name) const {
        if (auto it = properties.find(detail::toLowerAscii(name)); it != properties.end()) {
            return it->second;
        }
        return environment ? environment(name) : processEnvironmentVariable(name);
    }
};

// Expands $(Name) from the scope; nullopt when a name is unknown, is a property function or the
// text holds an item list @(...) or metadata %(...).
std::optional<std::string> expandProperties(std::string_view text, const Scope& scope) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const bool open = i + 1 < text.size() && text[i + 1] == '(';
        if (text[i] == '$' && open) {
            const std::size_t close = text.find(')', i + 2);
            if (close == std::string_view::npos) {
                return std::nullopt;
            }
            const std::string_view name = trim(text.substr(i + 2, close - i - 2));
            if (name.empty() || name.find_first_of("[.:(") != std::string_view::npos) {
                return std::nullopt;
            }
            auto value = scope.lookup(name);
            if (!value) {
                return std::nullopt;
            }
            out += *value;
            i = close + 1;
        } else if ((text[i] == '@' || text[i] == '%') && open) {
            return std::nullopt;
        } else {
            out += text[i++];
        }
    }
    return out;
}

// Expands the $(Name) references `properties` knows and keeps every other reference ($(Unknown),
// property functions, %(Metadata), @(Items)) as written; for ClCompile metadata (vcxproj.hpp).
std::string expandKnownProperties(std::string_view text, const PropertyTable& properties) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '(') {
            const std::size_t close = text.find(')', i + 2);
            if (close == std::string_view::npos) {
                out.append(text.substr(i));
                break;
            }
            const std::string_view name = trim(text.substr(i + 2, close - i - 2));
            auto it = properties.find(detail::toLowerAscii(name));
            if (it != properties.end() && name.find_first_of("[.:(") == std::string_view::npos) {
                out += it->second;
            } else {
                out.append(text.substr(i, close + 1 - i));
            }
            i = close + 1;
        } else {
            out += text[i++];
        }
    }
    return out;
}

// "Source Files/Core\\" -> "Source Files\Core".
std::string normalizeFilterPath(std::string_view filter) {
    std::string slashes(filter);
    for (char& c : slashes) {
        if (c == '/') {
            c = '\\';
        }
    }
    std::string out;
    for (std::string_view component : detail::splitTrimmed(slashes, '\\')) {
        if (!out.empty()) {
            out += '\\';
        }
        out += component;
    }
    return out;
}

// Resolves one Include entry (already split at ';') to the files it names, reporting problems
// through `warn` and skipped entries through `unresolved`.
template<class Warn>
std::vector<std::string> resolveInclude(io::IFileSystem& fs, std::string_view directory, std::string_view entry,
                                        const Scope& scope, std::vector<std::string>& unresolved, bool& wildcard,
                                        Warn&& warn) {
    wildcard = false;
    auto expanded = expandProperties(entry, scope);
    if (!expanded) {
        unresolved.emplace_back(entry);
        return {};
    }
    if (detail::hasWildcard(*expanded)) {
        wildcard = true;
        auto pattern = detail::PathPattern::make(directory, *expanded);
        if (!pattern) {
            warn(std::format("Include '{}' skipped: {}", entry, pattern.error().message()));
            return {};
        }
        auto expansion = detail::expandWildcard(fs, *pattern);
        if (!expansion) {
            warn(std::format("Include '{}' skipped: {}", entry, expansion.error().message()));
            return {};
        }
        if (!expansion->warning.empty()) {
            warn(std::format("Include '{}': {}", entry, expansion->warning));
        }
        return std::move(expansion->paths);
    }
    auto resolved = detail::resolvePath(directory, *expanded);
    if (!resolved) {
        warn(std::format("Include '{}' skipped: {}", entry, resolved.error().message()));
        return {};
    }
    return {std::move(*resolved)};
}

class VcxprojParser {
public:
    VcxprojParser(io::IFileSystem& fs, VcxProject& project, const VcxprojOptions& options)
        : m_fs(fs), m_project(project), m_scope{project.properties, options.environment} {}

    // Walks a <Project>, <When> or <Otherwise> element.
    void walk(const pugi::xml_node& parent) {
        for (pugi::xml_node child = parent.first_child(); child; child = child.next_sibling()) {
            if (isElement(child, "PropertyGroup")) {
                propertyGroup(child);
            } else if (isElement(child, "ItemGroup")) {
                itemGroup(child);
            } else if (isElement(child, "ItemDefinitionGroup")) {
                itemDefinitionGroup(child);
            } else if (isElement(child, "Choose")) {
                for (pugi::xml_node branch = child.first_child(); branch; branch = branch.next_sibling()) {
                    if (isElement(branch, "When") || isElement(branch, "Otherwise")) {
                        walk(branch);
                    }
                }
            }
        }
    }

    // Copies the per-configuration properties onto the ProjectConfiguration entries.
    void finish() {
        for (ProjectConfiguration& config : m_project.configurations) {
            for (const auto& [name, values] : m_perConfiguration) {
                if (iequals(name, config.name())) {
                    config.configurationType = values.configurationType;
                    config.platformToolset = values.platformToolset;
                    config.characterSet = values.characterSet;
                }
            }
        }
    }

private:
    struct PerConfiguration {
        std::string configurationType;
        std::string platformToolset;
        std::string characterSet;
    };

    void warn(std::string message) { m_project.warnings.push_back(std::move(message)); }

    static void setFirst(std::string& field, std::string_view value) {
        if (field.empty()) {
            field = std::string(value);
        }
    }

    PerConfiguration& perConfiguration(const std::pair<std::string, std::string>& key) {
        const std::string name = key.first + '|' + key.second;
        for (auto& [existing, values] : m_perConfiguration) {
            if (iequals(existing, name)) {
                return values;
            }
        }
        m_perConfiguration.emplace_back(name, PerConfiguration{});
        return m_perConfiguration.back().second;
    }

    void propertyGroup(const pugi::xml_node& group) {
        const std::string_view groupCondition = trim(attribute(group, "Condition"));
        const auto configuration = groupCondition.empty() ? std::nullopt : configurationCondition(groupCondition);
        for (pugi::xml_node property = group.first_child(); property; property = property.next_sibling()) {
            if (property.type() != pugi::node_element) {
                continue;
            }
            const std::string_view name = localName(property);
            const std::string_view value = elementText(property);
            const bool unconditioned = trim(attribute(property, "Condition")).empty();
            if (iequals(name, "ProjectGuid")) {
                setFirst(m_project.projectGuid, detail::normalizeGuid(value));
            } else if (iequals(name, "RootNamespace")) {
                setFirst(m_project.rootNamespace, value);
            } else if (iequals(name, "ConfigurationType")) {
                setFirst(m_project.configurationType, value);
                if (configuration && unconditioned) {
                    setFirst(perConfiguration(*configuration).configurationType, value);
                }
            } else if (iequals(name, "PlatformToolset")) {
                setFirst(m_project.platformToolset, value);
                if (configuration && unconditioned) {
                    setFirst(perConfiguration(*configuration).platformToolset, value);
                }
            } else if (iequals(name, "CharacterSet") && configuration && unconditioned) {
                setFirst(perConfiguration(*configuration).characterSet, value);
            }
            if (!groupCondition.empty() || !unconditioned || detail::istartsWith(name, "MSBuild")) {
                continue; // Only unconditioned, non-reserved properties join the static table.
            }
            const std::string key = detail::toLowerAscii(name);
            if (auto expanded = expandProperties(value, m_scope)) {
                m_project.properties[key] = std::move(*expanded);
            } else {
                m_project.properties.erase(key); // Redefined to something we cannot know statically.
            }
        }
    }

    // <ItemDefinitionGroup>: the <ClCompile> metadata, conditioned on the group or the element.
    void itemDefinitionGroup(const pugi::xml_node& group) {
        const std::string_view groupCondition = trim(attribute(group, "Condition"));
        for (pugi::xml_node item = group.first_child(); item; item = item.next_sibling()) {
            if (item.type() != pugi::node_element || !iequals(localName(item), "ClCompile")) {
                continue;
            }
            const std::string_view itemCondition = trim(attribute(item, "Condition"));
            ClCompileDefinition definition;
            if (!groupCondition.empty() && !itemCondition.empty()) {
                // Two conditions at once: not the standard shape, so it cannot be evaluated.
                definition.otherCondition = true;
                m_project.clCompileDefinitions.push_back(std::move(definition));
                continue;
            }
            const std::string_view condition = groupCondition.empty() ? itemCondition : groupCondition;
            if (!condition.empty()) {
                if (auto pair = configurationCondition(condition)) {
                    definition.configuration = pair->first + '|' + pair->second;
                } else {
                    definition.otherCondition = true;
                }
            }
            for (pugi::xml_node meta = item.first_child(); meta; meta = meta.next_sibling()) {
                if (meta.type() != pugi::node_element || !trim(attribute(meta, "Condition")).empty()) {
                    continue; // Metadata with its own condition cannot be evaluated statically.
                }
                definition.metadata[detail::toLowerAscii(localName(meta))] =
                    expandKnownProperties(elementText(meta), m_project.properties);
            }
            m_project.clCompileDefinitions.push_back(std::move(definition));
        }
    }

    void itemGroup(const pugi::xml_node& group) {
        for (pugi::xml_node item = group.first_child(); item; item = item.next_sibling()) {
            if (item.type() != pugi::node_element) {
                continue;
            }
            const std::string_view name = localName(item);
            if (iequals(name, "ProjectConfiguration")) {
                projectConfiguration(item);
            } else if (iequals(name, "ProjectReference")) {
                projectReference(item);
            } else if (auto type = fileItemType(name)) {
                fileItem(item, *type, name);
            }
        }
    }

    void projectConfiguration(const pugi::xml_node& item) {
        const std::string_view include = trim(attribute(item, "Include"));
        std::string configuration(elementText(detail::childElement(item, "Configuration")));
        std::string platform(elementText(detail::childElement(item, "Platform")));
        if (configuration.empty() || platform.empty()) {
            const std::size_t bar = include.find('|');
            if (bar == std::string_view::npos) {
                warn(std::format("ProjectConfiguration '{}' skipped: expected Configuration|Platform", include));
                return;
            }
            configuration = std::string(trim(include.substr(0, bar)));
            platform = std::string(trim(include.substr(bar + 1)));
        }
        ProjectConfiguration config{.configuration = std::move(configuration),
                                    .platform = std::move(platform),
                                    .configurationType = {},
                                    .platformToolset = {},
                                    .characterSet = {}};
        for (const ProjectConfiguration& existing : m_project.configurations) {
            if (iequals(existing.name(), config.name())) {
                return;
            }
        }
        m_project.configurations.push_back(std::move(config));
    }

    void projectReference(const pugi::xml_node& item) {
        const std::string guid = detail::normalizeGuid(elementText(detail::childElement(item, "Project")));
        for (std::string_view entry : detail::splitTrimmed(attribute(item, "Include"), ';')) {
            bool wildcard = false;
            auto paths = resolveInclude(m_fs, m_project.directory, entry, m_scope, m_project.unresolvedIncludes,
                                        wildcard, [this](std::string message) { warn(std::move(message)); });
            for (std::string& path : paths) {
                if (m_references.insert(detail::pathKey(path)).second) {
                    m_project.references.push_back(
                        {.include = std::string(entry), .path = std::move(path), .projectGuid = guid});
                }
            }
        }
    }

    void fileItem(const pugi::xml_node& item, ItemType type, std::string_view name) {
        const std::string_view include = attribute(item, "Include");
        if (trim(include).empty()) {
            return; // Remove= and Update= items name no new files.
        }
        std::vector<detail::PathPattern> excludes;
        for (std::string_view entry : detail::splitTrimmed(attribute(item, "Exclude"), ';')) {
            auto expanded = expandProperties(entry, m_scope);
            auto pattern = expanded ? detail::PathPattern::make(m_project.directory, *expanded)
                                    : Result<detail::PathPattern>(fail(ErrorCode::InvalidArgument, "unknown property"));
            if (pattern) {
                excludes.push_back(std::move(*pattern));
            } else {
                warn(std::format("Exclude '{}' ignored: {}", entry, pattern.error().message()));
            }
        }
        for (std::string_view entry : detail::splitTrimmed(include, ';')) {
            bool wildcard = false;
            auto paths = resolveInclude(m_fs, m_project.directory, entry, m_scope, m_project.unresolvedIncludes,
                                        wildcard, [this](std::string message) { warn(std::move(message)); });
            for (std::string& path : paths) {
                bool excluded = false;
                for (const detail::PathPattern& exclude : excludes) {
                    excluded = excluded || exclude.matches(path);
                }
                if (excluded || !m_items.insert(detail::pathKey(path)).second) {
                    continue;
                }
                m_project.items.push_back({.type = type,
                                           .itemName = std::string(name),
                                           .include = std::string(entry),
                                           .path = std::move(path),
                                           .fromWildcard = wildcard});
            }
        }
    }

    io::IFileSystem& m_fs;
    VcxProject& m_project;
    const Scope m_scope; // The project's property table, then the environment.
    std::vector<std::pair<std::string, PerConfiguration>> m_perConfiguration;
    std::unordered_set<std::string> m_items;      // Path keys of m_project.items.
    std::unordered_set<std::string> m_references; // Path keys of m_project.references.
};

// The reserved and well-known properties every project starts with (vcxproj.hpp).
void seedProperties(VcxProject& project, const VcxprojOptions& options) {
    const std::string_view file = io::fileName(project.path);
    const std::string name(detail::stem(file));
    const std::string dirWithSeparator = project.directory.back() == detail::kNativeSeparator
                                             ? project.directory
                                             : project.directory + detail::kNativeSeparator;
    PropertyTable& p = project.properties;
    p["msbuildprojectdirectory"] = project.directory;
    p["msbuildthisfiledirectory"] = dirWithSeparator;
    p["msbuildprojectname"] = name;
    p["msbuildprojectfile"] = std::string(file);
    p["msbuildprojectfullpath"] = project.path;
    p["msbuildprojectextension"] = detail::lowerExtension(file);
    p["projectdir"] = dirWithSeparator;
    p["projectname"] = name;
    p["projectfilename"] = std::string(file);
    p["projectpath"] = project.path;
    if (!options.solutionDirectory.empty()) {
        const std::string& dir = options.solutionDirectory;
        p["solutiondir"] = dir.back() == detail::kNativeSeparator ? dir : dir + detail::kNativeSeparator;
    }
}

} // namespace

std::optional<std::string> processEnvironmentVariable(std::string_view name) {
    if (name.empty() || name.find_first_of(std::string_view("=\0", 2)) != std::string_view::npos) {
        return std::nullopt;
    }
#ifdef _WIN32
    auto wideName = io::toWide(name);
    if (!wideName) {
        return std::nullopt;
    }
    std::wstring value(256, L' ');
    for (;;) {
        const DWORD length = GetEnvironmentVariableW(wideName->c_str(), value.data(), static_cast<DWORD>(value.size()));
        if (length == 0) {
            return std::nullopt; // Unset (an empty variable cannot exist on Windows).
        }
        if (length < value.size()) {
            value.resize(length);
            auto utf8 = io::fromWide(value);
            return utf8 ? std::optional<std::string>(std::move(*utf8)) : std::nullopt;
        }
        value.resize(length); // Too small: `length` is the size needed, terminator included.
    }
#else
    const std::string key(name);
    const char* value = std::getenv(key.c_str()); // NOLINT(concurrency-mt-unsafe): read-only use.
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
#endif
}

std::string_view toString(ItemType type) noexcept {
    for (const ItemTypeName& entry : kItemTypes) {
        if (entry.type == type && type != ItemType::Other) {
            return entry.name;
        }
    }
    return "Other";
}

Result<VcxProject> parseVcxproj(io::IFileSystem& fs, std::string_view content, std::string_view projectPath,
                                const VcxprojOptions& options) {
    auto path = detail::normalizeAbsolute(projectPath);
    if (!path) {
        return std::unexpected(std::move(path.error()));
    }
    pugi::xml_document doc;
    if (auto parsed = detail::parseXml(doc, content, *path, "Project"); !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    VcxProject project;
    project.directory = std::string(io::parentPath(*path));
    project.path = std::move(*path);
    seedProperties(project, options);
    VcxprojParser parser(fs, project, options);
    parser.walk(doc.document_element());
    parser.finish();
    return project;
}

std::map<std::string, std::string, std::less<>> clCompileMetadata(const VcxProject& project,
                                                                  std::string_view configuration) {
    std::map<std::string, std::string, std::less<>> folded;
    for (const ClCompileDefinition& definition : project.clCompileDefinitions) {
        if (definition.otherCondition ||
            (!definition.configuration.empty() && !iequals(definition.configuration, configuration))) {
            continue;
        }
        for (const auto& [name, value] : definition.metadata) {
            // Replace every %(Name) of this metadata (any case) with the inherited value.
            auto previous = folded.find(name);
            const std::string inherited = previous != folded.end() ? previous->second : std::string();
            std::string out;
            std::size_t i = 0;
            while (i < value.size()) {
                if (value[i] == '%' && i + 1 < value.size() && value[i + 1] == '(') {
                    const std::size_t close = value.find(')', i + 2);
                    if (close != std::string::npos &&
                        iequals(trim(std::string_view(value).substr(i + 2, close - i - 2)), name)) {
                        out += inherited;
                        i = close + 1;
                        continue;
                    }
                }
                out += value[i++];
            }
            folded[name] = std::move(out);
        }
    }
    return folded;
}

Result<std::map<std::string, std::string, std::less<>>> parseConfigurationProperties(std::string_view content,
                                                                                     std::string_view path,
                                                                                     std::string_view configuration,
                                                                                     std::string_view platform) {
    pugi::xml_document doc;
    if (auto parsed = detail::parseXml(doc, content, path, "Project"); !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    PropertyTable values;
    for (pugi::xml_node group = doc.document_element().first_child(); group; group = group.next_sibling()) {
        if (!isElement(group, "PropertyGroup")) {
            continue;
        }
        const std::string_view condition = trim(attribute(group, "Condition"));
        if (!condition.empty()) {
            const auto pair = configurationCondition(condition);
            if (!pair || !iequals(pair->first, configuration) || !iequals(pair->second, platform)) {
                continue;
            }
        }
        for (pugi::xml_node property = group.first_child(); property; property = property.next_sibling()) {
            if (property.type() != pugi::node_element || !trim(attribute(property, "Condition")).empty()) {
                continue;
            }
            values[detail::toLowerAscii(localName(property))] = std::string(elementText(property));
        }
    }
    return values;
}

Result<VcxProject> readVcxproj(io::IFileSystem& fs, std::string_view projectPath, const VcxprojOptions& options) {
    auto content = fs.readFile(projectPath);
    if (!content) {
        return std::unexpected(std::move(content.error()));
    }
    return parseVcxproj(fs, *content, projectPath, options);
}

Result<FiltersFile> parseVcxprojFilters(io::IFileSystem& fs, std::string_view content, const VcxProject& project,
                                        const VcxprojOptions& options) {
    const std::string path = project.path + ".filters";
    pugi::xml_document doc;
    if (auto parsed = detail::parseXml(doc, content, path, "Project"); !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    FiltersFile out;
    std::unordered_set<std::string> filterKeys;
    std::unordered_set<std::string> itemKeys;
    std::vector<std::string> unresolved;
    const Scope scope{project.properties, options.environment};
    auto warn = [&out](std::string message) { out.warnings.push_back(std::move(message)); };
    for (pugi::xml_node group = doc.document_element().first_child(); group; group = group.next_sibling()) {
        if (!isElement(group, "ItemGroup")) {
            continue;
        }
        for (pugi::xml_node item = group.first_child(); item; item = item.next_sibling()) {
            if (item.type() != pugi::node_element) {
                continue;
            }
            const std::string_view include = attribute(item, "Include");
            if (isElement(item, "Filter")) {
                std::string filter = normalizeFilterPath(include);
                if (!filter.empty() && filterKeys.insert(detail::toLowerAscii(filter)).second) {
                    out.filters.push_back(
                        {.path = std::move(filter),
                         .uniqueIdentifier = std::string(elementText(detail::childElement(item, "UniqueIdentifier"))),
                         .extensions = std::string(elementText(detail::childElement(item, "Extensions")))});
                }
                continue;
            }
            const std::string filter = normalizeFilterPath(elementText(detail::childElement(item, "Filter")));
            for (std::string_view entry : detail::splitTrimmed(include, ';')) {
                bool wildcard = false;
                auto paths = resolveInclude(fs, project.directory, entry, scope, unresolved, wildcard, warn);
                for (std::string& itemPath : paths) {
                    if (itemKeys.insert(detail::pathKey(itemPath)).second) {
                        out.assignments.push_back({.itemPath = std::move(itemPath), .filter = filter});
                    }
                }
            }
        }
    }
    for (const std::string& entry : unresolved) {
        out.warnings.push_back(std::format("Include '{}' names an unknown property", entry));
    }
    return out;
}

Result<FiltersFile> readVcxprojFilters(io::IFileSystem& fs, const VcxProject& project, const VcxprojOptions& options) {
    auto content = fs.readFile(project.path + ".filters");
    if (!content) {
        if (content.error().code() == ErrorCode::NotFound) {
            return FiltersFile{};
        }
        return std::unexpected(std::move(content.error()));
    }
    return parseVcxprojFilters(fs, *content, project, options);
}

} // namespace frost::project
