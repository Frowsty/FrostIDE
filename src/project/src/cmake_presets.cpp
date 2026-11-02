// SPDX-License-Identifier: Apache-2.0
//
// The CMake presets reader (frost/project/cmake_presets.hpp): file loading with includes,
// inheritance, macro expansion and condition evaluation.

#include "frost/project/cmake_presets.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/cmake_profile.hpp"

#include "json.hpp"
#include "process_environment.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <regex>
#include <set>
#include <span>
#include <tuple>

namespace frost::project {

namespace {

using detail::JsonNode;

// One preset as written, before inheritance.
struct RawPreset {
    std::string name;
    std::string file;
    bool user = false;
    const JsonNode* node = nullptr; // Points into Loader::documents, which never moves (std::map).
};

// The inheritable fields of a configure or build preset after inheritance, before expansion.
struct Merged {
    std::map<std::string, const JsonNode*> fields; // Scalar fields by name (generator, binaryDir, ...).
    std::vector<std::pair<std::string, const JsonNode*>> cacheVariables; // In precedence order.
    std::vector<std::pair<std::string, const JsonNode*>> environment;
};

// Inheritable scalar fields; every other member is handled separately or not inherited.
constexpr std::string_view kConfigureFields[] = {"generator",    "binaryDir", "installDir", "toolchainFile",
                                                 "architecture", "toolset",   "condition",  "cmakeExecutable"};
constexpr std::string_view kBuildFields[] = {
    "configurePreset", "configuration",     "targets",   "jobs",
    "cleanFirst",      "nativeToolOptions", "condition", "inheritConfigureEnvironment"};

std::string slashes(std::string_view path) {
    std::string out(path);
    if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
        std::ranges::replace(out, '\\', '/');
    }
    return out;
}

class Loader {
public:
    Loader(io::IFileSystem& fs, std::string sourceDir, const CMakePresetsContext& context)
        : m_fs(fs), m_sourceDir(std::move(sourceDir)), m_context(context) {
        if (m_context.hostSystemName.empty()) {
            m_context.hostSystemName = std::string(cmakeHostSystemName());
        }
        if (!m_context.environment) {
            m_context.environment = [](std::string_view name) { return detail::processEnvironmentVariable(name); };
        }
    }

    Result<std::optional<CMakePresets>> load() {
        auto projectFile = io::joinPath(m_sourceDir, kCMakePresetsFile);
        auto userFile = io::joinPath(m_sourceDir, kCMakeUserPresetsFile);
        if (!projectFile || !userFile) {
            return fail(ErrorCode::InvalidArgument, std::format("'{}' is not a usable source directory", m_sourceDir));
        }
        auto projectExists = m_fs.exists(*projectFile);
        auto userExists = m_fs.exists(*userFile);
        if (!projectExists || !userExists) {
            return std::unexpected(projectExists ? userExists.error() : projectExists.error());
        }
        if (!*projectExists && !*userExists) {
            return std::optional<CMakePresets>{};
        }
        std::vector<std::string> stack;
        if (*projectExists) {
            if (auto read = readFile(*projectFile, false, stack); !read) {
                return std::unexpected(std::move(read.error()));
            }
        }
        if (*userExists) {
            if (auto read = readFile(*userFile, true, stack); !read) {
                return std::unexpected(std::move(read.error()));
            }
        }
        CMakePresets presets;
        presets.version = m_version;
        presets.files = m_fileOrder;
        for (const RawPreset& raw : m_configure) {
            auto resolved = resolveConfigure(raw);
            if (!resolved) {
                return std::unexpected(std::move(resolved.error()));
            }
            presets.configurePresets.push_back(std::move(*resolved));
        }
        for (const RawPreset& raw : m_build) {
            auto resolved = resolveBuild(raw, presets);
            if (!resolved) {
                return std::unexpected(std::move(resolved.error()));
            }
            presets.buildPresets.push_back(std::move(*resolved));
        }
        return std::optional<CMakePresets>(std::move(presets));
    }

private:
    // The macro values of one preset (header comment, "Macros").
    struct Macros {
        std::string presetName;
        std::string generator;
        std::string fileDir;
        // The preset's environment before expansion; nullptr values unset the variable.
        const std::vector<std::pair<std::string, const JsonNode*>>* environment = nullptr;
    };

