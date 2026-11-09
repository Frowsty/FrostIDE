// SPDX-License-Identifier: Apache-2.0
//
// Lua host extension points (LUA-13, PLAN.md Appendix C R4): the Qt-free API through which a host
// that embeds Lua (FrostEngine first; later others) tells LuaLS about its scripting environment
// without the Lua module knowing the host: the runtime version and require paths it uses, the
// builtin libraries it removes, its definition libraries, its globals and generated meta files.
/*
 * Model. A provider (ILuaHostProvider) looks at one workspace folder and returns what it adds
 * there (LuaHostContribution), or nothing when the folder is not its own. The composition root
 * keeps the providers in a LuaHostRegistry; when LuaLS starts or its folders' files change, the Lua
 * integration asks the registry, writes each contribution's meta files into a FrostIDE-owned
 * folder (writeLuaMetaFiles, below the per-user LuaLS state directory, never the workspace), and
 * composes the client configuration with composeLuaConfiguration. A project's .luarc.json still
 * has priority in LuaLS, key by key.
 *
 * Truth rule (ADR 0078): a provider contributes only what its host's own files declare (generated
 * references, definition folders, project files); when it cannot read them it says so in `note`
 * and leaves the values out, so LuaLS reports what it reports.
 *
 * Thread safety: LuaHostRegistry is thread-safe; providers must be thread-safe as the file system
 * they are given is; the free functions are pure apart from `fs`.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/file_system.hpp"
#include "frost/lang/json.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::lang {

// A definition file a host generates for LuaLS (a "---@meta" file with its API's annotations).
struct LuaMetaFile {
    std::string name;    // Relative path below the meta folder ('/'-separated, no "..", ends in ".lua").
    std::string content; // The file's text.
    friend bool operator==(const LuaMetaFile&, const LuaMetaFile&) = default;
};

// What one host adds for one workspace folder. Empty fields add nothing.
struct LuaHostContribution {
    std::string provider;                 // The provider's name (ILuaHostProvider::name), for logs.
    std::string runtimeVersion;           // runtime.version the host runs ("Lua 5.4"); "" keeps the user's setting.
    std::vector<std::string> runtimePath; // runtime.path entries, e.g. "Assets/Scripts/?.lua".
    // runtime.builtin entries: a standard library name and "disable", "enable" or "default".
    std::vector<std::pair<std::string, std::string>> builtins;
    // workspace.library entries: folders or files, relative to the workspace folder or absolute.
    std::vector<std::string> library;
    std::vector<std::string> globals;   // diagnostics.globals: names the host defines.
    std::vector<LuaMetaFile> metaFiles; // Written by FrostIDE (writeLuaMetaFiles) and added to the library.
    /*
     * More workspace folders (absolute) the host's projects need as their own LuaLS folders, e.g.
     * the projects inside an engine checkout, whose runtime.path is relative to the project. The
     * registry asks the providers again for each of them.
     */
    std::vector<std::string> folders;
    // Absolute files or folders the contribution was read from (or would be, once they exist): a
    // change at or below one makes the Lua integration ask the providers again.
    std::vector<std::string> inputs;
    std::string note; // English, for the log: what the provider found, or why it adds less.
    friend bool operator==(const LuaHostContribution&, const LuaHostContribution&) = default;
};

class ILuaHostProvider {
public:
    virtual ~ILuaHostProvider() = default;
    ILuaHostProvider(const ILuaHostProvider&) = delete ("providers have identity");
    ILuaHostProvider& operator=(const ILuaHostProvider&) = delete ("providers have identity");
    // A short, stable name ("FrostEngine"); one provider per name in a registry.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    /*
     * What the host adds for the workspace folder `folder` (absolute, normalised), or nullopt when
     * the folder is not the host's. May read files through `fs`; must not start processes. Called
     * on the GUI thread or a worker.
     */
    [[nodiscard]] virtual std::optional<LuaHostContribution> contribute(io::IFileSystem& fs,
                                                                        std::string_view folder) = 0;

protected:
    ILuaHostProvider() = default;
};

class LuaHostRegistry {
public:
    // Adds `provider`, replacing one with the same name. Null is ignored.
    void add(std::shared_ptr<ILuaHostProvider> provider);
    // Removes the provider named `name`; false when none has that name.
    bool remove(std::string_view name);
    [[nodiscard]] std::vector<std::string> names() const;
    // Every provider's contribution for `folder`, in registration order. O(providers) calls.
    [[nodiscard]] std::vector<LuaHostContribution> contributions(io::IFileSystem& fs, std::string_view folder) const;

private:
    mutable Mutex m_mutex;
    std::vector<std::shared_ptr<ILuaHostProvider>> m_providers FROST_GUARDED_BY(m_mutex);
};

/*
 * The client "Lua" section for one workspace folder: luaDefaults(userRuntimeVersion) plus the
 * contributions in order. The first non-empty runtimeVersion replaces the user's; lists are merged
 * in order without duplicates; a later builtin entry for the same library wins. `metaLibraries`
 * (absolute folders writeLuaMetaFiles wrote) are appended to workspace.library. Keys a
 * contribution leaves empty are not written. O(total entries^2) for the duplicate checks (small).
 */
[[nodiscard]] Json composeLuaConfiguration(std::string userRuntimeVersion,
                                           std::span<const LuaHostContribution> contributions,
                                           std::span<const std::string> metaLibraries = {});

/*
 * The FrostIDE-owned folder for `provider`'s meta files of workspace folder `folder`:
 * <stateDirectory>/host-meta/<provider>-<16 hex digits of a stable hash of the folder's key>.
 * Errors: InvalidArgument (a relative state directory or an empty provider name).
 */
[[nodiscard]] Result<std::string> luaHostMetaDirectory(std::string_view stateDirectory, std::string_view provider,
                                                       std::string_view folder);

/*
 * Makes `directory` (absolute) hold exactly `files`: creates it, writes each file atomically
 * (unchanged files are not rewritten) and removes the .lua files below it that `files` no longer
 * lists. Errors: InvalidArgument (a relative directory, or a name that is absolute, holds "..",
 * '\' or does not end in ".lua"), and the file system's. O(files + existing files).
 */
[[nodiscard]] Result<void> writeLuaMetaFiles(io::IFileSystem& fs, std::string_view directory,
                                             std::span<const LuaMetaFile> files);

} // namespace frost::lang
