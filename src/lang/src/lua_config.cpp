// SPDX-License-Identifier: Apache-2.0
// LuaLS discovery (the bundled and pinned SDK installs first, ADR 0079), its launch arguments and the
// workspace configuration, including the FrostEngine defaults for a FrostEngine project root (a
// .frostproj or .feproject file; isFrostEngineProjectRoot, shared with the FrostEngine tool window).
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/lua_service.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
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
} // namespace frost::lang
