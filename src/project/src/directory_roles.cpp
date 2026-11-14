// SPDX-License-Identifier: Apache-2.0
//
// Directory roles and their persisted overrides (frost/project/directory_roles.hpp, UX-23).

#include "frost/project/directory_roles.hpp"

#include "frost/core/mutex.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/cmake_presets.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace frost::project {
namespace {

constexpr std::string_view kStoreHeader = "# FrostIDE directory roles v1";
constexpr std::string_view kStoreHeaderPrefix = "# FrostIDE directory roles v";
// Rule 1: never project content, never overridden.
constexpr std::array<std::string_view, 4> kStateNames{".git", ".svn", ".hg", ".vs"};
// Rule 4: exact case.
constexpr std::array<std::string_view, 3> kBuildNames{"build", "out", ".cache"};
constexpr std::string_view kCMakeBuildPrefix = "cmake-build-";
// Rule 4b: exact case.
constexpr std::string_view kDependencyName = "node_modules";
// Rule 6: ASCII case ignored, lower case here.
constexpr std::array<std::string_view, 8> kMSBuildOutputNames{"bin", "obj",   "ipch",  "x64",
                                                              "x86", "arm64", "debug", "release"};
constexpr std::array<std::string_view, 7> kMSBuildProjectExtensions{".sln",    ".slnx",   ".vcxproj", ".vcxitems",
                                                                    ".csproj", ".vbproj", ".fsproj"};
// Rule 6b: ASCII case ignored, lower case here (Unity's generated folders beside Assets/).
constexpr std::array<std::string_view, 6> kUnityGeneratedNames{"library",      "temp",           "logs",
                                                               "usersettings", "memorycaptures", "obj"};
// The decision cache is dropped when it grows past this (a watcher may see many new folders).
constexpr std::size_t kMaxCachedDecisions = 1U << 16;

[[nodiscard]] char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

[[nodiscard]] bool isSeparator(char c) noexcept { return c == '/' || c == '\\'; }

// A drive-letter or UNC path: compared with ASCII case ignored.
[[nodiscard]] bool windowsPath(std::string_view path) noexcept {
    return (path.size() >= 2 && path[1] == ':') || (path.size() >= 2 && isSeparator(path[0]) && isSeparator(path[1]));
}

// `path` without trailing separators (a root such as "C:\" or "/" keeps one).
[[nodiscard]] std::string_view trimmed(std::string_view path) noexcept {
    while (path.size() > 1 && isSeparator(path.back()) && !(path.size() == 3 && path[1] == ':'))
        path.remove_suffix(1);
    return path;
}

// The comparison key: '/' separators, ASCII case folded on Windows paths. Length-preserving, so a
// position in the key is the same position in the path.
[[nodiscard]] std::string pathKey(std::string_view path) {
    const bool fold = windowsPath(path);
    std::string key(path);
    for (char& c : key) {
        if (c == '\\')
            c = '/';
        else if (fold)
            c = lower(c);
    }
    return key;
}

[[nodiscard]] bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
}

[[nodiscard]] bool endsWithIgnoringCase(std::string_view text, std::string_view suffix) noexcept {
    return text.size() > suffix.size() && equalsIgnoringCase(text.substr(text.size() - suffix.size()), suffix);
}

[[nodiscard]] std::string_view lastComponent(std::string_view path) noexcept {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

[[nodiscard]] Result<std::string> normalizedDirectory(std::string_view path) {
    if (path.empty() || path.find('\0') != std::string_view::npos || path.find('\n') != std::string_view::npos ||
        path.find('\r') != std::string_view::npos)
        return fail(ErrorCode::InvalidArgument, "a directory path must be one line without NUL");
    if (!io::isAbsolutePath(path))
        return fail(ErrorCode::InvalidArgument, std::format("'{}' is not an absolute path", path));
    auto normalized = io::normalizePath(path);
    if (!normalized)
        return std::unexpected(std::move(normalized.error()));
    return std::string(trimmed(*normalized));
}

} // namespace

std::string_view directoryRoleName(DirectoryRole role) noexcept {
    switch (role) {
    case DirectoryRole::Source: return "source";
    case DirectoryRole::Library: return "library";
    case DirectoryRole::Generated: return "generated";
    case DirectoryRole::Excluded: return "excluded";
    }
    return "source";
}

