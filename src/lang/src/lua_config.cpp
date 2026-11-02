// SPDX-License-Identifier: Apache-2.0
// LuaLS discovery and workspace configuration.
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/lua_service.hpp"

#include <cstdlib>
#include <format>
#ifdef _WIN32
#include <windows.h>
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
} // namespace
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
    return fail(ErrorCode::NotFound, "Lua language server not found");
}
LuaSearch defaultLuaSearch(std::string configuredPath) {
    LuaSearch search;
    search.configuredPath = std::move(configuredPath);
#ifdef _WIN32
    DWORD size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (size) {
        std::wstring value(size, L'\0');
        DWORD length = GetEnvironmentVariableW(L"PATH", value.data(), size);
        if (length && length < size) {
            value.resize(length);
            if (auto path = io::fromWide(value))
                search.pathVariable = std::move(*path);
        }
    }
#else
    if (const char* path = std::getenv("PATH"))
        search.pathVariable = path;
#endif
    return search;
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
