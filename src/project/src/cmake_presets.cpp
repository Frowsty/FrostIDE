// SPDX-License-Identifier: Apache-2.0
//
// The CMake presets reader (frost/project/cmake_presets.hpp): file loading with includes in CMake's
// order, inheritance, macro expansion, condition evaluation, the five preset kinds and the
// `cmake --list-presets` listing.

#include "frost/project/cmake_presets.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/cmake_profile.hpp"

#include "json.hpp"
#include "process_environment.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <regex>
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
    std::int64_t version = 0;       // The version of the file that defines it.
    const JsonNode* node = nullptr; // Points into Loader::documents, which never moves (std::map).
};

// The inheritable fields of a preset after inheritance, before expansion.
struct Merged {
    // Scalar fields by name; members of the objects CMake merges member by member are dotted
    // paths ("warnings.dev", "filter.include.name").
    std::map<std::string, const JsonNode*> fields;
    std::vector<std::pair<std::string, const JsonNode*>> cacheVariables; // In precedence order.
    std::vector<std::pair<std::string, const JsonNode*>> environment;
};

// Preset kinds by index (CMakePresetKind order). Workflow presets have no inheritance.
constexpr std::size_t kConfigure = 0;
constexpr std::size_t kBuild = 1;
constexpr std::size_t kTest = 2;
constexpr std::size_t kPackage = 3;
constexpr std::size_t kWorkflow = 4;
constexpr std::size_t kKinds = 5;
constexpr std::array<std::string_view, kKinds> kKindKeys = {"configurePresets", "buildPresets", "testPresets",
                                                            "packagePresets", "workflowPresets"};
constexpr std::array<std::int64_t, kKinds> kKindVersions = {2, 2, 2, 6, 6}; // The file version each kind needs.
constexpr std::array<std::string_view, kKinds> kKindNames = {"configure preset", "build preset", "test preset",
                                                             "package preset", "workflow preset"};

// Inheritable scalar fields per kind; every other member is handled separately or not inherited.
constexpr std::string_view kConfigureFields[] = {"generator",
                                                 "binaryDir",
                                                 "installDir",
                                                 "toolchainFile",
                                                 "architecture",
                                                 "toolset",
                                                 "condition",
                                                 "cmakeExecutable",
                                                 "warnings.dev",
                                                 "warnings.deprecated",
                                                 "warnings.uninitialized",
                                                 "warnings.unusedCli",
                                                 "warnings.systemVars",
                                                 "errors.dev",
                                                 "errors.deprecated",
                                                 "debug.output",
                                                 "debug.tryCompile",
                                                 "debug.find"};
constexpr std::string_view kBuildFields[] = {"configurePreset",
                                             "configuration",
                                             "targets",
                                             "jobs",
                                             "cleanFirst",
                                             "verbose",
                                             "nativeToolOptions",
                                             "condition",
                                             "inheritConfigureEnvironment"};
constexpr std::string_view kTestFields[] = {
    "configurePreset",       "configuration",           "condition",           "inheritConfigureEnvironment",
    "filter.include.name",   "filter.include.label",    "filter.exclude.name", "filter.exclude.label",
    "execution.jobs",        "execution.stopOnFailure", "execution.timeout",   "execution.noTestsAction",
    "output.outputOnFailure"};
constexpr std::string_view kPackageFields[] = {
    "configurePreset", "generators",       "configurations", "packageName",
    "packageVersion",  "packageDirectory", "condition",      "inheritConfigureEnvironment"};

std::span<const std::string_view> fieldsOf(std::size_t kind) {
    switch (kind) {
    case kConfigure: return kConfigureFields;
    case kBuild: return kBuildFields;
    case kTest: return kTestFields;
    default: return kPackageFields;
    }
}