std::optional<DirectoryRole> parseDirectoryRole(std::string_view name) noexcept {
    for (const DirectoryRole role :
         {DirectoryRole::Source, DirectoryRole::Library, DirectoryRole::Generated, DirectoryRole::Excluded}) {
        if (name == directoryRoleName(role))
            return role;
    }
    return std::nullopt;
}

// --- DirectoryRoles ---

struct DirectoryRoles::State {
    io::IFileSystem* fs = nullptr;
    std::string root;    // Normalised, without a trailing separator.
    std::string rootKey; // pathKey(root).
    std::vector<std::string> generated;
    std::vector<DirectoryRoleOverride> overrides;
    std::unordered_set<std::string> generatedKeys;
    std::unordered_map<std::string, DirectoryRole> overrideKeys;
    std::optional<DirectoryRole> rootOverride;
    std::string presetsProblem;

    mutable Mutex mutex;
    // Rules 5 and 6 by folder key; parents' "holds an MSBuild project" by folder key.
    mutable std::unordered_map<std::string, DirectoryRoleDecision> decisions FROST_GUARDED_BY(mutex);
    mutable std::unordered_map<std::string, bool> msbuildParents FROST_GUARDED_BY(mutex);
    // Parents' "is a Unity project" (rule 6b) by folder key.
    mutable std::unordered_map<std::string, bool> unityParents FROST_GUARDED_BY(mutex);

    // True when `key` lies strictly below the root.
    [[nodiscard]] bool below(std::string_view key) const noexcept {
        return key.size() > rootKey.size() && key.starts_with(rootKey) &&
               (rootKey.back() == '/' || key[rootKey.size()] == '/');
    }

    [[nodiscard]] bool parentHoldsMSBuildProject(std::string_view directory) const {
        const std::string_view parent = io::parentPath(directory);
        const std::string key = pathKey(parent);
        {
            LockGuard lock(mutex);
            if (const auto it = msbuildParents.find(key); it != msbuildParents.end())
                return it->second;
        }
        bool holds = false;
        if (auto entries = fs->listDirectory(parent)) {
            holds = std::ranges::any_of(*entries, [](const io::DirEntry& entry) {
                return entry.type == io::FileType::Regular &&
                       std::ranges::any_of(kMSBuildProjectExtensions, [&](std::string_view extension) {
                           return endsWithIgnoringCase(entry.name, extension);
                       });
            });
        }
        LockGuard lock(mutex);
        if (msbuildParents.size() >= kMaxCachedDecisions)
            msbuildParents.clear();
        msbuildParents.emplace(key, holds);
        return holds;
    }

    // Rule 6b: the parent of `directory` holds both an Assets and a ProjectSettings folder.
    [[nodiscard]] bool parentIsUnityProject(std::string_view directory) const {
        const std::string_view parent = io::parentPath(directory);
        const std::string key = pathKey(parent);
        {
            LockGuard lock(mutex);
            if (const auto it = unityParents.find(key); it != unityParents.end())
                return it->second;
        }
        const auto isFolder = [&](std::string_view name) {
            auto child = io::joinPath(parent, name);
            if (!child)
                return false;
            auto status = fs->status(*child);
            return status && status->type == io::FileType::Directory;
        };
        const bool unity = isFolder("Assets") && isFolder("ProjectSettings");
        LockGuard lock(mutex);
        if (unityParents.size() >= kMaxCachedDecisions)
            unityParents.clear();
        unityParents.emplace(key, unity);
        return unity;
    }

