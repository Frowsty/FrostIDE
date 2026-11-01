// SPDX-License-Identifier: Apache-2.0
//
// The static .sln reader (solution_file.hpp): a line-based state machine over the text format
// Visual Studio writes ("Format Version 12.00"), plus readSolutionFile's dispatch by extension.
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/solution_file.hpp"

#include "paths.hpp"
#include "text_util.hpp"

#include <format>
#include <optional>
#include <unordered_map>

namespace frost::project {
namespace {

using detail::iequals;
using detail::istartsWith;
using detail::normalizeGuid;
using detail::trim;

constexpr std::string_view kHeader = "Microsoft Visual Studio Solution File, Format Version ";

// Parses `"a", "b"` style quoted fields: returns the next quoted string after `pos` and moves
// `pos` past it, or nullopt when no complete quoted string follows.
std::optional<std::string_view> nextQuoted(std::string_view line, std::size_t& pos) {
    const std::size_t open = line.find('"', pos);
    if (open == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t close = line.find('"', open + 1);
    if (close == std::string_view::npos) {
        return std::nullopt;
    }
    pos = close + 1;
    return line.substr(open + 1, close - open - 1);
}

// "key = value" split at the first '='; both sides trimmed. nullopt without '='.
std::optional<std::pair<std::string_view, std::string_view>> keyValue(std::string_view line) {
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
        return std::nullopt;
    }
    return std::pair{trim(line.substr(0, eq)), trim(line.substr(eq + 1))};
}

// "Debug|x64" -> {Debug, x64}; a missing '|' leaves the platform empty.
std::pair<std::string, std::string> splitConfiguration(std::string_view text) {
    const std::size_t bar = text.find('|');
    if (bar == std::string_view::npos) {
        return {std::string(trim(text)), std::string()};
    }
    return {std::string(trim(text.substr(0, bar))), std::string(trim(text.substr(bar + 1)))};
}

enum class Section : unsigned char {
    None,
    Project,             // Inside Project(...) ... EndProject, outside any ProjectSection.
    ProjectDependencies, // ProjectSection(ProjectDependencies)
    SolutionItems,       // ProjectSection(SolutionItems)
    OtherProjectSection,
    Global,
    SolutionConfigurations, // GlobalSection(SolutionConfigurationPlatforms)
    ProjectConfigurations,  // GlobalSection(ProjectConfigurationPlatforms)
    NestedProjects,         // GlobalSection(NestedProjects)
    OtherGlobalSection,
};

// The parenthesised name of "ProjectSection(Name) = ..." or "GlobalSection(Name) = ...".
std::string_view sectionName(std::string_view line) {
    const std::size_t open = line.find('(');
    const std::size_t close = line.find(')', open == std::string_view::npos ? 0 : open);
    if (open == std::string_view::npos || close == std::string_view::npos) {
        return {};
    }
    return trim(line.substr(open + 1, close - open - 1));
}

class SlnParser {
public:
    explicit SlnParser(SolutionFile& file) : m_file(file) {}

    void line(std::string_view text, std::size_t number) {
        const std::string_view l = trim(text);
        if (l.empty() || l.front() == '#') {
            return;
        }
        switch (m_section) {
        case Section::None: topLevel(l, number); break;
        case Section::Project: inProject(l, number); break;
        case Section::ProjectDependencies:
        case Section::SolutionItems:
        case Section::OtherProjectSection: inProjectSection(l); break;
        case Section::Global: inGlobal(l, number); break;
        case Section::SolutionConfigurations:
        case Section::ProjectConfigurations:
        case Section::NestedProjects:
        case Section::OtherGlobalSection: inGlobalSection(l); break;
        }
    }

    void finish() {
        if (m_section != Section::None) {
            warn("the file ends inside a section");
        }
        if (m_current) {
            commitProject();
        }
        applyConfigurations();
        applyNesting();
    }

private:
    struct PendingProject {
        bool folder = false;
        SolutionProjectEntry project;
        SolutionFolderEntry folderEntry;
    };

    void warn(std::string message) { m_file.warnings.push_back(std::move(message)); }

    void topLevel(std::string_view l, std::size_t number) {
        if (istartsWith(l, "Project(")) {
            startProject(l, number);
        } else if (iequals(l, "Global")) {
            m_section = Section::Global;
        } else if (istartsWith(l, "VisualStudioVersion")) {
            if (auto kv = keyValue(l)) {
                m_file.visualStudioVersion = std::string(kv->second);
            }
        }
        // MinimumVisualStudioVersion and unknown lines are ignored.
    }

