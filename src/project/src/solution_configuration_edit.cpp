// SPDX-License-Identifier: Apache-2.0
//
// Solution configuration mapping edits (frost/project/solution_configuration_edit.hpp, SLN-7):
// line edits of a .sln's ProjectConfigurationPlatforms section, rule edits of a .slnx found by a
// small tag scanner that keeps every byte it does not change, and the verified write.

#include "frost/project/solution_configuration_edit.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include "paths.hpp"
#include "text_util.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <tuple>

namespace frost::project {
namespace {

using detail::iequals;

Result<void> checkEdit(const SolutionModel& model, const ConfigurationMappingEdit& edit) {
    if (edit.project.value() >= model.projects().size()) {
        return fail(ErrorCode::NotFound, std::format("project {} is not in the solution", edit.project.value()));
    }
    if (edit.solutionConfiguration >= model.configurations().size()) {
        return fail(ErrorCode::NotFound,
                    std::format("solution configuration {} does not exist", edit.solutionConfiguration));
    }
    if (edit.mapping && (edit.mapping->configuration.empty() || edit.mapping->platform.empty())) {
        return fail(ErrorCode::InvalidArgument, "a mapping needs a project configuration and platform");
    }
    return {};
}

// --- .sln ---

struct Line {
    std::string text;
    std::string ending; // "\r\n", "\n" or "" for a last line without one.
};

std::vector<Line> splitLines(std::string_view content) {
    std::vector<Line> lines;
    std::size_t start = 0;
    while (start < content.size()) {
        const std::size_t newline = content.find('\n', start);
        if (newline == std::string_view::npos) {
            lines.push_back({std::string(content.substr(start)), ""});
            break;
        }
        const bool crlf = newline > start && content[newline - 1] == '\r';
        lines.push_back({std::string(content.substr(start, newline - start - (crlf ? 1 : 0))), crlf ? "\r\n" : "\n"});
        start = newline + 1;
    }
    return lines;
}

// One ProjectConfigurationPlatforms line, split.
struct SlnEntry {
    std::string guid;          // As written.
    std::string configuration; // The solution configuration as written.
    std::string property;      // "ActiveCfg", "Build.0", "Deploy.0" or another.
};

std::optional<SlnEntry> parseEntry(std::string_view line) {
    const std::string_view trimmed = detail::trim(line);
    const std::size_t equals = trimmed.find('=');
    if (!trimmed.starts_with('{') || equals == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view key = detail::trim(trimmed.substr(0, equals));
    const std::size_t close = key.find('}');
    if (close == std::string_view::npos || close + 1 >= key.size() || key[close + 1] != '.') {
        return std::nullopt;
    }
    const std::string_view rest = key.substr(close + 2);
    for (std::string_view property :
         {std::string_view("ActiveCfg"), std::string_view("Build.0"), std::string_view("Deploy.0")}) {
        if (rest.size() > property.size() + 1 && rest.ends_with(property) &&
            rest[rest.size() - property.size() - 1] == '.') {
            return SlnEntry{std::string(key.substr(0, close + 1)),
                            std::string(rest.substr(0, rest.size() - property.size() - 1)), std::string(property)};
        }
    }
    return SlnEntry{std::string(key.substr(0, close + 1)), std::string(rest), std::string()};
}

// The line with its value (after '=') replaced, the spacing before the value kept as " ".
std::string withValue(std::string_view line, std::string_view value) {
    const std::size_t equals = line.find('=');
    return std::string(line.substr(0, equals + 1)) + " " + std::string(value);
}

} // namespace

Result<std::string> applySlnConfigurationEdits(std::string_view content, const SolutionModel& model,
                                               std::span<const ConfigurationMappingEdit> edits) {
    if (content.starts_with("\xFF\xFE") || content.starts_with("\xFE\xFF")) {
        return fail(ErrorCode::Unsupported, "UTF-16 solution files are not edited; save the solution as UTF-8 first");
    }
    std::vector<Line> lines = splitLines(content);
    const std::string eol = !lines.empty() && !lines.front().ending.empty() ? lines.front().ending : "\r\n";
    // The section bounds, found again after every change.
    const auto section = [&lines]() -> std::optional<std::pair<std::size_t, std::size_t>> {
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (detail::trim(lines[i].text).starts_with("GlobalSection(ProjectConfigurationPlatforms)")) {
                for (std::size_t j = i + 1; j < lines.size(); ++j) {
                    if (detail::trim(lines[j].text) == "EndGlobalSection") {
                        return std::pair{i, j};
                    }
                }
                return std::nullopt;
            }
        }
        return std::nullopt;
    };
    for (const ConfigurationMappingEdit& edit : edits) {
        if (auto checked = checkEdit(model, edit); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        const ProjectInfo& info = model.project(edit.project);
        if (info.projectGuid.empty()) {
            return fail(ErrorCode::NotFound, std::format("{} has no project GUID in the solution", info.name));
        }
        const std::string configurationName = model.configurations()[edit.solutionConfiguration].name();
        const std::string guidKey = detail::normalizeGuid(info.projectGuid);
        struct Found {
            std::optional<std::size_t> active, build, deploy, last;
            std::string guid; // As the file writes it.
            std::string indent;
            std::size_t end = 0;
        };
        const auto scan = [&]() -> Result<Found> {
            const auto bounds = section();
            if (!bounds) {
                return fail(ErrorCode::Corrupt, "the solution has no ProjectConfigurationPlatforms section");
            }
            Found found;
            found.end = bounds->second;
            found.guid = info.projectGuid;
            found.indent = "\t\t";
            for (std::size_t i = bounds->first + 1; i < bounds->second; ++i) {
                const std::string& text = lines[i].text;
                if (i == bounds->first + 1) {
                    found.indent = text.substr(0, text.find_first_not_of(" \t"));
                }
                const auto entry = parseEntry(text);
                if (!entry || detail::normalizeGuid(entry->guid) != guidKey) {
                    continue;
                }
                found.guid = entry->guid;
                found.last = i;
                if (!iequals(entry->configuration, configurationName)) {
                    continue;
                }
                if (entry->property == "ActiveCfg") {
                    found.active = i;
                } else if (entry->property == "Build.0") {
                    found.build = i;
                } else if (entry->property == "Deploy.0") {
                    found.deploy = i;
                }
            }
            return found;
        };
        const auto lineFor = [&](const Found& found, std::string_view property, std::string_view value) {
            return Line{std::format("{}{}.{}.{} = {}", found.indent, found.guid, configurationName, property, value),
                        eol};
        };
        auto found = scan();
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        if (!edit.mapping) {
            std::vector<std::size_t> remove;
            for (const auto& index : {found->active, found->build, found->deploy}) {
                if (index) {
                    remove.push_back(*index);
                }
            }
            std::ranges::sort(remove, std::greater<>());
            for (const std::size_t index : remove) {
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index));
            }
            continue;
        }
        const std::string value = edit.mapping->configuration + "|" + edit.mapping->platform;
        if (found->active) {
            lines[*found->active].text = withValue(lines[*found->active].text, value);
        } else {
            const std::size_t at = found->last ? *found->last + 1 : found->end;
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), lineFor(*found, "ActiveCfg", value));
        }
        // Build.0 then Deploy.0, each placed after the line before it in Visual Studio's order.
        for (const auto& [property, wanted] : {std::pair{std::string_view("Build.0"), edit.mapping->build},
                                               std::pair{std::string_view("Deploy.0"), edit.mapping->deploy}}) {
            found = scan();
            if (!found) {
                return std::unexpected(std::move(found.error()));
            }
            const std::optional<std::size_t> existing = property == "Build.0" ? found->build : found->deploy;
            if (existing && !wanted) {
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(*existing));
            } else if (existing) {
                lines[*existing].text = withValue(lines[*existing].text, value);
            } else if (wanted) {
                const std::size_t after = property == "Deploy.0" && found->build ? *found->build : *found->active;
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(after + 1), lineFor(*found, property, value));
            }
        }
    }
    std::string out;
    out.reserve(content.size() + 256);
    for (const Line& line : lines) {
        out += line.text;
        out += line.ending;
    }
    return out;
}