    // Rules 5, 6 and 6b (the file-system rules), cached.
    [[nodiscard]] DirectoryRoleDecision probe(std::string_view directory, const std::string& key) const {
        {
            LockGuard lock(mutex);
            if (const auto it = decisions.find(key); it != decisions.end())
                return it->second;
        }
        DirectoryRoleDecision decision;
        if (auto cache = io::joinPath(directory, "CMakeCache.txt")) {
            auto status = fs->status(*cache);
            if (status && status->type == io::FileType::Regular)
                decision = {DirectoryRole::Generated, DirectoryRoleReason::CMakeCache};
        }
        if (decision.reason == DirectoryRoleReason::Default) {
            const std::string_view name = lastComponent(directory);
            const bool outputName = std::ranges::any_of(
                kMSBuildOutputNames, [&](std::string_view output) { return equalsIgnoringCase(name, output); });
            if (outputName && parentHoldsMSBuildProject(directory))
                decision = {DirectoryRole::Generated, DirectoryRoleReason::MSBuildOutput};
        }
        if (decision.reason == DirectoryRoleReason::Default) {
            const std::string_view name = lastComponent(directory);
            const bool unityName = std::ranges::any_of(
                kUnityGeneratedNames, [&](std::string_view generated) { return equalsIgnoringCase(name, generated); });
            if (unityName && parentIsUnityProject(directory))
                decision = {DirectoryRole::Generated, DirectoryRoleReason::UnityProject};
        }
        LockGuard lock(mutex);
        if (decisions.size() >= kMaxCachedDecisions)
            decisions.clear();
        decisions.emplace(key, decision);
        return decision;
    }

    [[nodiscard]] DirectoryRoleDecision decide(std::string_view directory) const {
        directory = trimmed(directory);
        const std::string key = pathKey(directory);
        if (key == rootKey) {
            if (rootOverride)
                return {*rootOverride, DirectoryRoleReason::Override};
            return {};
        }
        if (!below(key))
            return {};
        const std::string_view name = lastComponent(directory);
        if (std::ranges::find(kStateNames, name) != kStateNames.end())
            return {DirectoryRole::Excluded, DirectoryRoleReason::VersionControl};
        if (const auto it = overrideKeys.find(key); it != overrideKeys.end())
            return {it->second, DirectoryRoleReason::Override};
        if (generatedKeys.contains(key))
            return {DirectoryRole::Generated, DirectoryRoleReason::CMakePreset};
        if (std::ranges::find(kBuildNames, name) != kBuildNames.end() || name.starts_with(kCMakeBuildPrefix))
            return {DirectoryRole::Generated, DirectoryRoleReason::BuildName};
        if (name == kDependencyName)
            return {DirectoryRole::Generated, DirectoryRoleReason::DependencyName};
        return probe(directory, key);
    }
};

DirectoryRoles::DirectoryRoles(std::unique_ptr<State> state) noexcept : m_state(std::move(state)) {}
DirectoryRoles::~DirectoryRoles() = default;

Result<std::shared_ptr<const DirectoryRoles>> DirectoryRoles::create(io::IFileSystem& fs, const std::string& root,
                                                                     Options options) {
    auto normalized = normalizedDirectory(root);
    if (!normalized)
        return std::unexpected(std::move(normalized.error()));
    auto state = std::make_unique<State>();
    state->fs = &fs;
    state->root = std::move(*normalized);
    state->rootKey = pathKey(state->root);
    for (std::string& directory : options.generatedDirectories) {
        auto path = normalizedDirectory(directory);
        if (!path)
            continue;
        const std::string key = pathKey(*path);
        if (state->below(key) && state->generatedKeys.insert(key).second)
            state->generated.push_back(std::move(*path));
    }
    for (DirectoryRoleOverride& entry : options.overrides) {
        auto path = normalizedDirectory(entry.path);
        if (!path)
            continue;
        const std::string key = pathKey(*path);
        if (key == state->rootKey) {
            state->rootOverride = entry.role;
        } else if (!state->below(key)) {
            continue;
        }
        state->overrideKeys[key] = entry.role;
        state->overrides.push_back({std::move(*path), entry.role});
    }
    return std::shared_ptr<const DirectoryRoles>(new DirectoryRoles(std::move(state)));
}

// NOLINTNEXTLINE(performance-unnecessary-value-param): Own root snapshot across filesystem callbacks.
Result<std::shared_ptr<const DirectoryRoles>> DirectoryRoles::load(io::IFileSystem& fs, std::string root,
                                                                   std::vector<DirectoryRoleOverride> overrides) {
    Options options;
    options.overrides = std::move(overrides);
    std::string problem;
    if (io::isAbsolutePath(root)) {
        auto presets = loadCMakePresets(fs, root);
        if (!presets) {
            problem = presets.error().describe();
        } else if (*presets) {
            for (const CMakeConfigurePreset& preset : (*presets)->configurePresets) {
                if (!preset.binaryDir.empty())
                    options.generatedDirectories.push_back(preset.binaryDir);
            }
        }
    }
    auto roles = create(fs, root, std::move(options));
    if (roles)
        const_cast<State&>(*(*roles)->m_state).presetsProblem = std::move(problem); // Not yet shared.
    return roles;
}