// The member at dotted `path` of `node` ("filter.include.name"), or nullptr.
const JsonNode* findPath(const JsonNode& node, std::string_view path) {
    const JsonNode* current = &node;
    while (current != nullptr) {
        const std::size_t dot = path.find('.');
        current = current->find(path.substr(0, dot));
        if (dot == std::string_view::npos) {
            return current;
        }
        path.remove_prefix(dot + 1);
    }
    return nullptr;
}

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
        m_projectFile = *projectFile;
        m_projectExists = *projectExists;
        // The user file leads and includes the project file (header comment, "Order").
        std::vector<std::string> stack;
        if (auto read = *userExists ? readFile(*userFile, true, stack) : readFile(*projectFile, false, stack); !read) {
            return std::unexpected(std::move(read.error()));
        }
        CMakePresets presets;
        presets.version = static_cast<int>(m_projectVersion != 0 ? m_projectVersion : m_userVersion);
        presets.files = m_fileOrder;
        for (const RawPreset& raw : m_raw[kConfigure]) {
            auto resolved = resolveConfigure(raw);
            if (!resolved) {
                return std::unexpected(std::move(resolved.error()));
            }
            presets.configurePresets.push_back(std::move(*resolved));
        }
        for (const RawPreset& raw : m_raw[kBuild]) {
            auto resolved = resolveBuild(raw, presets);
            if (!resolved) {
                return std::unexpected(std::move(resolved.error()));
            }
            presets.buildPresets.push_back(std::move(*resolved));
        }
        for (const RawPreset& raw : m_raw[kTest]) {
            auto resolved = resolveTest(raw, presets);
            if (!resolved) {
                return std::unexpected(std::move(resolved.error()));
            }
            presets.testPresets.push_back(std::move(*resolved));
        }
        for (const RawPreset& raw : m_raw[kPackage]) {
            auto resolved = resolvePackage(raw, presets);
            if (!resolved) {
                return std::unexpected(std::move(resolved.error()));
            }
            presets.packagePresets.push_back(std::move(*resolved));
        }
        for (const RawPreset& raw : m_raw[kWorkflow]) {
            auto resolved = resolveWorkflow(raw, presets);
            if (!resolved) {
                return std::unexpected(std::move(resolved.error()));
            }
            presets.workflowPresets.push_back(std::move(*resolved));
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

    /*
     * Reads `path` and what it includes. Its own presets are registered before its includes are
     * read, and the user file's implicit include of the project file comes after its own includes,
     * which is CMake's order (header comment).
     */
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
        const bool isProjectFile = path == m_projectFile;
        const bool isUserFile = user && io::fileName(path) == kCMakeUserPresetsFile && stack.empty();
        if (isProjectFile) {
            m_projectVersion = *version;
        } else if (isUserFile) {
            m_userVersion = *version;
        }
        const JsonNode& root = m_documents.emplace(path, std::move(*parsed)).first->second;
        m_fileOrder.push_back(path);
        const std::string fileDir(io::parentPath(path));

        for (std::size_t kind = 0; kind < kKinds; ++kind) {
            const JsonNode* list = root.find(kKindKeys[kind]);
            if (list == nullptr) {
                continue;
            }
            if (!list->isArray()) {
                return std::unexpected(error(path, std::format("\"{}\" is not an array", kKindKeys[kind])));
            }
            if (*version < kKindVersions[kind] && !list->items.empty()) {
                return std::unexpected(error(path, std::format("file version {} has no {}s (version {} and later)",
                                                               *version, kKindNames[kind], kKindVersions[kind])));
            }
            for (const JsonNode& node : list->items) {
                if (!node.isObject() || node.stringAt("name").empty()) {
                    return std::unexpected(error(path, std::format("a preset in \"{}\" has no name", kKindKeys[kind])));
                }
                if (*version < 3 && node.find("condition") != nullptr) {
                    return std::unexpected(error(path, "conditions need presets version 3 or later"));
                }
                std::string name = node.stringAt("name");
                std::vector<RawPreset>& raws = m_raw[kind];
                if (std::ranges::any_of(raws, [&](const RawPreset& other) { return other.name == name; })) {
                    return std::unexpected(
                        error(path, std::format("{} '{}' is defined twice", kKindNames[kind], name)));
                }
                raws.push_back(
                    RawPreset{.name = std::move(name), .file = path, .user = user, .version = *version, .node = &node});
            }
        }

        std::vector<std::pair<std::string, bool>> includes; // Path, and whether it is user-only.
        if (const JsonNode* list = root.find("include")) {
            if (!list->isArray()) {
                return std::unexpected(error(path, "\"include\" is not an array"));
            }
            if (*version < 4 && !list->items.empty()) {
                return std::unexpected(error(path, "\"include\" needs presets version 4 or later"));
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
                includes.emplace_back(std::move(*absolute), user);
            }
        }
        if (isUserFile && m_projectExists) {
            includes.emplace_back(m_projectFile, false); // The user file includes the project file, last.
        }
        std::vector<std::string>& edges = m_includes[path];
        for (const auto& include : includes) {
            edges.push_back(include.first);
        }
        stack.push_back(path);
        for (const auto& [include, includeUser] : includes) {
            if (auto read = readFile(include, includeUser, stack); !read) {
                return read;
            }
        }
        stack.pop_back();
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

    /*
     * Whether a preset of file `from` may refer to one of file `to`: CMake allows only presets of
     * the same file or of a file it includes, directly or not ("... is unreachable from preset's
     * file"; the user file reaches the project file through its implicit include).
     * O(files + includes).
     */
    bool reachable(const std::string& from, const std::string& to) const {
        std::vector<std::string_view> pending = {from};
        std::vector<std::string_view> seen;
        while (!pending.empty()) {
            const std::string_view file = pending.back();
            pending.pop_back();
            if (file == to) {
                return true;
            }
            if (std::ranges::find(seen, file) != seen.end()) {
                continue;
            }
            seen.push_back(file);
            if (const auto edges = m_includes.find(std::string(file)); edges != m_includes.end()) {
                pending.insert(pending.end(), edges->second.begin(), edges->second.end());
            }
        }
        return false;
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

    // Inheritance (header comment) of a preset of kind `kind`; memoised per kind.
    Result<Merged> merge(const RawPreset& raw, std::size_t kind, std::vector<std::string>& stack) {
        std::map<std::string, Merged>& memo = m_memo[kind];
        if (auto found = memo.find(raw.name); found != memo.end()) {
            return found->second;
        }
        if (std::ranges::find(stack, raw.name) != stack.end()) {
            return std::unexpected(error(raw.file, std::format("preset '{}' inherits from itself", raw.name)));
        }
        stack.push_back(raw.name);
        Merged merged;
        for (std::string_view field : fieldsOf(kind)) {
            if (const JsonNode* value = findPath(*raw.node, field)) {
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
        if (kind == kConfigure) {
            addMap(merged.cacheVariables, raw.node->find("cacheVariables"));
        }
        addMap(merged.environment, raw.node->find("environment"));
        const std::vector<RawPreset>& raws = m_raw[kind];
        for (const std::string& parentName : inheritsOf(raw)) {
            const auto parent = std::ranges::find(raws, parentName, &RawPreset::name);
            if (parent == raws.end()) {
                return std::unexpected(error(
                    raw.file, std::format("preset '{}' inherits from unknown preset '{}'", raw.name, parentName)));
            }
            if (!reachable(raw.file, parent->file)) {
                return std::unexpected(
                    error(raw.file, std::format("preset '{}' inherits '{}', which is not in its file "
                                                "or a file it includes",
                                                raw.name, parentName)));
            }
            auto parentMerged = merge(*parent, kind, stack);
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
            /*
             * CMake reads the characters after '$' as a macro namespace while they can still
             * begin "", "env", "penv" or "vendor"; any other text (another '$', a space, "e{")
             * is copied as written, including the character that ended the namespace, and the
             * scan goes on after it (so "$${x}" and "$a{b}" stay as written, while "$a{${x}}"
             * expands the inner macro). Checked against cmake 4.4 (CMakePresetsOracle).
             */
            {
                constexpr std::array<std::string_view, 3> kNamespaces = {"env", "penv", "vendor"};
                std::size_t j = i + 1;
                while (j < text.size() && text[j] != '{') {
                    const std::string_view prefix = text.substr(i + 1, j - i);
                    if (std::ranges::none_of(kNamespaces,
                                             [&](std::string_view ns) { return ns.starts_with(prefix); })) {
                        break;
                    }
                    ++j;
                }
                const std::string_view ns = text.substr(i + 1, j - i - 1);
                const bool known = j < text.size() && text[j] == '{' &&
                                   (ns.empty() || std::ranges::find(kNamespaces, ns) != kNamespaces.end());
                if (!known) {
                    const std::size_t end = std::min(j + 1, text.size()); // The ending character too.
                    out += text.substr(i, end - i);
                    i = end;
                    continue;
                }
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
            } else {
                // $vendor{}: left as written, and cmake ignores the preset that uses it (it is
                // neither listed nor an error), so resolve*() disable it (m_vendorMacro).
                const auto name = closing(8);
                if (!name) {
                    return std::unexpected(error(file, std::format("unterminated macro in '{}'", text)));
                }
                m_vendorMacro = true;
                out += rest.substr(0, name->size() + 9);
                i += name->size() + 9;
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

    static const JsonNode* field(const Merged& merged, std::string_view name) {
        const auto found = merged.fields.find(std::string(name));
        return found == merged.fields.end() ? nullptr : found->second;
    }

    // A string field, expanded; "" when absent. Errors: not a string, expansion errors.
    Result<std::string> stringField(const Merged& merged, std::string_view name, const Macros& macros,
                                    const std::string& file) {
        const JsonNode* value = field(merged, name);
        if (value == nullptr || value->isNull()) {
            return std::string();
        }
        if (!value->isString()) {
            return std::unexpected(error(file, std::format("\"{}\" is not a string", name)));
        }
        return expand(value->text, macros, file);
    }

    // A string or array-of-strings field, each item expanded. Errors: expansion errors.
    Result<std::vector<std::string>> listField(const Merged& merged, std::string_view name, const Macros& macros,
                                               const std::string& file) {
        std::vector<std::string> out;
        const JsonNode* node = field(merged, name);
        if (node == nullptr) {
            return out;
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
            auto expanded = expand(item->text, macros, file);
            if (!expanded) {
                return std::unexpected(std::move(expanded.error()));
            }
            out.push_back(std::move(*expanded));
        }
        return out;
    }

    static std::optional<bool> boolField(const Merged& merged, std::string_view name) {
        const JsonNode* node = field(merged, name);
        return node != nullptr && node->isBool() ? std::optional<bool>(node->boolean) : std::nullopt;
    }

    static std::optional<int> intField(const Merged& merged, std::string_view name) {
        const JsonNode* node = field(merged, name);
        return node != nullptr && node->integer ? std::optional<int>(static_cast<int>(*node->integer)) : std::nullopt;
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

    // The merged condition of a preset, evaluated with `macros`; true without one.
    Result<bool> enabled(const Merged& merged, const Macros& macros, const std::string& file) {
        const JsonNode* condition = field(merged, "condition");
        return condition == nullptr ? Result<bool>(true) : evaluate(*condition, macros, file);
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
        m_vendorMacro = false;
        std::vector<std::string> stack;
        auto merged = merge(raw, kConfigure, stack);
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

        for (const auto& [name, target] :
             {std::pair<std::string_view, std::string*>{"binaryDir", &preset.binaryDir},
              std::pair<std::string_view, std::string*>{"installDir", &preset.installDir},
              std::pair<std::string_view, std::string*>{"toolchainFile", &preset.toolchainFile}}) {
            auto value = stringField(*merged, name, macros, raw.file);
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            if (!value->empty()) {
                auto absolute = absolutePath(*value, m_sourceDir);
                if (!absolute) {
                    return std::unexpected(error(
                        raw.file, std::format("preset '{}': \"{}\" '{}' is not a valid path", raw.name, name, *value)));
                }
                *target = std::move(*absolute);
            }
        }

        for (const auto& [name, value, external] :
             {std::tuple<std::string_view, std::string*, bool*>{"architecture", &preset.architecture,
                                                                &preset.architectureExternal},
              std::tuple<std::string_view, std::string*, bool*>{"toolset", &preset.toolset, &preset.toolsetExternal}}) {
            const JsonNode* node = field(*merged, name);
            if (node == nullptr) {
                continue;
            }
            std::string text = node->isString() ? node->text : node->stringAt("value");
            auto expanded = expand(text, macros, raw.file);
            if (!expanded) {
                return std::unexpected(std::move(expanded.error()));
            }
            *value = std::move(*expanded);
            *external = node->isObject() && node->stringAt("strategy") == "external";
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

        // warnings, errors and debug, as cmake --preset passes them (cmake-presets(7)).
        const auto toggle = [&](std::string_view name, std::string_view on, std::string_view off) {
            if (const auto value = boolField(*merged, name)) {
                const std::string_view chosen = *value ? on : off;
                if (!chosen.empty()) {
                    preset.extraArguments.emplace_back(chosen);
                }
            }
        };
        toggle("warnings.dev", "-Wdev", "-Wno-dev");
        toggle("warnings.deprecated", "-Wdeprecated", "-Wno-deprecated");
        toggle("warnings.uninitialized", "--warn-uninitialized", "");
        toggle("warnings.unusedCli", "", "--no-warn-unused-cli");
        toggle("warnings.systemVars", "--check-system-vars", "");
        toggle("errors.dev", "-Werror=dev", "-Wno-error=dev");
        toggle("errors.deprecated", "-Werror=deprecated", "-Wno-error=deprecated");
        toggle("debug.output", "--debug-output", "");
        toggle("debug.tryCompile", "--debug-trycompile", "");
        toggle("debug.find", "--debug-find", "");

        auto environment = environmentOf(merged->environment, macros, raw.file);
        if (!environment) {
            return std::unexpected(std::move(environment.error()));
        }
        preset.environment = std::move(*environment);

        auto isEnabled = enabled(*merged, macros, raw.file);
        if (!isEnabled) {
            return std::unexpected(std::move(isEnabled.error()));
        }
        preset.enabled = *isEnabled && !m_vendorMacro; // cmake ignores a preset using $vendor{}.
        return preset;
    }

    // What build, test and package presets share: the configure preset named by
    // "configurePreset" and the environment overlay (header comment of CMakeBuildPreset).
    struct Dependent {
        Merged merged;
        std::string configurePreset;
        const CMakeConfigurePreset* configure = nullptr;
        std::vector<std::pair<std::string, const JsonNode*>> environment;
    };

    Result<Dependent> dependent(const RawPreset& raw, std::size_t kind, const CMakePresets& presets) {
        std::vector<std::string> stack;
        auto merged = merge(raw, kind, stack);
        if (!merged) {
            return std::unexpected(std::move(merged.error()));
        }
        Dependent out;
        out.merged = std::move(*merged);
        if (const JsonNode* configure = field(out.merged, "configurePreset");
            configure != nullptr && configure->isString()) {
            out.configurePreset = configure->text;
        }
        out.configure = presets.findConfigurePreset(out.configurePreset);
        const bool hidden = raw.node->boolAt("hidden");
        if (!hidden && out.configure == nullptr) {
            return std::unexpected(error(raw.file, std::format("{} '{}' names no known configure preset ('{}')",
                                                               kKindNames[kind], raw.name, out.configurePreset)));
        }
        if (out.configure != nullptr && !reachable(raw.file, out.configure->file)) {
            return std::unexpected(error(raw.file, std::format("{} '{}' names configure preset '{}', which is not in "
                                                               "its file or a file it includes",
                                                               kKindNames[kind], raw.name, out.configurePreset)));
        }
        // The environment: the preset's own entries, then (by default) the configure preset's.
        out.environment = out.merged.environment;
        const auto inherit = boolField(out.merged, "inheritConfigureEnvironment");
        if (out.configure != nullptr && inherit.value_or(true)) {
            std::vector<std::string> configureStack;
            const auto raws = std::ranges::find(m_raw[kConfigure], out.configure->name, &RawPreset::name);
            auto configureMerged = merge(*raws, kConfigure, configureStack);
            if (!configureMerged) {
                return std::unexpected(std::move(configureMerged.error()));
            }
            for (const auto& entry : configureMerged->environment) {
                if (std::ranges::none_of(out.environment, [&](const auto& e) { return e.first == entry.first; })) {
                    out.environment.push_back(entry);
                }
            }
        }
        return out;
    }

    template<class Preset>
    void common(Preset& preset, const RawPreset& raw) const {
        preset.name = raw.name;
        preset.file = raw.file;
        preset.user = raw.user;
        preset.displayName = raw.node->stringAt("displayName");
        preset.description = raw.node->stringAt("description");
        preset.hidden = raw.node->boolAt("hidden");
    }

    Macros macrosOf(const RawPreset& raw, const Dependent& base) const {
        return Macros{.presetName = raw.name,
                      .generator = base.configure != nullptr ? base.configure->generator : std::string(),
                      .fileDir = slashes(io::parentPath(raw.file)),
                      .environment = &base.environment};
    }

    Result<CMakeBuildPreset> resolveBuild(const RawPreset& raw, const CMakePresets& presets) {
        m_vendorMacro = false;
        auto base = dependent(raw, kBuild, presets);
        if (!base) {
            return std::unexpected(std::move(base.error()));
        }
        CMakeBuildPreset preset;
        common(preset, raw);
        preset.configurePreset = base->configurePreset;
        const Macros macros = macrosOf(raw, *base);
        auto configuration = stringField(base->merged, "configuration", macros, raw.file);
        auto targets = listField(base->merged, "targets", macros, raw.file);
        auto options = listField(base->merged, "nativeToolOptions", macros, raw.file);
        auto environment = environmentOf(base->environment, macros, raw.file);
        auto isEnabled = enabled(base->merged, macros, raw.file);
        if (!configuration || !targets || !options || !environment || !isEnabled) {
            return std::unexpected(!configuration ? configuration.error()
                                   : !targets     ? targets.error()
                                   : !options     ? options.error()
                                   : !environment ? environment.error()
                                                  : isEnabled.error());
        }
        preset.configuration = std::move(*configuration);
        preset.targets = std::move(*targets);
        preset.nativeToolOptions = std::move(*options);
        preset.jobs = intField(base->merged, "jobs");
        preset.cleanFirst = boolField(base->merged, "cleanFirst").value_or(false);
        preset.verbose = boolField(base->merged, "verbose").value_or(false);
        preset.environment = std::move(*environment);
        preset.enabled = *isEnabled && !m_vendorMacro; // cmake ignores a preset using $vendor{}.
        return preset;
    }

    Result<CMakeTestPreset> resolveTest(const RawPreset& raw, const CMakePresets& presets) {
        m_vendorMacro = false;
        auto base = dependent(raw, kTest, presets);
        if (!base) {
            return std::unexpected(std::move(base.error()));
        }
        CMakeTestPreset preset;
        common(preset, raw);
        preset.configurePreset = base->configurePreset;
        const Macros macros = macrosOf(raw, *base);
        for (const auto& [name, into] :
             {std::pair<std::string_view, std::string*>{"configuration", &preset.configuration},
              std::pair<std::string_view, std::string*>{"filter.include.name", &preset.includeName},
              std::pair<std::string_view, std::string*>{"filter.include.label", &preset.includeLabel},
              std::pair<std::string_view, std::string*>{"filter.exclude.name", &preset.excludeName},
              std::pair<std::string_view, std::string*>{"filter.exclude.label", &preset.excludeLabel},
              std::pair<std::string_view, std::string*>{"execution.noTestsAction", &preset.noTestsAction}}) {
            auto value = stringField(base->merged, name, macros, raw.file);
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            *into = std::move(*value);
        }
        preset.jobs = intField(base->merged, "execution.jobs");
        preset.timeout = intField(base->merged, "execution.timeout");
        preset.stopOnFailure = boolField(base->merged, "execution.stopOnFailure").value_or(false);
        preset.outputOnFailure = boolField(base->merged, "output.outputOnFailure").value_or(false);
        auto environment = environmentOf(base->environment, macros, raw.file);
        if (!environment) {
            return std::unexpected(std::move(environment.error()));
        }
        preset.environment = std::move(*environment);
        auto isEnabled = enabled(base->merged, macros, raw.file);
        if (!isEnabled) {
            return std::unexpected(std::move(isEnabled.error()));
        }
        preset.enabled = *isEnabled && !m_vendorMacro; // cmake ignores a preset using $vendor{}.
        return preset;
    }

    Result<CMakePackagePreset> resolvePackage(const RawPreset& raw, const CMakePresets& presets) {
        m_vendorMacro = false;
        auto base = dependent(raw, kPackage, presets);
        if (!base) {
            return std::unexpected(std::move(base.error()));
        }
        CMakePackagePreset preset;
        common(preset, raw);
        preset.configurePreset = base->configurePreset;
        const Macros macros = macrosOf(raw, *base);
        for (const auto& [name, into] :
             {std::pair<std::string_view, std::string*>{"packageName", &preset.packageName},
              std::pair<std::string_view, std::string*>{"packageVersion", &preset.packageVersion},
              std::pair<std::string_view, std::string*>{"packageDirectory", &preset.packageDirectory}}) {
            auto value = stringField(base->merged, name, macros, raw.file);
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            *into = std::move(*value);
        }
        auto generators = listField(base->merged, "generators", macros, raw.file);
        auto configurations = listField(base->merged, "configurations", macros, raw.file);
        auto environment = environmentOf(base->environment, macros, raw.file);
        auto isEnabled = enabled(base->merged, macros, raw.file);
        if (!generators || !configurations || !environment || !isEnabled) {
            return std::unexpected(!generators       ? generators.error()
                                   : !configurations ? configurations.error()
                                   : !environment    ? environment.error()
                                                     : isEnabled.error());
        }
        preset.generators = std::move(*generators);
        preset.configurations = std::move(*configurations);
        preset.environment = std::move(*environment);
        preset.enabled = *isEnabled && !m_vendorMacro; // cmake ignores a preset using $vendor{}.
        return preset;
    }

    // A workflow preset's steps, checked as CMake checks them (header comment, "Kinds").
    Result<CMakeWorkflowPreset> resolveWorkflow(const RawPreset& raw, const CMakePresets& presets) {
        CMakeWorkflowPreset preset;
        preset.name = raw.name;
        preset.file = raw.file;
        preset.user = raw.user;
        preset.displayName = raw.node->stringAt("displayName");
        preset.description = raw.node->stringAt("description");
        const JsonNode* steps = raw.node->find("steps");
        if (steps == nullptr || !steps->isArray() || steps->items.empty()) {
            return std::unexpected(error(raw.file, std::format("workflow preset '{}' has no steps", raw.name)));
        }
        std::string configurePreset;
        for (std::size_t i = 0; i < steps->items.size(); ++i) {
            const JsonNode& step = steps->items[i];
            const std::string type = step.stringAt("type");
            const std::string name = step.stringAt("name");
            std::optional<CMakePresetKind> kind;
            for (CMakePresetKind candidate : {CMakePresetKind::Configure, CMakePresetKind::Build, CMakePresetKind::Test,
                                              CMakePresetKind::Package}) {
                if (type == toString(candidate)) {
                    kind = candidate;
                }
            }
            if (!kind || name.empty()) {
                return std::unexpected(
                    error(raw.file,
                          std::format("workflow preset '{}': step {} has no valid type and name", raw.name, i + 1)));
            }
            if ((i == 0) != (*kind == CMakePresetKind::Configure)) {
                return std::unexpected(error(
                    raw.file,
                    std::format("workflow preset '{}': only the first step is a configure step, and it must be one",
                                raw.name)));
            }
            const auto stepError = [&](std::string_view why) {
                return error(raw.file, std::format("workflow preset '{}': step {} ({} preset '{}') {}", raw.name, i + 1,
                                                   type, name, why));
            };
            const auto check = [&](const auto& list) -> Result<std::string> {
                const auto found =
                    std::ranges::find(list, name, [](const auto& p) -> const std::string& { return p.name; });
                if (found == list.end()) {
                    return std::unexpected(stepError("does not exist"));
                }
                if (found->hidden) {
                    return std::unexpected(stepError("is hidden"));
                }
                if constexpr (requires { found->configurePreset; }) {
                    return found->configurePreset;
                } else {
                    return found->name;
                }
            };
            Result<std::string> uses = *kind == CMakePresetKind::Configure ? check(presets.configurePresets)
                                       : *kind == CMakePresetKind::Build   ? check(presets.buildPresets)
                                       : *kind == CMakePresetKind::Test    ? check(presets.testPresets)
                                                                           : check(presets.packagePresets);
            if (!uses) {
                return std::unexpected(std::move(uses.error()));
            }
            if (i == 0) {
                configurePreset = *uses;
            } else if (*uses != configurePreset) {
                return std::unexpected(stepError(std::format("does not use configure preset '{}'", configurePreset)));
            }
            preset.steps.push_back(CMakeWorkflowStep{.kind = *kind, .name = name});
        }
        return preset;
    }

    io::IFileSystem& m_fs;
    std::string m_sourceDir;
    CMakePresetsContext m_context;
    std::string m_projectFile;
    bool m_projectExists = false;
    std::int64_t m_projectVersion = 0;
    std::int64_t m_userVersion = 0;
    std::map<std::string, JsonNode> m_documents; // By absolute path; nodes never move once inserted.
    std::vector<std::string> m_fileOrder;
    // Each read file's includes (the user file's implicit include of the project file too).
    std::map<std::string, std::vector<std::string>> m_includes;
    std::array<std::vector<RawPreset>, kKinds> m_raw;
    std::array<std::map<std::string, Merged>, kKinds> m_memo;
    // Set by expand() when it meets a $vendor{} macro; resolve*() disable such a preset.
    bool m_vendorMacro = false;
};

template<class Preset>
void listVisible(const std::vector<Preset>& presets, std::vector<CMakePresetListEntry>& into) {
    for (const Preset& preset : presets) {
        if (!preset.hidden && preset.enabled) {
            into.push_back(CMakePresetListEntry{.name = preset.name, .displayName = preset.displayName});
        }
    }
}

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

std::string_view toString(CMakePresetKind kind) noexcept {
    switch (kind) {
    case CMakePresetKind::Configure: return "configure";
    case CMakePresetKind::Build: return "build";
    case CMakePresetKind::Test: return "test";
    case CMakePresetKind::Package: return "package";
    case CMakePresetKind::Workflow: return "workflow";
    }
    return "configure";
}

const CMakeConfigurePreset* CMakePresets::findConfigurePreset(std::string_view name) const noexcept {
    for (const CMakeConfigurePreset& preset : configurePresets) {
        if (preset.name == name) {
            return &preset;
        }
    }
    return nullptr;
}

const CMakeBuildPreset* CMakePresets::findBuildPreset(std::string_view name) const noexcept {
    for (const CMakeBuildPreset& preset : buildPresets) {
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

std::vector<const CMakeTestPreset*> CMakePresets::testPresetsFor(std::string_view configurePreset) const {
    std::vector<const CMakeTestPreset*> out;
    for (const CMakeTestPreset& preset : testPresets) {
        if (!preset.hidden && preset.enabled && preset.configurePreset == configurePreset) {
            out.push_back(&preset);
        }
    }
    return out;
}

CMakePresetListing CMakePresets::listing() const {
    CMakePresetListing out;
    listVisible(configurePresets, out.configure);
    listVisible(buildPresets, out.build);
    listVisible(testPresets, out.test);
    listVisible(packagePresets, out.package);
    for (const CMakeWorkflowPreset& preset : workflowPresets) {
        out.workflow.push_back(CMakePresetListEntry{.name = preset.name, .displayName = preset.displayName});
    }
    return out;
}

std::string formatCMakePresetListing(const CMakePresetListing& listing) {
    std::string out;
    for (const auto& [kind, entries] :
         {std::pair<std::string_view, const std::vector<CMakePresetListEntry>*>{"configure", &listing.configure},
          std::pair<std::string_view, const std::vector<CMakePresetListEntry>*>{"build", &listing.build},
          std::pair<std::string_view, const std::vector<CMakePresetListEntry>*>{"test", &listing.test},
          std::pair<std::string_view, const std::vector<CMakePresetListEntry>*>{"package", &listing.package},
          std::pair<std::string_view, const std::vector<CMakePresetListEntry>*>{"workflow", &listing.workflow}}) {
        if (entries->empty()) {
            continue;
        }
        if (!out.empty()) {
            out += '\n';
        }
        out += std::format("Available {} presets:\n\n", kind);
        std::size_t longest = 0;
        for (const CMakePresetListEntry& entry : *entries) {
            longest = std::max(longest, entry.name.size());
        }
        for (const CMakePresetListEntry& entry : *entries) {
            out += std::format("  \"{}\"", entry.name);
            if (!entry.displayName.empty()) {
                out += std::string(longest - entry.name.size(), ' ');
                out += std::format(" - {}", entry.displayName);
            }
            out += '\n';
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