    void startProject(std::string_view l, std::size_t number) {
        std::size_t pos = 0;
        auto type = nextQuoted(l, pos);
        auto name = nextQuoted(l, pos);
        auto path = nextQuoted(l, pos);
        auto guid = nextQuoted(l, pos);
        m_section = Section::Project;
        if (!type || !name || !path || !guid) {
            warn(std::format("line {}: malformed Project line skipped", number));
            m_current.reset();
            return;
        }
        PendingProject pending;
        const std::string typeGuid = normalizeGuid(*type);
        const std::string projectGuid = normalizeGuid(*guid);
        if (typeGuid == kSolutionFolderTypeGuid) {
            pending.folder = true;
            pending.folderEntry.id = projectGuid;
            pending.folderEntry.name = std::string(*name);
        } else {
            SolutionProjectEntry& p = pending.project;
            p.name = std::string(*name);
            p.relativePath = std::string(*path);
            p.typeGuid = typeGuid;
            p.projectGuid = projectGuid;
            auto resolved = detail::resolvePath(m_file.directory, *path);
            if (resolved) {
                p.path = std::move(*resolved);
            } else {
                warn(std::format("line {}: project '{}' has an unusable path '{}': {}", number, *name, *path,
                                 resolved.error().message()));
            }
        }
        m_current = std::move(pending);
    }

    void inProject(std::string_view l, std::size_t number) {
        if (iequals(l, "EndProject")) {
            commitProject();
            m_section = Section::None;
        } else if (istartsWith(l, "ProjectSection(")) {
            const std::string_view name = sectionName(l);
            if (iequals(name, "ProjectDependencies")) {
                m_section = Section::ProjectDependencies;
            } else if (iequals(name, "SolutionItems")) {
                m_section = Section::SolutionItems;
            } else {
                m_section = Section::OtherProjectSection;
            }
        } else if (istartsWith(l, "Project(")) {
            warn(std::format("line {}: Project without EndProject", number));
            commitProject();
            startProject(l, number);
        }
    }

    void inProjectSection(std::string_view l) {
        if (iequals(l, "EndProjectSection")) {
            m_section = Section::Project;
            return;
        }
        if (!m_current) {
            return;
        }
        auto kv = keyValue(l);
        if (!kv || kv->first.empty()) {
            return;
        }
        if (m_section == Section::ProjectDependencies && !m_current->folder) {
            m_current->project.dependencies.push_back(normalizeGuid(kv->first));
        } else if (m_section == Section::SolutionItems && m_current->folder) {
            auto resolved = detail::resolvePath(m_file.directory, kv->first);
            if (resolved) {
                m_current->folderEntry.files.push_back(std::move(*resolved));
            } else {
                warn(std::format("solution item '{}' skipped: {}", kv->first, resolved.error().message()));
            }
        }
    }

    void commitProject() {
        if (!m_current) {
            return;
        }
        if (m_current->folder) {
            m_folderIndex[m_current->folderEntry.id] = m_file.folders.size();
            m_file.folders.push_back(std::move(m_current->folderEntry));
        } else {
            m_projectIndex[m_current->project.projectGuid] = m_file.projects.size();
            m_file.projects.push_back(std::move(m_current->project));
        }
        m_current.reset();
    }

    void inGlobal(std::string_view l, std::size_t number) {
        if (iequals(l, "EndGlobal")) {
            m_section = Section::None;
        } else if (istartsWith(l, "GlobalSection(")) {
            const std::string_view name = sectionName(l);
            if (iequals(name, "SolutionConfigurationPlatforms")) {
                m_section = Section::SolutionConfigurations;
            } else if (iequals(name, "ProjectConfigurationPlatforms")) {
                m_section = Section::ProjectConfigurations;
            } else if (iequals(name, "NestedProjects")) {
                m_section = Section::NestedProjects;
            } else {
                m_section = Section::OtherGlobalSection;
            }
        } else {
            warn(std::format("line {}: unexpected line in Global ignored", number));
        }
    }

    void inGlobalSection(std::string_view l) {
        if (iequals(l, "EndGlobalSection")) {
            m_section = Section::Global;
            return;
        }
        auto kv = keyValue(l);
        if (!kv) {
            return;
        }
        if (m_section == Section::SolutionConfigurations) {
            auto [configuration, platform] = splitConfiguration(kv->first);
            SolutionConfiguration config{.configuration = std::move(configuration), .platform = std::move(platform)};
            bool known = false;
            for (const SolutionConfiguration& c : m_file.configurations) {
                known = known || iequals(c.name(), config.name());
            }
            if (!known && !config.configuration.empty()) {
                m_file.configurations.push_back(std::move(config));
            }
        } else if (m_section == Section::ProjectConfigurations) {
            m_projectConfigurationLines.emplace_back(kv->first, kv->second);
        } else if (m_section == Section::NestedProjects) {
            m_nesting.emplace_back(normalizeGuid(kv->first), normalizeGuid(kv->second));
        }
    }