const std::string& DirectoryRoles::root() const noexcept { return m_state->root; }
const std::vector<std::string>& DirectoryRoles::generatedDirectories() const noexcept { return m_state->generated; }
const std::vector<DirectoryRoleOverride>& DirectoryRoles::overrides() const noexcept { return m_state->overrides; }
const std::string& DirectoryRoles::presetsProblem() const noexcept { return m_state->presetsProblem; }

DirectoryRoleDecision DirectoryRoles::decide(std::string_view directory) const { return m_state->decide(directory); }

bool DirectoryRoles::traversed(std::string_view directory) const { return isTraversed(decide(directory).role); }

DirectoryRole DirectoryRoles::roleOf(std::string_view directory) const {
    directory = trimmed(directory);
    const std::string key = pathKey(directory);
    DirectoryRole role = m_state->rootOverride.value_or(DirectoryRole::Source);
    if (key == m_state->rootKey || !m_state->below(key))
        return key == m_state->rootKey ? role : DirectoryRole::Source;
    if (!isTraversed(role))
        return role;
    // Every folder from the root's first child down to `directory` itself.
    for (std::size_t at = m_state->rootKey.size() + (m_state->rootKey.back() == '/' ? 0 : 1); at <= key.size();) {
        std::size_t next = key.find('/', at);
        if (next == std::string::npos)
            next = key.size();
        const DirectoryRoleDecision decision = m_state->decide(directory.substr(0, next));
        if (decision.reason != DirectoryRoleReason::Default)
            role = decision.role;
        if (!isTraversed(role))
            return role;
        at = next + 1;
    }
    return role;
}

bool DirectoryRoles::belowUntraversed(std::string_view path) const {
    path = trimmed(path);
    const std::string key = pathKey(path);
    if (!m_state->below(key))
        return false;
    if (m_state->rootOverride && !isTraversed(*m_state->rootOverride))
        return true;
    // Every folder strictly between the root and `path`.
    std::size_t at = m_state->rootKey.size() + (m_state->rootKey.back() == '/' ? 0 : 1);
    for (;;) {
        const std::size_t next = key.find('/', at);
        if (next == std::string::npos)
            return false;
        if (!isTraversed(m_state->decide(path.substr(0, next)).role))
            return true;
        at = next + 1;
    }
}

// --- DirectoryRoleStore ---

struct DirectoryRoleStore::State {
    io::IFileSystem* fs = nullptr;
    std::string path; // "" for an in-memory store.
    mutable Mutex mutex;
    std::vector<DirectoryRoleOverride> overrides FROST_GUARDED_BY(mutex);
    std::vector<std::string> warnings FROST_GUARDED_BY(mutex);

    // Re-reads the file. Errors: Unsupported, readFile's other than NotFound.
    [[nodiscard]] Result<void> reloadLocked() FROST_REQUIRES(mutex) {
        if (path.empty())
            return {};
        auto content = fs->readFile(path);
        if (!content) {
            if (content.error().code() == ErrorCode::NotFound) {
                overrides.clear();
                return {};
            }
            return std::unexpected(std::move(content.error()));
        }
        std::vector<DirectoryRoleOverride> read;
        std::vector<std::string> problems;
        std::string_view rest = *content;
        bool first = true;
        std::size_t number = 0;
        while (!rest.empty()) {
            const std::size_t end = rest.find('\n');
            std::string_view line = rest.substr(0, end);
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
            ++number;
            if (line.ends_with('\r'))
                line.remove_suffix(1);
            if (first) {
                first = false;
                if (line.starts_with(kStoreHeaderPrefix) && line != kStoreHeader)
                    return fail(ErrorCode::Unsupported, std::format("{}: unsupported version '{}'", path, line));
                if (line == kStoreHeader)
                    continue;
                problems.push_back(std::format("{}:1: no header line", path));
            }
            if (line.empty() || line.starts_with('#'))
                continue;
            const std::size_t tab = line.find('\t');
            const auto role = tab == std::string_view::npos ? std::nullopt : parseDirectoryRole(line.substr(0, tab));
            auto directory = tab == std::string_view::npos
                                 ? Result<std::string>(fail(ErrorCode::InvalidArgument, "no tab"))
                                 : normalizedDirectory(line.substr(tab + 1));
            if (!role || !directory) {
                problems.push_back(std::format("{}:{}: skipped '{}'", path, number, line));
                continue;
            }
            std::erase_if(
                read, [&](const DirectoryRoleOverride& entry) { return pathKey(entry.path) == pathKey(*directory); });
            read.push_back({std::move(*directory), *role});
        }
        overrides = std::move(read);
        warnings = std::move(problems);
        return {};
    }