// --- .slnx ---

namespace {

struct Attribute {
    std::string name;
    std::string value;          // Entities decoded.
    std::size_t valueBegin = 0; // The raw value's bytes, inside the quotes.
    std::size_t valueEnd = 0;
};

struct Tag {
    std::size_t begin = 0; // '<'
    std::size_t end = 0;   // One past '>'
    std::string name;      // Local name.
    bool closing = false;
    bool selfClosing = false;
    std::vector<Attribute> attributes;

    [[nodiscard]] const Attribute* attribute(std::string_view wanted) const {
        for (const Attribute& a : attributes) {
            if (iequals(a.name, wanted)) {
                return &a;
            }
        }
        return nullptr;
    }
};

std::string decodeEntities(std::string_view raw) {
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '&') {
            out += raw[i];
            continue;
        }
        static constexpr std::array<std::pair<std::string_view, char>, 5> kEntities{
            {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}}};
        bool decoded = false;
        for (const auto& [entity, c] : kEntities) {
            if (raw.substr(i).starts_with(entity)) {
                out += c;
                i += entity.size() - 1;
                decoded = true;
                break;
            }
        }
        if (!decoded) {
            out += '&';
        }
    }
    return out;
}

std::string encodeAttribute(std::string_view value) {
    std::string out;
    for (const char c : value) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

// Every element tag of `text` in order (comments, processing instructions, CDATA and the
// doctype skipped). Errors: Corrupt (an unterminated construct).
Result<std::vector<Tag>> scanTags(std::string_view text) {
    std::vector<Tag> tags;
    std::size_t i = 0;
    const auto skipTo = [&](std::string_view terminator) -> bool {
        const std::size_t found = text.find(terminator, i);
        if (found == std::string_view::npos) {
            return false;
        }
        i = found + terminator.size();
        return true;
    };
    while ((i = text.find('<', i)) != std::string_view::npos) {
        const std::string_view rest = text.substr(i);
        if (rest.starts_with("<!--")) {
            if (!skipTo("-->")) {
                return fail(ErrorCode::Corrupt, "an unterminated XML comment");
            }
            continue;
        }
        if (rest.starts_with("<![CDATA[")) {
            if (!skipTo("]]>")) {
                return fail(ErrorCode::Corrupt, "an unterminated CDATA section");
            }
            continue;
        }
        if (rest.starts_with("<?") || rest.starts_with("<!")) {
            if (!skipTo(">")) {
                return fail(ErrorCode::Corrupt, "an unterminated XML declaration");
            }
            continue;
        }
        Tag tag;
        tag.begin = i;
        std::size_t j = i + 1;
        if (j < text.size() && text[j] == '/') {
            tag.closing = true;
            ++j;
        }
        const std::size_t nameBegin = j;
        while (j < text.size() && text[j] != '>' && text[j] != '/' && text[j] != ' ' && text[j] != '\t' &&
               text[j] != '\r' && text[j] != '\n') {
            ++j;
        }
        std::string_view name = text.substr(nameBegin, j - nameBegin);
        if (const std::size_t colon = name.find(':'); colon != std::string_view::npos) {
            name = name.substr(colon + 1);
        }
        tag.name = std::string(name);
        for (;;) {
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r' || text[j] == '\n')) {
                ++j;
            }
            if (j >= text.size()) {
                return fail(ErrorCode::Corrupt, "an unterminated XML tag");
            }
            if (text[j] == '>') {
                tag.end = j + 1;
                break;
            }
            if (text[j] == '/' && j + 1 < text.size() && text[j + 1] == '>') {
                tag.selfClosing = true;
                tag.end = j + 2;
                break;
            }
            const std::size_t attributeBegin = j;
            while (j < text.size() && text[j] != '=' && text[j] != '>' && text[j] != ' ' && text[j] != '/') {
                ++j;
            }
            Attribute attribute;
            attribute.name = std::string(detail::trim(text.substr(attributeBegin, j - attributeBegin)));
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) {
                ++j;
            }
            if (j >= text.size() || text[j] != '=') {
                if (attribute.name.empty()) {
                    return fail(ErrorCode::Corrupt, "a malformed XML tag");
                }
                continue; // An attribute without a value (not XML; tolerated, kept as written).
            }
            ++j;
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) {
                ++j;
            }
            if (j >= text.size() || (text[j] != '"' && text[j] != '\'')) {
                return fail(ErrorCode::Corrupt, "an unquoted XML attribute");
            }
            const char quote = text[j];
            const std::size_t close = text.find(quote, j + 1);
            if (close == std::string_view::npos) {
                return fail(ErrorCode::Corrupt, "an unterminated XML attribute");
            }
            attribute.valueBegin = j + 1;
            attribute.valueEnd = close;
            attribute.value = decodeEntities(text.substr(j + 1, close - j - 1));
            tag.attributes.push_back(std::move(attribute));
            j = close + 1;
        }
        i = tag.end;
        tags.push_back(std::move(tag));
    }
    return tags;
}

