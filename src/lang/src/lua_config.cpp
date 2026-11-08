// SPDX-License-Identifier: Apache-2.0
// LuaLS discovery (the bundled and pinned SDK installs first, ADR 0079), its launch arguments and the
// workspace configuration, including the FrostEngine defaults for a FrostEngine project root (a
// .frostproj or .feproject file; isFrostEngineProjectRoot, shared with the FrostEngine tool window)
// and what they learn from the engine's generated scripting reference (frost_engine_lua.hpp, FEC-10a).
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/frost_engine_lua.hpp"
#include "frost/lang/lua_service.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <set>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace frost::lang {
namespace {
#ifdef _WIN32
constexpr std::string_view kName = "lua-language-server.exe";
constexpr char kSeparator = ';';
#else
constexpr std::string_view kName = "lua-language-server";
constexpr char kSeparator = ':';
#endif
bool regular(io::IFileSystem& fs, std::string_view path) {
    auto status = fs.status(path);
    return status && status->type == io::FileType::Regular;
}
std::optional<std::string> candidate(io::IFileSystem& fs, std::string_view dir) {
    if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"')
        dir = dir.substr(1, dir.size() - 2);
    if (!io::isAbsolutePath(dir))
        return std::nullopt;
    auto path = io::joinPath(dir, kName);
    if (!path)
        return std::nullopt;
    auto normalized = io::normalizePath(*path);
    if (normalized && regular(fs, *normalized))
        return std::move(*normalized);
    return std::nullopt;
}
// The value of an environment variable (UTF-8), or "" when unset.
std::string environment(const char* name) {
#ifdef _WIN32
    auto wide = io::toWide(name);
    if (!wide)
        return {};
    const DWORD needed = GetEnvironmentVariableW(wide->c_str(), nullptr, 0);
    if (needed == 0)
        return {};
    std::wstring value(needed, L'\0');
    const DWORD length = GetEnvironmentVariableW(wide->c_str(), value.data(), needed);
    if (length == 0 || length >= needed)
        return {};
    value.resize(length);
    auto utf8 = io::fromWide(value);
    return utf8 ? std::move(*utf8) : std::string();
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}
// The directory of this process's executable, or "" when it cannot be read.
std::string executableDirectory() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0 || len >= buffer.size())
        return {};
    buffer.resize(len);
    auto exePath = io::fromWide(buffer);
    return exePath ? std::string(io::parentPath(*exePath)) : std::string();
#elif defined(__linux__)
    std::string buffer(4096, '\0');
    const ssize_t len = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (len <= 0)
        return {};
    buffer.resize(static_cast<std::size_t>(len));
    return std::string(io::parentPath(buffer));
#else
    return {};