    Error error(const std::string& file, std::string_view what) const {
        return Error(ErrorCode::Corrupt, std::format("{}: {}", file, what));
    }

    std::optional<std::string> parentEnv(std::string_view name) const { return m_context.environment(name); }

    Result<void> readFile(const std::string& path, bool user, std::vector<std::string>& stack) {
        if (std::ranges::find(stack, path) != stack.end()) {
            return fail(ErrorCode::Corrupt, std::format("{}: the presets include each other in a cycle", path));
        }
        if (m_documents.contains(path)) {
            return {}; // Read already (a diamond include).
        }
        auto content = m_fs.readFile(path);
        if (!content) {
            return std::unexpected(std::move(content.error()).withContext(std::format("while reading {}", path)));
        }
        auto parsed = detail::parseJson(*content);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()).withContext(std::format("in {}", path)));
        }
        if (!parsed->isObject()) {
            return std::unexpected(error(path, "the top level is not an object"));
        }
        const auto version = parsed->integerAt("version");
        if (!version) {
            return std::unexpected(error(path, "\"version\" is missing or not an integer"));
        }
        if (*version < 2) {
            return fail(ErrorCode::Unsupported,
                        std::format("{}: presets version {} is not supported (2 and later are)", path, *version));
        }
        if (m_version == 0 || (!user && m_fileOrder.empty())) {
            m_version = static_cast<int>(*version);
        }
        const JsonNode& root = m_documents.emplace(path, std::move(*parsed)).first->second;
        m_fileOrder.push_back(path);
        stack.push_back(path);
        const std::string fileDir(io::parentPath(path));

        // The user file includes the project file first (cmake-presets(7)).
        std::vector<std::string> includes;
        if (user && io::fileName(path) == kCMakeUserPresetsFile) {
            auto projectFile = io::joinPath(m_sourceDir, kCMakePresetsFile);
            if (projectFile && m_fs.exists(*projectFile).value_or(false)) {
                includes.push_back(*projectFile);
            }
        }
        if (const JsonNode* list = root.find("include")) {
            if (!list->isArray()) {
                return std::unexpected(error(path, "\"include\" is not an array"));
            }
            for (const JsonNode& item : list->items) {
                if (!item.isString()) {
                    return std::unexpected(error(path, "an \"include\" entry is not a string"));
                }
                Macros macros{.presetName = {}, .generator = {}, .fileDir = slashes(fileDir), .environment = nullptr};
                auto expanded = expand(item.text, macros, path);
                if (!expanded) {
                    return std::unexpected(std::move(expanded.error()));
                }
                auto absolute = absolutePath(*expanded, fileDir);
                if (!absolute) {
                    return std::unexpected(error(path, std::format("include '{}' is not a valid path", item.text)));
                }
                includes.push_back(std::move(*absolute));
            }
        }
        for (const std::string& include : includes) {
            if (auto read = readFile(include, user, stack); !read) {
                return read;
            }
        }
        stack.pop_back();

        for (const auto& [kind, list] :
             {std::pair<std::string_view, std::vector<RawPreset>*>{"configurePresets", &m_configure},
              std::pair<std::string_view, std::vector<RawPreset>*>{"buildPresets", &m_build}}) {
            const JsonNode* presets = root.find(kind);
            if (presets == nullptr) {
                continue;
            }
            if (!presets->isArray()) {
                return std::unexpected(error(path, std::format("\"{}\" is not an array", kind)));
            }
            for (const JsonNode& node : presets->items) {
                if (!node.isObject() || node.stringAt("name").empty()) {
                    return std::unexpected(error(path, std::format("a preset in \"{}\" has no name", kind)));
                }
                std::string name = node.stringAt("name");
                if (std::ranges::any_of(*list, [&](const RawPreset& other) { return other.name == name; })) {
                    return std::unexpected(error(path, std::format("preset '{}' is defined twice", name)));
                }
                list->push_back(RawPreset{.name = std::move(name), .file = path, .user = user, .node = &node});
            }
        }
        return {};
    }

    // `path` (expanded) made absolute against `base` and normalised; nullopt when invalid.
    static std::optional<std::string> absolutePath(std::string_view path, std::string_view base) {
        std::string joined;
        if (io::isAbsolutePath(path)) {
            joined = std::string(path);
        } else {
            auto relative = io::joinPath(base, path);
            if (!relative) {
                return std::nullopt;
            }
            joined = std::move(*relative);
        }
        std::string collapsed = collapseCMakePath(joined); // CMake resolves ".." lexically.
        return collapsed.empty() ? std::nullopt : std::optional<std::string>(std::move(collapsed));
    }

    std::vector<std::string> inheritsOf(const RawPreset& raw) const {
        std::vector<std::string> parents;
        if (const JsonNode* inherits = raw.node->find("inherits")) {
            if (inherits->isString()) {
                parents.push_back(inherits->text);
            } else if (inherits->isArray()) {
                for (const JsonNode& item : inherits->items) {
                    if (item.isString()) {
                        parents.push_back(item.text);
                    }
                }
            }
        }
        return parents;
    }

    // Inheritance (header comment) over `raws`; memoised per kind.
    Result<Merged> merge(const RawPreset& raw, const std::vector<RawPreset>& raws,
                         std::span<const std::string_view> fields, std::map<std::string, Merged>& memo,
                         std::vector<std::string>& stack) {
        if (auto found = memo.find(raw.name); found != memo.end()) {
            return found->second;
        }
        if (std::ranges::find(stack, raw.name) != stack.end()) {
            return std::unexpected(error(raw.file, std::format("preset '{}' inherits from itself", raw.name)));
        }
        stack.push_back(raw.name);
        Merged merged;
        for (std::string_view field : fields) {
            if (const JsonNode* value = raw.node->find(field)) {
                merged.fields.emplace(std::string(field), value);
            }
        }
        const auto addMap = [](std::vector<std::pair<std::string, const JsonNode*>>& into, const JsonNode* map) {
            if (map == nullptr || !map->isObject()) {
                return;
            }
            for (std::size_t i = 0; i < map->keys.size(); ++i) {
                if (std::ranges::none_of(into, [&](const auto& entry) { return entry.first == map->keys[i]; })) {
                    into.emplace_back(map->keys[i], &map->items[i]);
                }
            }
        };
        addMap(merged.cacheVariables, raw.node->find("cacheVariables"));
        addMap(merged.environment, raw.node->find("environment"));
        for (const std::string& parentName : inheritsOf(raw)) {
            const auto parent = std::ranges::find(raws, parentName, &RawPreset::name);
            if (parent == raws.end()) {
                return std::unexpected(error(
                    raw.file, std::format("preset '{}' inherits from unknown preset '{}'", raw.name, parentName)));
            }
            auto parentMerged = merge(*parent, raws, fields, memo, stack);
            if (!parentMerged) {
                return parentMerged;
            }
            for (const auto& [field, value] : parentMerged->fields) {
                merged.fields.emplace(field, value); // Keeps the preset's own (or an earlier parent's).
            }
            for (const auto& entry : parentMerged->cacheVariables) {
                if (std::ranges::none_of(merged.cacheVariables,
                                         [&](const auto& e) { return e.first == entry.first; })) {
                    merged.cacheVariables.push_back(entry);
                }
            }
            for (const auto& entry : parentMerged->environment) {
                if (std::ranges::none_of(merged.environment, [&](const auto& e) { return e.first == entry.first; })) {
                    merged.environment.push_back(entry);
                }
            }
        }
        stack.pop_back();
        memo.emplace(raw.name, merged);
        return merged;
    }

    // $env{name} for `macros`: the preset's entry (expanded, cycles refused) or the parent's.
    Result<std::optional<std::string>> envValue(std::string_view name, const Macros& macros, const std::string& file,
                                                std::vector<std::string>& visiting) {
        if (macros.environment != nullptr) {
            for (const auto& [key, value] : *macros.environment) {
                if (!sameEnvName(key, name)) {
                    continue;
                }
                if (value == nullptr || value->isNull()) {
                    return std::optional<std::string>{};
                }
                if (!value->isString()) {
                    return std::unexpected(error(file, std::format("environment variable '{}' is not a string", key)));
                }
                if (std::ranges::any_of(visiting, [&](const std::string& v) { return sameEnvName(v, key); })) {
                    return std::unexpected(
                        error(file, std::format("environment variable '{}' refers to itself through $env{{}}", key)));
                }
                visiting.push_back(key);
                auto expanded = expand(value->text, macros, file, &visiting);
                visiting.pop_back();
                if (!expanded) {
                    return std::unexpected(std::move(expanded.error()));
                }
                return std::optional<std::string>(std::move(*expanded));
            }
        }
        return parentEnv(name);
    }

    static bool sameEnvName(std::string_view a, std::string_view b) noexcept {
        if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
            return std::ranges::equal(a, b, [](char x, char y) {
                const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
                return lower(x) == lower(y);
            });
        } else {
            return a == b;
        }
    }

    // Expands the macros in `text` (header comment). `visiting` tracks $env{} recursion.
    Result<std::string> expand(std::string_view text, const Macros& macros, const std::string& file,
                               std::vector<std::string>* visiting = nullptr) {
        std::vector<std::string> localVisiting;
        std::vector<std::string>& stack = visiting != nullptr ? *visiting : localVisiting;
        std::string out;
        std::size_t i = 0;
        while (i < text.size()) {
            const char c = text[i];
            if (c != '$') {
                out += c;
                ++i;
                continue;
            }
            const std::string_view rest = text.substr(i);
            const auto closing = [&](std::size_t open) -> std::optional<std::string_view> {
                const std::size_t end = rest.find('}', open);
                if (end == std::string_view::npos) {
                    return std::nullopt;
                }
                return rest.substr(open, end - open);
            };
            if (rest.starts_with("${")) {
                const auto name = closing(2);
                if (!name) {
                    return std::unexpected(error(file, std::format("unterminated macro in '{}'", text)));
                }
                auto value = macro(*name, macros);
                if (!value) {
                    return std::unexpected(error(file, std::format("unknown macro ${{{}}} in '{}'", *name, text)));
                }
                out += *value;
                i += name->size() + 3;
            } else if (rest.starts_with("$env{")) {
                const auto name = closing(5);
                if (!name) {
                    return std::unexpected(error(file, std::format("unterminated macro in '{}'", text)));
                }
                auto value = envValue(*name, macros, file, stack);
                if (!value) {
                    return std::unexpected(std::move(value.error()));
                }
                out += value->value_or(std::string());
                i += name->size() + 6;
            } else if (rest.starts_with("$penv{")) {
                const auto name = closing(6);
                if (!name) {
                    return std::unexpected(error(file, std::format("unterminated macro in '{}'", text)));
                }
                out += parentEnv(*name).value_or(std::string());
                i += name->size() + 7;
            } else if (rest.starts_with("$vendor{")) {
                const auto name = closing(8);
                const std::size_t length = name ? name->size() + 9 : rest.size();
                out += rest.substr(0, length); // Left as written.
                i += length;
            } else {
                out += c;
                ++i;
            }
        }
        return out;
    }

    std::optional<std::string> macro(std::string_view name, const Macros& macros) const {
        const std::string_view parent = io::parentPath(m_sourceDir);
        if (name == "sourceDir") {
            return slashes(m_sourceDir);
        }
        if (name == "sourceParentDir") {
            return slashes(parent.empty() ? std::string_view(m_sourceDir) : parent);
        }
        if (name == "sourceDirName") {
            return std::string(io::fileName(m_sourceDir));
        }
        if (name == "presetName") {
            return macros.presetName;
        }
        if (name == "generator") {
            return macros.generator;
        }
        if (name == "hostSystemName") {
            return m_context.hostSystemName;
        }
        if (name == "fileDir") {
            return macros.fileDir;
        }
        if (name == "dollar") {
            return std::string("$");
        }
        if (name == "pathListSep") {
            return std::string(io::kNativePathStyle == io::PathStyle::Windows ? ";" : ":");
        }
        return std::nullopt;
    }

    // A string field, expanded; "" when absent. Errors: not a string, expansion errors.
    Result<std::string> stringField(const Merged& merged, std::string_view field, const Macros& macros,
                                    const std::string& file) {
        const auto found = merged.fields.find(std::string(field));
        if (found == merged.fields.end() || found->second->isNull()) {
            return std::string();
        }
        if (!found->second->isString()) {
            return std::unexpected(error(file, std::format("\"{}\" is not a string", field)));
        }
        return expand(found->second->text, macros, file);
    }

    Result<bool> evaluate(const JsonNode& condition, const Macros& macros, const std::string& file) {
        if (condition.isNull()) {
            return true;
        }
        if (condition.isBool()) {
            return condition.boolean;
        }
        if (!condition.isObject()) {
            return std::unexpected(error(file, "a condition is not an object"));
        }
        const std::string type = condition.stringAt("type");
        const auto operand = [&](std::string_view key) -> Result<std::string> {
            return expand(condition.stringAt(key), macros, file);
        };
        if (type == "const") {
            return condition.boolAt("value");
        }
        if (type == "equals" || type == "notEquals") {
            auto lhs = operand("lhs");
            auto rhs = operand("rhs");
            if (!lhs || !rhs) {
                return std::unexpected(lhs ? rhs.error() : lhs.error());
            }
            return (*lhs == *rhs) == (type == "equals");
        }
        if (type == "inList" || type == "notInList") {
            auto value = operand("string");
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            bool found = false;
            if (const JsonNode* list = condition.find("list"); list != nullptr && list->isArray()) {
                for (const JsonNode& item : list->items) {
                    auto expanded = expand(item.text, macros, file);
                    if (!expanded) {
                        return std::unexpected(std::move(expanded.error()));
                    }
                    found = found || *expanded == *value;
                }
            }
            return found == (type == "inList");
        }
        if (type == "matches" || type == "notMatches") {
            auto value = operand("string");
            auto pattern = operand("regex");
            if (!value || !pattern) {
                return std::unexpected(value ? pattern.error() : value.error());
            }
            try {
                const std::regex regex(*pattern, std::regex::ECMAScript);
                return std::regex_search(*value, regex) == (type == "matches");
            } catch (const std::regex_error&) {
                return std::unexpected(error(file, std::format("invalid regex '{}' in a condition", *pattern)));
            }
        }
        if (type == "anyOf" || type == "allOf") {
            const JsonNode* list = condition.find("conditions");
            if (list == nullptr || !list->isArray()) {
                return std::unexpected(error(file, std::format("a {} condition has no \"conditions\" array", type)));
            }
            const bool any = type == "anyOf";
            for (const JsonNode& item : list->items) {
                auto value = evaluate(item, macros, file);
                if (!value) {
                    return value;
                }
                if (*value == any) {
                    return any;
                }
            }
            return !any;
        }
        if (type == "not") {
            const JsonNode* inner = condition.find("condition");
            if (inner == nullptr) {
                return std::unexpected(error(file, "a not condition has no \"condition\""));
            }
            auto value = evaluate(*inner, macros, file);
            if (!value) {
                return value;
            }
            return !*value;
        }
        return std::unexpected(error(file, std::format("unknown condition type '{}'", type)));
    }

    Result<CMakeEnvironment> environmentOf(const std::vector<std::pair<std::string, const JsonNode*>>& entries,
                                           const Macros& macros, const std::string& file) {
        CMakeEnvironment out;
        for (const auto& [name, value] : entries) {
            if (value == nullptr || value->isNull()) {
                out.emplace_back(name, std::nullopt);
                continue;
            }
            std::vector<std::string> visiting{name};
            if (!value->isString()) {
                return std::unexpected(error(file, std::format("environment variable '{}' is not a string", name)));
            }
            auto expanded = expand(value->text, macros, file, &visiting);
            if (!expanded) {
                return std::unexpected(std::move(expanded.error()));
            }
            out.emplace_back(name, std::move(*expanded));
        }
        return out;
    }

    Result<CMakeConfigurePreset> resolveConfigure(const RawPreset& raw) {
        std::vector<std::string> stack;
        auto merged = merge(raw, m_configure, kConfigureFields, m_configureMemo, stack);
        if (!merged) {
            return std::unexpected(std::move(merged.error()));
        }
        CMakeConfigurePreset preset;
        preset.name = raw.name;
        preset.file = raw.file;
        preset.user = raw.user;
        preset.displayName = raw.node->stringAt("displayName");
        preset.description = raw.node->stringAt("description");
        preset.hidden = raw.node->boolAt("hidden");

        Macros macros{.presetName = raw.name,
                      .generator = {},
                      .fileDir = slashes(io::parentPath(raw.file)),
                      .environment = &merged->environment};
        auto generator = stringField(*merged, "generator", macros, raw.file);
        if (!generator) {
            return std::unexpected(std::move(generator.error()));
        }
        preset.generator = *generator;
        macros.generator = *generator;

        for (const auto& [field, target] :
             {std::pair<std::string_view, std::string*>{"binaryDir", &preset.binaryDir},
              std::pair<std::string_view, std::string*>{"installDir", &preset.installDir},
              std::pair<std::string_view, std::string*>{"toolchainFile", &preset.toolchainFile}}) {
            auto value = stringField(*merged, field, macros, raw.file);
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            if (!value->empty()) {
                auto absolute = absolutePath(*value, m_sourceDir);
                if (!absolute) {
                    return std::unexpected(error(raw.file, std::format("preset '{}': \"{}\" '{}' is not a valid path",
                                                                       raw.name, field, *value)));
                }
                *target = std::move(*absolute);
            }
        }

        for (const auto& [field, value, external] :
             {std::tuple<std::string_view, std::string*, bool*>{"architecture", &preset.architecture,
                                                                &preset.architectureExternal},
              std::tuple<std::string_view, std::string*, bool*>{"toolset", &preset.toolset, &preset.toolsetExternal}}) {
            const auto found = merged->fields.find(std::string(field));
            if (found == merged->fields.end()) {
                continue;
            }
            const JsonNode& node = *found->second;
            std::string text = node.isString() ? node.text : node.stringAt("value");
            auto expanded = expand(text, macros, raw.file);
            if (!expanded) {
                return std::unexpected(std::move(expanded.error()));
            }
            *value = std::move(*expanded);
            *external = node.isObject() && node.stringAt("strategy") == "external";
        }

        for (const auto& [name, node] : merged->cacheVariables) {
            if (node == nullptr || node->isNull()) {
                continue; // Unset (it removes an inherited value).
            }
            CMakeCacheVariable variable{.name = name, .type = {}, .value = {}};
            const JsonNode* value = node;
            if (node->isObject()) {
                variable.type = node->stringAt("type");
                value = node->find("value");
            }
            if (value == nullptr) {
                continue;
            }
            if (value->isBool()) {
                variable.value = value->boolean ? "TRUE" : "FALSE";
                if (variable.type.empty() && !node->isObject()) {
                    variable.type = "BOOL";
                }
            } else if (value->isString()) {
                auto expanded = expand(value->text, macros, raw.file);
                if (!expanded) {
                    return std::unexpected(std::move(expanded.error()));
                }
                variable.value = std::move(*expanded);
            } else {
                return std::unexpected(error(
                    raw.file, std::format("preset '{}': cache variable '{}' is not a string or bool", raw.name, name)));
            }
            preset.cacheVariables.push_back(std::move(variable));
        }

        auto environment = environmentOf(merged->environment, macros, raw.file);
        if (!environment) {
            return std::unexpected(std::move(environment.error()));
        }
        preset.environment = std::move(*environment);

        if (const auto condition = merged->fields.find("condition"); condition != merged->fields.end()) {
            auto enabled = evaluate(*condition->second, macros, raw.file);
            if (!enabled) {
                return std::unexpected(std::move(enabled.error()));
            }
            preset.enabled = *enabled;
        }
        return preset;
    }

    Result<CMakeBuildPreset> resolveBuild(const RawPreset& raw, const CMakePresets& presets) {
        std::vector<std::string> stack;
        auto merged = merge(raw, m_build, kBuildFields, m_buildMemo, stack);
        if (!merged) {
            return std::unexpected(std::move(merged.error()));
        }
        CMakeBuildPreset preset;
        preset.name = raw.name;
        preset.file = raw.file;
        preset.user = raw.user;
        preset.displayName = raw.node->stringAt("displayName");
        preset.description = raw.node->stringAt("description");
        preset.hidden = raw.node->boolAt("hidden");

        const auto field = [&](std::string_view name) -> const JsonNode* {
            const auto found = merged->fields.find(std::string(name));
            return found == merged->fields.end() ? nullptr : found->second;
        };
        if (const JsonNode* configure = field("configurePreset"); configure != nullptr && configure->isString()) {
            preset.configurePreset = configure->text;
        }
        const CMakeConfigurePreset* configure = presets.findConfigurePreset(preset.configurePreset);
        if (!preset.hidden && configure == nullptr) {
            return std::unexpected(
                error(raw.file, std::format("build preset '{}' names no known configure preset ('{}')", raw.name,
                                            preset.configurePreset)));
        }

        // The environment: the build preset's own entries, then (by default) the configure preset's.
        std::vector<std::pair<std::string, const JsonNode*>> environment = merged->environment;
        const JsonNode* inherit = field("inheritConfigureEnvironment");
        if (configure != nullptr && (inherit == nullptr || !inherit->isBool() || inherit->boolean)) {
            std::vector<std::string> configureStack;
            const auto raws = std::ranges::find(m_configure, configure->name, &RawPreset::name);
            auto configureMerged = merge(*raws, m_configure, kConfigureFields, m_configureMemo, configureStack);
            if (!configureMerged) {
                return std::unexpected(std::move(configureMerged.error()));
            }
            for (const auto& entry : configureMerged->environment) {
                if (std::ranges::none_of(environment, [&](const auto& e) { return e.first == entry.first; })) {
                    environment.push_back(entry);
                }
            }
        }
        Macros macros{.presetName = raw.name,
                      .generator = configure != nullptr ? configure->generator : std::string(),
                      .fileDir = slashes(io::parentPath(raw.file)),
                      .environment = &environment};

        auto configuration = stringField(*merged, "configuration", macros, raw.file);
        if (!configuration) {
            return std::unexpected(std::move(configuration.error()));
        }
        preset.configuration = std::move(*configuration);
        for (const auto& [name, into] :
             {std::pair<std::string_view, std::vector<std::string>*>{"targets", &preset.targets},
              std::pair<std::string_view, std::vector<std::string>*>{"nativeToolOptions", &preset.nativeToolOptions}}) {
            const JsonNode* node = field(name);
            if (node == nullptr) {
                continue;
            }
            std::vector<const JsonNode*> items;
            if (node->isString()) {
                items.push_back(node);
            } else if (node->isArray()) {
                for (const JsonNode& item : node->items) {
                    items.push_back(&item);
                }
            }
            for (const JsonNode* item : items) {
                auto expanded = expand(item->text, macros, raw.file);
                if (!expanded) {
                    return std::unexpected(std::move(expanded.error()));
                }
                into->push_back(std::move(*expanded));
            }
        }
        if (const JsonNode* jobs = field("jobs"); jobs != nullptr && jobs->integer) {
            preset.jobs = static_cast<int>(*jobs->integer);
        }
        if (const JsonNode* cleanFirst = field("cleanFirst"); cleanFirst != nullptr && cleanFirst->isBool()) {
            preset.cleanFirst = cleanFirst->boolean;
        }
        auto expandedEnvironment = environmentOf(environment, macros, raw.file);
        if (!expandedEnvironment) {
            return std::unexpected(std::move(expandedEnvironment.error()));
        }
        preset.environment = std::move(*expandedEnvironment);
        if (const JsonNode* condition = field("condition")) {
            auto enabled = evaluate(*condition, macros, raw.file);
            if (!enabled) {
                return std::unexpected(std::move(enabled.error()));
            }
            preset.enabled = *enabled;
        }
        return preset;
    }

    io::IFileSystem& m_fs;
    std::string m_sourceDir;
    CMakePresetsContext m_context;
    int m_version = 0;
    std::map<std::string, JsonNode> m_documents; // By absolute path; nodes never move once inserted.
    std::vector<std::string> m_fileOrder;
    std::vector<RawPreset> m_configure;
    std::vector<RawPreset> m_build;
    std::map<std::string, Merged> m_configureMemo;
    std::map<std::string, Merged> m_buildMemo;
};

} // namespace

std::string_view cmakeHostSystemName() noexcept {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "Darwin";
#else
    return "Linux";
#endif
}

const CMakeConfigurePreset* CMakePresets::findConfigurePreset(std::string_view name) const noexcept {
    for (const CMakeConfigurePreset& preset : configurePresets) {
        if (preset.name == name) {
            return &preset;
        }
    }
    return nullptr;
}

std::vector<const CMakeBuildPreset*> CMakePresets::buildPresetsFor(std::string_view configurePreset) const {
    std::vector<const CMakeBuildPreset*> out;
    for (const CMakeBuildPreset& preset : buildPresets) {
        if (!preset.hidden && preset.enabled && preset.configurePreset == configurePreset) {
            out.push_back(&preset);
        }
    }
    return out;
}

Result<std::optional<CMakePresets>> loadCMakePresets(io::IFileSystem& fs, std::string_view sourceDir,
                                                     const CMakePresetsContext& context) {
    auto normalized = io::normalizePath(sourceDir);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    Loader loader(fs, std::move(*normalized), context);
    return loader.load();
}

} // namespace frost::project