std::string_view ruleName(ConfigurationRuleKind kind) {
    switch (kind) {
    case ConfigurationRuleKind::BuildType: return "BuildType";
    case ConfigurationRuleKind::Platform: return "Platform";
    case ConfigurationRuleKind::Build: return "Build";
    case ConfigurationRuleKind::Deploy: return "Deploy";
    }
    return "BuildType";
}

// The .slnx pattern match of solution_model.cpp's .slnx mapping: "Debug|*", "*|x64", "Debug", "".
bool patternMatches(std::string_view pattern, const SolutionConfiguration& config) {
    pattern = detail::trim(pattern);
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

// The whitespace that indents the line `offset` is on, if only whitespace precedes it there.
std::string indentationAt(std::string_view text, std::size_t offset) {
    const std::size_t lineStart = text.rfind('\n', offset == 0 ? 0 : offset - 1);
    const std::size_t from = lineStart == std::string_view::npos ? 0 : lineStart + 1;
    const std::string_view prefix = text.substr(from, offset - from);
    return prefix.find_first_not_of(" \t") == std::string_view::npos ? std::string(prefix) : std::string();
}

struct Replacement {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string text;
};

} // namespace

Result<std::string> applySlnxConfigurationEdits(std::string_view content, const SolutionModel& model,
                                                std::span<const ConfigurationMappingEdit> edits) {
    auto scanned = scanTags(content);
    if (!scanned) {
        return std::unexpected(std::move(scanned.error()));
    }
    const std::vector<Tag>& tags = *scanned;
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    // The element tree: each start tag's parent and each open start tag's matching end tag.
    std::vector<std::size_t> parent(tags.size(), kNone);
    std::vector<std::size_t> matchingEnd(tags.size(), kNone);
    {
        std::vector<std::size_t> stack;
        for (std::size_t t = 0; t < tags.size(); ++t) {
            if (tags[t].closing) {
                if (stack.empty() || !iequals(tags[stack.back()].name, tags[t].name)) {
                    return fail(ErrorCode::Corrupt, std::format("the .slnx closes </{}> out of order", tags[t].name));
                }
                matchingEnd[stack.back()] = t;
                stack.pop_back();
                continue;
            }
            parent[t] = stack.empty() ? kNone : stack.back();
            if (!tags[t].selfClosing) {
                stack.push_back(t);
            }
        }
        if (!stack.empty()) {
            return fail(ErrorCode::Corrupt, std::format("the .slnx never closes <{}>", tags[stack.back()].name));
        }
    }
    const std::string eol = content.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    // Path key -> the <Project> start tag.
    std::map<std::string, std::size_t> projectTags;
    for (std::size_t t = 0; t < tags.size(); ++t) {
        const std::size_t up = parent[t];
        if (tags[t].closing || !iequals(tags[t].name, "Project") || up == kNone ||
            !(iequals(tags[up].name, "Solution") || iequals(tags[up].name, "Folder"))) {
            continue;
        }
        if (const Attribute* path = tags[t].attribute("Path")) {
            if (auto resolved = detail::resolvePath(model.directory(), path->value)) {
                projectTags.emplace(detail::pathKey(*resolved), t);
            }
        }
    }
    // (project tag, kind, configuration name) -> the value the edits want, last edit winning.
    std::map<std::tuple<std::size_t, ConfigurationRuleKind, std::string>, std::string> wanted;
    std::map<std::tuple<std::size_t, ConfigurationRuleKind, std::string>, SolutionConfiguration> configs;
    for (const ConfigurationMappingEdit& edit : edits) {
        if (auto checked = checkEdit(model, edit); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        const ProjectInfo& info = model.project(edit.project);
        if (!edit.mapping) {
            return fail(ErrorCode::Unsupported,
                        std::format("a .slnx cannot leave {} out of a solution configuration", info.name));
        }
        const auto found = projectTags.find(detail::pathKey(info.path));
        if (found == projectTags.end()) {
            return fail(ErrorCode::NotFound, std::format("the .slnx has no <Project> for {}", info.path));
        }
        const SolutionConfiguration& config = model.configurations()[edit.solutionConfiguration];
        const auto& current = model.projectConfiguration(edit.project, edit.solutionConfiguration);
        const ProjectConfigurationMapping& target = *edit.mapping;
        const std::array<std::tuple<ConfigurationRuleKind, std::string, bool>, 4> properties{{
            {ConfigurationRuleKind::BuildType, target.configuration,
             !current || !iequals(current->configuration, target.configuration)},
            {ConfigurationRuleKind::Platform, target.platform,
             !current || !iequals(current->platform, target.platform)},
            {ConfigurationRuleKind::Build, target.build ? "true" : "false", !current || current->build != target.build},
            {ConfigurationRuleKind::Deploy, target.deploy ? "true" : "false",
             !current || current->deploy != target.deploy},
        }};
        for (const auto& [kind, value, differs] : properties) {
            const auto key = std::tuple{found->second, kind, config.name()};
            if (differs || wanted.contains(key)) {
                wanted[key] = value;
                configs[key] = config;
            }
        }
    }
    std::vector<Replacement> replacements;
    std::map<std::size_t, std::vector<std::string>> appended; // Project tag -> new rule elements.
    for (const auto& [key, value] : wanted) {
        const auto& [projectTag, kind, name] = key;
        const SolutionConfiguration& config = configs.at(key);
        // The project's rules of this kind, in document order.
        std::vector<std::size_t> rules;
        for (std::size_t t = projectTag + 1; t < tags.size(); ++t) {
            if (parent[t] == projectTag && !tags[t].closing && iequals(tags[t].name, ruleName(kind))) {
                rules.push_back(t);
            }
            if (matchingEnd[projectTag] != kNone && t >= matchingEnd[projectTag]) {
                break;
            }
            if (tags[projectTag].selfClosing) {
                break;
            }
        }
        std::optional<std::size_t> exact;
        bool laterMatch = false;
        for (const std::size_t rule : rules) {
            const Attribute* solution = tags[rule].attribute("Solution");
            const std::string pattern = solution ? solution->value : std::string();
            if (solution && iequals(detail::trim(pattern), name)) {
                exact = rule;
                laterMatch = false;
            } else if (exact && patternMatches(pattern, config)) {
                laterMatch = true;
            }
        }
        const Attribute* projectValue = exact ? tags[*exact].attribute("Project") : nullptr;
        if (exact && !laterMatch && projectValue != nullptr) {
            replacements.push_back({projectValue->valueBegin, projectValue->valueEnd, encodeAttribute(value)});
            continue;
        }
        appended[projectTag].push_back(std::format("<{} Solution=\"{}\" Project=\"{}\" />", ruleName(kind),
                                                   encodeAttribute(name), encodeAttribute(value)));
    }
    for (const auto& [projectTag, rules] : appended) {
        const Tag& start = tags[projectTag];
        const std::string projectIndent = indentationAt(content, start.begin);
        if (start.selfClosing) {
            // <Project Path="x" /> becomes <Project Path="x">, the rules, </Project>.
            std::size_t cut = start.end - 2;
            while (cut > start.begin && (content[cut - 1] == ' ' || content[cut - 1] == '\t')) {
                --cut;
            }
            std::string text = ">";
            for (const std::string& rule : rules) {
                text += eol + projectIndent + "  " + rule;
            }
            // The end tag repeats the name as the start tag wrote it (a namespace prefix included).
            const std::size_t nameEnd = content.find_first_of(" \t\r\n/>", start.begin + 1);
            text += eol + projectIndent + "</" +
                    std::string(content.substr(start.begin + 1, nameEnd - start.begin - 1)) + ">";
            replacements.push_back({cut, start.end, std::move(text)});
            continue;
        }
        // After the last direct child (or the start tag), indented like the first child.
        std::size_t lastEnd = start.end;
        std::optional<std::size_t> firstChild;
        for (std::size_t t = projectTag + 1; t < matchingEnd[projectTag]; ++t) {
            if (parent[t] == projectTag && !tags[t].closing) {
                if (!firstChild) {
                    firstChild = t;
                }
                lastEnd = tags[t].selfClosing ? tags[t].end : tags[matchingEnd[t]].end;
            }
        }
        std::string childIndent = firstChild ? indentationAt(content, tags[*firstChild].begin) : std::string();
        if (!firstChild || childIndent.empty()) {
            childIndent = projectIndent + "  ";
        }
        std::string text;
        for (const std::string& rule : rules) {
            text += eol + childIndent + rule;
        }
        if (!firstChild) {
            // An empty <Project ...></Project>: its end tag moves to a line of its own.
            text += eol + projectIndent;
            const std::size_t endBegin = tags[matchingEnd[projectTag]].begin;
            replacements.push_back({start.end, endBegin, std::move(text)});
            continue;
        }
        replacements.push_back({lastEnd, lastEnd, std::move(text)});
    }
    std::ranges::sort(replacements, [](const Replacement& a, const Replacement& b) { return a.begin > b.begin; });
    std::string out(content);
    for (const Replacement& replacement : replacements) {
        out.replace(replacement.begin, replacement.end - replacement.begin, replacement.text);
    }
    return out;
}

Result<void> writeSolutionConfigurationEdits(io::IFileSystem& fs, const SolutionModel& model,
                                             std::span<const ConfigurationMappingEdit> edits) {
    auto original = fs.readFile(model.path());
    if (!original) {
        return std::unexpected(std::move(original.error()));
    }
    auto edited = model.format() == SolutionFormat::Sln ? applySlnConfigurationEdits(*original, model, edits)
                                                        : applySlnxConfigurationEdits(*original, model, edits);
    if (!edited) {
        return std::unexpected(std::move(edited.error()));
    }
    if (*edited == *original) {
        return {};
    }
    if (auto written = fs.writeFileAtomic(model.path(), *edited); !written) {
        return written;
    }
    // Read back: every edited mapping must be exactly the one asked for, else the file is restored.
    const auto restore = [&](std::string why) -> Result<void> {
        static_cast<void>(fs.writeFileAtomic(model.path(), *original));
        return fail(ErrorCode::Corrupt,
                    std::format("the solution was left unchanged: the edited mappings did not read back ({})", why));
    };
    auto reloaded = SolutionModel::load(fs, model.path());
    if (!reloaded) {
        return restore(reloaded.error().message());
    }
    for (const ConfigurationMappingEdit& edit : edits) {
        const ProjectInfo& info = model.project(edit.project);
        const auto project = reloaded->findProjectByPath(info.path);
        const auto configuration =
            reloaded->findConfiguration(model.configurations()[edit.solutionConfiguration].name());
        if (!project || !configuration) {
            return restore(std::format("{} or its configuration is gone", info.name));
        }
        const auto& mapping = reloaded->projectConfiguration(*project, *configuration);
        const bool same =
            mapping.has_value() == edit.mapping.has_value() &&
            (!mapping || (iequals(mapping->configuration, edit.mapping->configuration) &&
                          iequals(mapping->platform, edit.mapping->platform) && mapping->build == edit.mapping->build &&
                          mapping->deploy == edit.mapping->deploy));
        if (!same) {
            return restore(
                std::format("{} in {}", info.name, model.configurations()[edit.solutionConfiguration].name()));
        }
    }
    return {};
}

} // namespace frost::project