    // "{GUID}.Debug|x64.ActiveCfg = Debug|Win32" lines, applied once every project is known (the
    // global section comes after the projects, but nothing requires that order).
    void applyConfigurations() {
        for (const auto& [key, value] : m_projectConfigurationLines) {
            const std::size_t close = key.find('}');
            if (close == std::string::npos || close + 1 >= key.size() || key[close + 1] != '.') {
                continue;
            }
            const std::string guid = normalizeGuid(key.substr(0, close + 1));
            std::string_view rest = std::string_view(key).substr(close + 2);
            enum class What : unsigned char { ActiveCfg, Build, Deploy } what{};
            if (detail::iendsWith(rest, ".ActiveCfg")) {
                what = What::ActiveCfg;
                rest.remove_suffix(std::string_view(".ActiveCfg").size());
            } else if (detail::iendsWith(rest, ".Build.0")) {
                what = What::Build;
                rest.remove_suffix(std::string_view(".Build.0").size());
            } else if (detail::iendsWith(rest, ".Deploy.0")) {
                what = What::Deploy;
                rest.remove_suffix(std::string_view(".Deploy.0").size());
            } else {
                continue;
            }
            auto it = m_projectIndex.find(guid);
            if (it == m_projectIndex.end()) {
                continue; // A configuration line for a project the file does not list.
            }
            auto& entries = m_file.projects[it->second].slnConfigurations;
            ProjectConfigurationMapping* mapping = nullptr;
            for (auto& [solutionConfig, m] : entries) {
                if (iequals(solutionConfig, rest)) {
                    mapping = &m;
                }
            }
            if (!mapping) {
                entries.emplace_back(std::string(rest), ProjectConfigurationMapping{});
                mapping = &entries.back().second;
            }
            if (what == What::ActiveCfg || mapping->configuration.empty()) {
                auto [configuration, platform] = splitConfiguration(value);
                mapping->configuration = std::move(configuration);
                mapping->platform = std::move(platform);
            }
            if (what == What::Build) {
                mapping->build = true;
            } else if (what == What::Deploy) {
                mapping->deploy = true;
            }
        }
    }

    // NestedProjects: "{child} = {parent}". Cycles are left to SolutionModel, which checks them.
    void applyNesting() {
        for (const auto& [child, parent] : m_nesting) {
            if (!m_folderIndex.contains(parent)) {
                warn(std::format("NestedProjects names an unknown parent folder {}", parent));
                continue;
            }
            if (auto f = m_folderIndex.find(child); f != m_folderIndex.end()) {
                m_file.folders[f->second].parentId = parent;
            } else if (auto p = m_projectIndex.find(child); p != m_projectIndex.end()) {
                m_file.projects[p->second].parentId = parent;
            } else {
                warn(std::format("NestedProjects names an unknown entry {}", child));
            }
        }
    }

    SolutionFile& m_file;
    Section m_section = Section::None;
    std::optional<PendingProject> m_current;
    std::unordered_map<std::string, std::size_t> m_folderIndex;  // Folder GUID -> index.
    std::unordered_map<std::string, std::size_t> m_projectIndex; // Project GUID -> index.
    std::vector<std::pair<std::string, std::string>> m_projectConfigurationLines;
    std::vector<std::pair<std::string, std::string>> m_nesting;
};

} // namespace

Result<SolutionFile> parseSln(std::string_view content, std::string_view solutionPath) {
    auto path = detail::normalizeAbsolute(solutionPath);
    if (!path) {
        return std::unexpected(std::move(path.error()));
    }
    auto text = detail::decodeText(content, *path);
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    SolutionFile file;
    file.format = SolutionFormat::Sln;
    file.directory = std::string(io::parentPath(*path));
    file.path = std::move(*path);

    SlnParser parser(file);
    bool header = false;
    std::size_t number = 0;
    std::size_t start = 0;
    const std::string_view all = *text;
    while (start < all.size()) {
        std::size_t end = all.find('\n', start);
        if (end == std::string_view::npos) {
            end = all.size();
        }
        const std::string_view l = all.substr(start, end - start);
        start = end + 1;
        ++number;
        if (!header) {
            const std::string_view t = trim(l);
            if (t.empty()) {
                continue;
            }
            if (!istartsWith(t, kHeader)) {
                return fail(ErrorCode::Corrupt, std::format("{}: not a Visual Studio solution file (line {} is not "
                                                            "the 'Format Version' header)",
                                                            file.path, number));
            }
            file.formatVersion = std::string(trim(t.substr(kHeader.size())));
            header = true;
            continue;
        }
        parser.line(l, number);
    }
    if (!header) {
        return fail(ErrorCode::Corrupt, std::format("{}: empty solution file", file.path));
    }
    parser.finish();
    return file;
}

Result<SolutionFile> readSolutionFile(io::IFileSystem& fs, std::string_view solutionPath) {
    const std::string extension = detail::lowerExtension(io::fileName(solutionPath));
    if (extension != ".sln" && extension != ".slnx") {
        return fail(ErrorCode::Unsupported,
                    std::format("{}: not a solution file (expected .sln or .slnx)", solutionPath));
    }
    auto content = fs.readFile(solutionPath);
    if (!content) {
        return std::unexpected(std::move(content.error()));
    }
    return extension == ".sln" ? parseSln(*content, solutionPath) : parseSlnx(*content, solutionPath);
}

} // namespace frost::project