#endif
}
// <root>/luals-<pinned version>/bin, the layout of bootstrap's SDK install, tools/stage.ps1 and tools/stage.sh.
void addPinnedBin(std::vector<std::string>& out, std::string_view root) {
    if (root.empty())
        return;
    if (auto bin = io::joinPath(root, std::format("luals-{}/bin", pinnedLuaLanguageServerVersion())))
        out.push_back(std::move(*bin));
}
} // namespace
std::string_view pinnedLuaLanguageServerVersion() noexcept { return FROST_PINNED_LUALS_VERSION; }
Result<std::string> findLuaLanguageServer(io::IFileSystem& fs, const LuaSearch& search) {
    if (!search.configuredPath.empty()) {
        if (!io::isAbsolutePath(search.configuredPath))
            return fail(ErrorCode::InvalidArgument, "LuaLS path must be absolute");
        auto path = io::normalizePath(search.configuredPath);
        if (!path || !regular(fs, *path))
            return fail(ErrorCode::NotFound, "configured LuaLS executable does not exist");
        return *path;
    }
    for (const auto& dir : search.directories)
        if (auto path = candidate(fs, dir))
            return *path;
    std::string_view rest = search.pathVariable;
    while (!rest.empty()) {
        auto pos = rest.find(kSeparator);
        auto dir = rest.substr(0, pos);
        if (auto path = candidate(fs, dir))
            return *path;
        if (pos == std::string_view::npos)
            break;
        rest.remove_prefix(pos + 1);
    }
    return fail(ErrorCode::NotFound, std::format("LuaLS {} was not found in the bundled folder, the pinned SDK or PATH",
                                                 pinnedLuaLanguageServerVersion()));
}
LuaSearch defaultLuaSearch(std::string configuredPath) {
    LuaSearch search;
    search.configuredPath = std::move(configuredPath);
    // A staged FrostIDE bundles the pinned LuaLS beside the executable (tools/stage.ps1, tools/stage.sh).
    if (const std::string exeDir = executableDirectory(); !exeDir.empty()) {
        addPinnedBin(search.directories, exeDir);
        if (auto sub = io::joinPath(exeDir, "lua-language-server/bin")) // Layout of earlier previews.
            search.directories.push_back(std::move(*sub));
        search.directories.push_back(exeDir);
    }
    // Development and tests: the pinned install of tools/bootstrap.ps1 or tools/bootstrap-wsl.sh,
    // named by FROST_SDK (tools/env.ps1, ~/sdk/env.sh). No machine-specific default root (FND-10).
    addPinnedBin(search.directories, environment("FROST_SDK"));
    search.pathVariable = environment("PATH");
    return search;
}
Result<std::vector<std::string>> luaLanguageServerArguments(std::string_view stateDirectory) {
    if (!io::isAbsolutePath(stateDirectory))
        return fail(ErrorCode::InvalidArgument, "the LuaLS state directory must be absolute");
    auto log = io::joinPath(stateDirectory, "log");
    if (!log)
        return std::unexpected(std::move(log.error()));
    auto meta = io::joinPath(stateDirectory, "meta");
    if (!meta)
        return std::unexpected(std::move(meta.error()));
    return std::vector<std::string>{"--logpath=" + *log, "--metapath=" + *meta};
}
bool isFrostEngineProjectFileName(std::string_view name) noexcept {
    // ASCII case is ignored on every platform, so a project copied between Windows and Linux is
    // recognised the same way; the stem must not be empty (".frostproj" alone is no project).
    const auto endsWithIgnoringCase = [name](std::string_view suffix) {
        if (name.size() <= suffix.size())
            return false;
        const std::string_view tail = name.substr(name.size() - suffix.size());
        return std::ranges::equal(tail, suffix, [](char a, char b) {
            const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
            return lower(a) == lower(b);
        });
    };
    return endsWithIgnoringCase(".frostproj") || endsWithIgnoringCase(".feproject");
}
bool isFrostEngineProjectRoot(io::IFileSystem& fs, std::string_view root) {
    if (root.empty())
        return false;
    auto entries = fs.listDirectory(root);
    if (!entries)
        return false;
    return std::ranges::any_of(*entries, [](const io::DirEntry& entry) {
        return entry.type == io::FileType::Regular && isFrostEngineProjectFileName(entry.name);
    });
}
bool isFrostEngineLuaWorkspace(io::IFileSystem& fs, std::string_view root) {
    return isFrostEngineProjectRoot(fs, root);
}
Json luaDefaults(std::string runtimeVersion, bool frostEngine) {
    Json runtime = Json::object().set("version", std::move(runtimeVersion));
    Json workspace = Json::object().set("checkThirdParty", false);
    if (frostEngine) {
        runtime.set("path", Json::array({"Assets/Scripts/?.lua"}));
        runtime.set(
            "builtin",
            Json::object().set("io", "disable").set("package", "disable").set("os", "disable").set("debug", "disable"));
        workspace.set("library", Json::array({".frost/lua-api"}));
    }
    return Json::object().set("runtime", std::move(runtime)).set("workspace", std::move(workspace));
}
Json luaConfigurationSection(const Json& configuration, std::string_view section) {
    if (section == "Lua")
        return configuration;
    if (!section.starts_with("Lua."))
        return Json();
    section.remove_prefix(4);
    const Json* value = &configuration;
    while (!section.empty()) {
        auto dot = section.find('.');
        value = value->find(section.substr(0, dot));
        if (!value)
            return Json();
        if (dot == std::string_view::npos)
            return *value;
        section.remove_prefix(dot + 1);
    }
    return Json();
}
namespace {
// The identifier `text` starts with ("Wait" of "Wait(seconds: number)"), or "".
std::string_view leadingIdentifier(std::string_view text) noexcept {
    const auto start = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    const auto rest = [&](char c) { return start(c) || (c >= '0' && c <= '9'); };
    if (text.empty() || !start(text.front()))
        return {};
    std::size_t length = 1;
    while (length < text.size() && rest(text[length]))
        ++length;
    return text.substr(0, length);
}
bool directory(io::IFileSystem& fs, std::string_view path) {
    auto status = fs.status(path);
    return status && status->type == io::FileType::Directory;
}
// Adds the generated definitions folder of the project at `project` (relative to `root`, "" for the
// root itself), '/'-separated, when it exists.
void addLibrary(io::IFileSystem& fs, std::string_view root, std::string_view project, std::vector<std::string>& out) {
    const std::string relative = project.empty() ? std::string(kFrostEngineLuaApiDirectory)
                                                 : std::format("{}/{}", project, kFrostEngineLuaApiDirectory);
    if (auto path = io::joinPath(root, relative); path && directory(fs, *path))
        out.push_back(relative);
}
} // namespace
Result<FrostEngineScriptApi> parseFrostEngineScriptApi(std::string_view markdown) {
    if (markdown.find(kFrostEngineScriptApiMarker) == std::string_view::npos)
        return fail(ErrorCode::InvalidArgument,
                    std::format("not the engine's generated scripting reference (no \"{}\" line)",
                                kFrostEngineScriptApiMarker));
    // The sections whose entries are Lua globals, by the reference's own introductions (header comment).
    enum class Section : std::uint8_t { Other, Globals, GlobalTables };
    Section section = Section::Other;
    std::set<std::string, std::less<>> globals;
    FrostEngineScriptApi api;
    std::string_view rest = markdown;
    while (!rest.empty()) {
        const std::size_t end = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.starts_with("## ")) {
            const std::string_view title = line.substr(3);
            section = title == "Globals" ? Section::Globals
                      : title == "Namespaces" || title == "Types" || title == "Enums" || title == "Lua standard library"
                          ? Section::GlobalTables
                          : Section::Other;
            continue;
        }
        if (section == Section::Other)
            continue;
        if (line.starts_with("### ")) {
            if (const std::string_view name = leadingIdentifier(line.substr(4)); !name.empty())
                globals.emplace(name);
            continue;
        }
        // The Globals table: | `name(...)` | Kind | Summary |.
        if (section == Section::Globals && line.starts_with("| `")) {
            const std::string_view name = leadingIdentifier(line.substr(3));
            if (name.empty())
                continue;
            globals.emplace(name);
            // | `_VERSION: string` | GlobalValue | The Lua version ("Lua 5.4"). |
            if (name == "_VERSION") {
                if (const std::size_t quote = line.find("\"Lua "); quote != std::string_view::npos) {
                    if (const std::size_t close = line.find('"', quote + 1); close != std::string_view::npos)
                        api.runtimeVersion = std::string(line.substr(quote + 1, close - quote - 1));
                }
            }
        }
    }
    if (globals.empty())
        return fail(ErrorCode::InvalidArgument, "the engine's scripting reference declares no global");
    api.globals.assign(globals.begin(), globals.end());
    return api;
}
FrostEngineLuaProfile discoverFrostEngineLua(io::IFileSystem& fs, std::string_view root) {
    FrostEngineLuaProfile profile;
    if (root.empty() || !io::isAbsolutePath(root))
        return profile;
    profile.projectRoot = isFrostEngineProjectRoot(fs, root);
    bool engineCheckout = false;
    std::string problem;
    std::string folder(root);
    for (std::size_t depth = 0; depth <= kMaxReferenceDepth; ++depth) {
        auto candidate = io::joinPath(folder, kFrostEngineScriptApiReference);
        if (candidate && regular(fs, *candidate)) {
            auto text = fs.readFile(*candidate);
            if (!text) {
                problem = std::format("{} could not be read: {}", *candidate, text.error().describe());
                break;
            }
            auto api = parseFrostEngineScriptApi(*text);
            if (!api) {
                problem = std::format("{}: {}", *candidate, api.error().message());
                break;
            }
            // A reference in an ancestor applies to a FrostEngine project only (a sample inside the
            // engine's repository); the root itself holding it makes it an engine checkout.
            engineCheckout = depth == 0;
            if (engineCheckout || profile.projectRoot) {
                profile.reference = std::move(*candidate);
                profile.api = std::move(*api);
            }
            break;
        }
        const std::string parent(io::parentPath(folder));
        if (parent.empty() || parent == folder)
            break;
        folder = parent;
    }
    profile.frostEngine = profile.projectRoot || engineCheckout;
    if (!profile.frostEngine)
        return profile;
    if (profile.reference.empty())
        profile.problem = problem.empty() ? std::format("no generated {} in {} or its {} parent folders",
                                                        kFrostEngineScriptApiReference, root, kMaxReferenceDepth)
                                          : std::move(problem);
    if (profile.projectRoot)
        addLibrary(fs, root, {}, profile.library);
    if (engineCheckout) {
        // The engine's projects (samples, test projects) up to two levels below the checkout.
        const auto subdirectories = [&fs](std::string_view parent) {
            std::vector<std::string> names;
            if (auto entries = fs.listDirectory(parent))
                for (const io::DirEntry& entry : *entries)
                    if (entry.type == io::FileType::Directory && !entry.name.starts_with('.'))
                        names.push_back(entry.name);
            std::ranges::sort(names);
            return names;
        };
        for (const std::string& child : subdirectories(root)) {
            auto childPath = io::joinPath(root, child);
            if (!childPath)
                continue;
            if (isFrostEngineProjectRoot(fs, *childPath)) {
                addLibrary(fs, root, child, profile.library);
                continue;
            }
            for (const std::string& grandchild : subdirectories(*childPath)) {
                const std::string relative = child + "/" + grandchild;
                if (auto path = io::joinPath(root, relative); path && isFrostEngineProjectRoot(fs, *path))
                    addLibrary(fs, root, relative, profile.library);
            }
        }
    }
    return profile;
}
Json frostEngineLuaDefaults(std::string runtimeVersion, const FrostEngineLuaProfile& profile) {
    if (!profile.api.runtimeVersion.empty())
        runtimeVersion = profile.api.runtimeVersion;
    Json configuration = luaDefaults(std::move(runtimeVersion), profile.frostEngine);
    if (!profile.frostEngine)
        return configuration;
    if (!profile.library.empty()) {
        Json library = Json::array();
        for (const std::string& folder : profile.library)
            library.push(folder);
        if (Json* workspace = configuration.find("workspace"))
            workspace->set("library", std::move(library));
    }
    if (!profile.api.globals.empty()) {
        Json globals = Json::array();
        for (const std::string& name : profile.api.globals)
            globals.push(name);
        configuration.set("diagnostics", Json::object().set("globals", std::move(globals)));
    }
    return configuration;
}
} // namespace frost::lang