    [[nodiscard]] Result<void> saveLocked() FROST_REQUIRES(mutex) {
        if (path.empty())
            return {};
        std::string content(kStoreHeader);
        content += '\n';
        for (const DirectoryRoleOverride& entry : overrides)
            content += std::format("{}\t{}\n", directoryRoleName(entry.role), entry.path);
        if (const std::string_view parent = io::parentPath(path); !parent.empty()) {
            if (auto created = fs->createDirectories(parent); !created)
                return created;
        }
        return fs->writeFileAtomic(path, content);
    }
};

DirectoryRoleStore::DirectoryRoleStore(std::unique_ptr<State> state) noexcept : m_state(std::move(state)) {}
DirectoryRoleStore::~DirectoryRoleStore() = default;

Result<std::unique_ptr<DirectoryRoleStore>> DirectoryRoleStore::open(io::IFileSystem& fs, std::string path) {
    if (!io::isAbsolutePath(path))
        return fail(ErrorCode::InvalidArgument, "the directory roles file must be an absolute path");
    auto state = std::make_unique<State>();
    state->fs = &fs;
    state->path = std::move(path);
    {
        LockGuard lock(state->mutex);
        if (auto loaded = state->reloadLocked(); !loaded)
            return std::unexpected(std::move(loaded.error()));
    }
    return std::unique_ptr<DirectoryRoleStore>(new DirectoryRoleStore(std::move(state)));
}

std::unique_ptr<DirectoryRoleStore> DirectoryRoleStore::inMemory(io::IFileSystem& fs) {
    auto state = std::make_unique<State>();
    state->fs = &fs;
    return std::unique_ptr<DirectoryRoleStore>(new DirectoryRoleStore(std::move(state)));
}

std::vector<DirectoryRoleOverride> DirectoryRoleStore::overridesFor(std::string_view root) const {
    std::vector<DirectoryRoleOverride> result;
    auto normalized = normalizedDirectory(root);
    if (!normalized)
        return result;
    const std::string rootKey = pathKey(*normalized);
    LockGuard lock(m_state->mutex);
    for (const DirectoryRoleOverride& entry : m_state->overrides) {
        const std::string key = pathKey(entry.path);
        if (key == rootKey || (key.starts_with(rootKey) && key.size() > rootKey.size() &&
                               (rootKey.back() == '/' || key[rootKey.size()] == '/')))
            result.push_back(entry);
    }
    std::ranges::sort(result, {}, &DirectoryRoleOverride::path);
    return result;
}

Result<void> DirectoryRoleStore::set(std::string_view directory, std::optional<DirectoryRole> role) {
    auto normalized = normalizedDirectory(directory);
    if (!normalized)
        return std::unexpected(std::move(normalized.error()));
    LockGuard lock(m_state->mutex);
    if (auto reloaded = m_state->reloadLocked(); !reloaded)
        return reloaded;
    std::vector<DirectoryRoleOverride> previous = m_state->overrides;
    const std::string key = pathKey(*normalized);
    std::erase_if(m_state->overrides, [&](const DirectoryRoleOverride& entry) { return pathKey(entry.path) == key; });
    if (role)
        m_state->overrides.push_back({std::move(*normalized), *role});
    if (auto saved = m_state->saveLocked(); !saved) {
        m_state->overrides = std::move(previous);
        return saved;
    }
    return {};
}

std::vector<std::string> DirectoryRoleStore::warnings() const {
    LockGuard lock(m_state->mutex);
    return m_state->warnings;
}

} // namespace frost::project
