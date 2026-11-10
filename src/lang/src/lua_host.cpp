// SPDX-License-Identifier: Apache-2.0
//
// Lua host extension points (lua_host.hpp, LUA-13): the provider registry, the composition of
// host contributions into the LuaLS client configuration and the FrostIDE-owned meta folders.

#include "frost/lang/lua_host.hpp"

#include "frost/io/path.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/lang/uri.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <set>

namespace frost::lang {
namespace {

// Appends the strings of `from` to the JSON array `to` that it does not hold yet.
void appendUnique(Json& to, std::span<const std::string> from) {
    for (const std::string& item : from) {
        const bool present = std::ranges::any_of(
            to.items(), [&](const Json& existing) { return existing.asString() && *existing.asString() == item; });
        if (!present) {
            to.push(item);
        }
    }
}

// FNV-1a 64 (public domain algorithm by Fowler, Noll and Vo): a hash that is the same on every
// run and platform, for folder names that must survive restarts.
[[nodiscard]] std::uint64_t stableHash(std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] bool validMetaName(std::string_view name) noexcept {
    if (name.empty() || name.starts_with('/') || name.find('\\') != std::string_view::npos ||
        name.find(':') != std::string_view::npos || !name.ends_with(".lua")) {
        return false;
    }
    std::string_view rest = name;
    while (!rest.empty()) {
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
    }
    return true;
}

// The .lua files below `directory`, as '/'-separated names relative to it.
void listLuaFiles(io::IFileSystem& fs, const std::string& directory, const std::string& prefix,
                  std::vector<std::string>& out) {
    auto entries = fs.listDirectory(directory);
    if (!entries) {
        return;
    }
    for (const io::DirEntry& entry : *entries) {
        const std::string relative = prefix.empty() ? entry.name : prefix + "/" + entry.name;
        if (entry.type == io::FileType::Directory) {
            if (auto child = io::joinPath(directory, entry.name)) {
                listLuaFiles(fs, *child, relative, out);
            }
        } else if (entry.type == io::FileType::Regular && entry.name.ends_with(".lua")) {
            out.push_back(relative);
        }
    }
}

} // namespace

void LuaHostRegistry::add(std::shared_ptr<ILuaHostProvider> provider) {
    if (!provider) {
        return;
    }
    LockGuard lock(m_mutex);
    const std::string_view name = provider->name();
    std::erase_if(m_providers, [&](const auto& existing) { return existing->name() == name; });
    m_providers.push_back(std::move(provider));
}

bool LuaHostRegistry::remove(std::string_view name) {
    LockGuard lock(m_mutex);
    return std::erase_if(m_providers, [&](const auto& existing) { return existing->name() == name; }) > 0;
}

std::vector<std::string> LuaHostRegistry::names() const {
    LockGuard lock(m_mutex);
    std::vector<std::string> out;
    for (const auto& provider : m_providers) {
        out.emplace_back(provider->name());
    }
    return out;
}

std::vector<LuaHostContribution> LuaHostRegistry::contributions(io::IFileSystem& fs, std::string_view folder) const {
    std::vector<std::shared_ptr<ILuaHostProvider>> providers;
    {
        LockGuard lock(m_mutex);
        providers = m_providers; // Providers may read files; never under the lock.
    }
    std::vector<LuaHostContribution> out;
    for (const auto& provider : providers) {
        if (auto contribution = provider->contribute(fs, folder)) {
            if (contribution->provider.empty()) {
                contribution->provider = std::string(provider->name());
            }
            out.push_back(std::move(*contribution));
        }
    }
    return out;
}

Json composeLuaConfiguration(std::string userRuntimeVersion, std::span<const LuaHostContribution> contributions,
                             std::span<const std::string> metaLibraries) {
    for (const LuaHostContribution& contribution : contributions) {
        if (!contribution.runtimeVersion.empty()) {
            userRuntimeVersion = contribution.runtimeVersion;
            break;
        }
    }
    Json configuration = luaDefaults(std::move(userRuntimeVersion));
    Json path = Json::array();
    Json builtin = Json::object();
    Json library = Json::array();
    Json globals = Json::array();
    for (const LuaHostContribution& contribution : contributions) {
        appendUnique(path, contribution.runtimePath);
        for (const auto& [name, state] : contribution.builtins) {
            builtin.set(name, state);
        }
        appendUnique(library, contribution.library);
        appendUnique(globals, contribution.globals);
    }
    appendUnique(library, metaLibraries);
    if (Json* runtime = configuration.find("runtime")) {
        if (path.size() > 0) {
            runtime->set("path", std::move(path));
        }
        if (!builtin.keys().empty()) {
            runtime->set("builtin", std::move(builtin));
        }
    }
    if (library.size() > 0) {
        if (Json* workspace = configuration.find("workspace")) {
            workspace->set("library", std::move(library));
        }
    }
    if (globals.size() > 0) {
        configuration.set("diagnostics", Json::object().set("globals", std::move(globals)));
    }
    return configuration;
}

Result<std::string> luaHostMetaDirectory(std::string_view stateDirectory, std::string_view provider,
                                         std::string_view folder) {
    if (!io::isAbsolutePath(stateDirectory)) {
        return fail(ErrorCode::InvalidArgument, "the LuaLS state directory must be absolute");
    }
    if (provider.empty()) {
        return fail(ErrorCode::InvalidArgument, "a Lua host provider needs a name");
    }
    std::string safe;
    for (const char c : provider) {
        const bool plain =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        safe.push_back(plain ? c : '_');
    }
    return io::joinPath(stateDirectory, std::format("host-meta/{}-{:016x}", safe, stableHash(documentKey(folder))));
}

Result<void> writeLuaMetaFiles(io::IFileSystem& fs, std::string_view directory, std::span<const LuaMetaFile> files) {
    if (!io::isAbsolutePath(directory)) {
        return fail(ErrorCode::InvalidArgument, "the meta folder must be absolute");
    }
    std::set<std::string, std::less<>> wanted;
    for (const LuaMetaFile& file : files) {
        if (!validMetaName(file.name)) {
            return fail(ErrorCode::InvalidArgument, std::format("invalid Lua meta file name \"{}\"", file.name));
        }
        wanted.insert(file.name);
    }
    if (auto made = fs.createDirectories(directory); !made) {
        return std::unexpected(std::move(made.error().withContext("creating the Lua meta folder")));
    }
    for (const LuaMetaFile& file : files) {
        auto path = io::joinPath(directory, file.name);
        if (!path) {
            return std::unexpected(std::move(path.error()));
        }
        if (auto existing = fs.readFile(*path); existing && *existing == file.content) {
            continue; // Unchanged: LuaLS's watcher sees no change.
        }
        if (auto parent = std::string(io::parentPath(*path)); !parent.empty()) {
            if (auto made = fs.createDirectories(parent); !made) {
                return std::unexpected(std::move(made.error()));
            }
        }
        if (auto written = fs.writeFileAtomic(*path, file.content); !written) {
            return std::unexpected(std::move(written.error().withContext("writing a Lua meta file")));
        }
    }
    std::vector<std::string> present;
    listLuaFiles(fs, std::string(directory), {}, present);
    for (const std::string& name : present) {
        if (wanted.contains(name)) {
            continue;
        }
        if (auto path = io::joinPath(directory, name)) {
            if (auto removed = fs.removeFile(*path); !removed) {
                return std::unexpected(std::move(removed.error().withContext("removing a stale Lua meta file")));
            }
        }
    }
    return {};
}

} // namespace frost::lang
