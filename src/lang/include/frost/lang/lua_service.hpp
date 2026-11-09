// SPDX-License-Identifier: Apache-2.0
// LuaLS discovery, configuration and supervised LSP document service.
/*
 * The workspace side of the protocol (LUA-3): LuaService announces multi-root workspace folders
 * (the root plus LuaServiceOptions::folders, each answered its own configuration by scopeUri),
 * sends workspace/didChangeConfiguration when changeConfiguration() replaces the configuration
 * (LuaLS then asks for it again through workspace/configuration, so the change takes effect without
 * a restart), forwards file changes to the watchers the server registered for
 * workspace/didChangeWatchedFiles (dynamic registration, LSP 3.17), passes $/progress and the
 * workspace refresh requests (semantic tokens, inlay hints, code lens, diagnostics) to its owner.
 * LuaLS 3.19.1 registers no file watcher (it watches the workspace itself), so for it
 * notifyFileChanges() sends nothing; another server that registers gets the changes.
 */
#pragma once

#include "frost/lang/clangd.hpp" // RestartPolicy and the shared backoff calculation.

#include <span>

namespace frost::lang {

struct LuaSearch {
    std::string configuredPath;           // Absolute user-selected executable, if any.
    std::vector<std::string> directories; // Absolute installation directories in priority order.
    std::string pathVariable;             // PATH; relative entries are ignored.
};

// The LuaLS version pinned in sdk/sdk.lock.json (ADR 0079), e.g. "3.19.1".
[[nodiscard]] std::string_view pinnedLuaLanguageServerVersion() noexcept;
// Search without launching, O(directories + PATH entries). Returns InvalidArgument for a relative
// configured path and NotFound if none exists; a bad configured path never silently falls back.
[[nodiscard]] Result<std::string> findLuaLanguageServer(io::IFileSystem& fs, const LuaSearch& search);
/*
 * The default search, in priority order: the LuaLS bundled beside this executable
 * (<exe dir>/luals-<pinned>/bin from tools/stage.ps1 and tools/stage.sh, then the older
 * lua-language-server/bin and the executable's own directory), the pinned SDK install
 * <$FROST_SDK>/luals-<pinned>/bin when FROST_SDK is set (tools/env.ps1 and ~/sdk/env.sh set it for
 * development and tests; there is no machine-specific default root, FND-10), then this process's PATH.
 * Reads the environment once; thread-safe.
 */
[[nodiscard]] LuaSearch defaultLuaSearch(std::string configuredPath = {});
/*
 * LuaLS's launch arguments that keep its log and its generated meta files under `stateDirectory`
 * (a per-user directory) instead of the installation directory, which may be read-only:
 * --logpath=<dir>/log and --metapath=<dir>/meta. The directories need not exist; LuaLS creates
 * them. Errors: InvalidArgument (relative directory).
 */
[[nodiscard]] Result<std::vector<std::string>> luaLanguageServerArguments(std::string_view stateDirectory);
// The client's own defaults for LuaLS (the "Lua" section; .luarc.json has priority in LuaLS, key
// by key): runtime.version and workspace.checkThirdParty false. Host providers (lua_host.hpp,
// LUA-13) add their libraries, globals and paths on top through composeLuaConfiguration.
[[nodiscard]] Json luaDefaults(std::string runtimeVersion = "Lua 5.4");
// Returns a copy of the requested Lua section, or null for an unknown section, O(size).
[[nodiscard]] Json luaConfigurationSection(const Json& configuration, std::string_view section);

// One more LuaLS workspace folder beside the root (multi-root).
struct LuaWorkspaceFolder {
    std::string path;   // Absolute.
    Json configuration; // Its "Lua" section, answered to workspace/configuration with its scopeUri.
    friend bool operator==(const LuaWorkspaceFolder&, const LuaWorkspaceFolder&) = default;
};

// A $/progress notification of LuaLS (the LSP work-done progress value, as clangd's).
using LuaProgress = ClangdProgress;
// One file change for workspace/didChangeWatchedFiles: an absolute path and its LSP change type.
struct LuaFileChange {
    std::string path;
    FileChangeType type = FileChangeType::Changed;
};

enum class LuaState : std::uint8_t { Stopped, Starting, Running, Restarting, Failed };
struct LuaServiceOptions {
    std::string serverPath;    // Absolute executable, discovered by findLuaLanguageServer.
    std::string workspaceRoot; // Absolute.
    /*
     * More workspace folders (absolute, distinct from the root), announced before the root:
     * LuaLS assigns a file to the first folder that contains it, so a folder nested in the root
     * (a FrostEngine project inside an engine checkout) keeps its own files, runtime.path and
     * .luarc.json. Changing the set of folders needs a restart; their configurations can change live.
     */
    std::vector<LuaWorkspaceFolder> folders;
    // Defaults preserve LuaLS; another supervised LSP may supply its own argv and language id.
    std::vector<std::string> arguments;
    std::vector<std::pair<std::string, std::optional<std::string>>> environment;
    std::string languageId = "lua";
    std::string displayName = "LuaLS";
    bool firstArgumentIsAbsolutePath = false;
    bool trusted = false; // Must be true before any process is launched.
    Json configuration = luaDefaults();
    RestartPolicy restart;
    IClock::Duration initializeTimeout = std::chrono::seconds(60);
    IClock::Duration shutdownTimeout = std::chrono::seconds(5);
    std::size_t historyPerDocument = 32;
    // Called on a worker after a server becomes ready or its connection ends.
    std::function<void()> onLifecycle;
    // Called on the reader thread for each decoded $/progress of the current server (marshal to
    // the GUI yourself); must be quick and stay callable until stop() finished. Null: ignored.
    std::function<void(const LuaProgress&)> onProgress;
    // Called on the reader thread after the current server's workspace refresh request (e.g.
    // "workspace/semanticTokens/refresh") was answered; the owner re-requests what it shows.
    std::function<void(std::string_view method)> onRefresh;
};

// Thread-safe; launcher, reactor and scheduler outlive stop(). Call stop before shutting them down.
// start and document notifications return promptly; supervision and callbacks run on scheduler workers.
class LuaService final : public ILspRequester {
public:
    // The launcher and context must outlive the service and the completion of stop().
    LuaService(io::IProcessLauncher& launcher, io::AsyncContext context, LuaServiceOptions options);
    // Requests cancellation and kills the child without blocking; await stop() for clean shutdown.
    ~LuaService() override;
    LuaService(const LuaService&) = delete;
    LuaService& operator=(const LuaService&) = delete;
    // Returns immediately. Errors: Untrusted, InvalidArgument (relative path), Busy (already started).
    [[nodiscard]] Result<void> start();
    // Await before destroying this service or its context; bounded shutdown then process-tree kill.
    [[nodiscard]] sched::Task<void> stop();
    // Snapshot accessors, each O(1) except recentStderr() which copies up to 64 lines.
    [[nodiscard]] LuaState state() const;
    [[nodiscard]] unsigned restarts() const;
    [[nodiscard]] std::optional<io::ProcessId> serverPid() const;
    [[nodiscard]] std::optional<ServerCapabilities> capabilities() const;
    [[nodiscard]] std::optional<Error> lastError() const;
    [[nodiscard]] std::vector<std::string> recentStderr() const;
    // The store belongs to this service. Listeners run on the reader worker and must be quick.
    [[nodiscard]] DiagnosticsStore& diagnostics() noexcept;
    // Current LSP version for an open document, nullopt after close; O(log open documents).
    [[nodiscard]] std::optional<std::int64_t> documentVersion(std::string_view path) const;
    // Open an absolute file path. Errors: InvalidArgument, AlreadyExists.
    [[nodiscard]] Result<void> openDocument(std::string_view path, text::Snapshot snapshot);
    // Send incremental edits when supported and applicable, otherwise full text. Errors: NotFound.
    [[nodiscard]] Result<void> changeDocument(std::string_view path, const text::ChangeSet& changes,
                                              text::Snapshot after);
    // Save and close notifications return immediately; closing clears diagnostics. Errors: NotFound.
    [[nodiscard]] Result<void> saveDocument(std::string_view path);
    [[nodiscard]] Result<void> closeDocument(std::string_view path);
    // Captures the current connection immediately; the task survives service destruction and cannot
    // migrate to a replacement server. Errors: Busy or the underlying request error.
    [[nodiscard]] sched::Task<Result<Json>> request(std::string method, Json params,
                                                    std::stop_token token = {}) override;
    [[nodiscard]] PositionEncoding encoding() const override;
    [[nodiscard]] std::optional<ServerCapabilities> serverCapabilities() const override { return capabilities(); }
    void setApplyEditHandler(ApplyEditHandler handler) override;

    // The root's configuration in effect: the options' at first, then the last changeConfiguration's.
    [[nodiscard]] Json configuration() const;
    [[nodiscard]] std::vector<LuaWorkspaceFolder> folders() const;
    /*
     * Replaces the root's configuration and the folders' (matched by path; the folder paths must
     * be the ones the service started with) and sends workspace/didChangeConfiguration to a running
     * server, which then reads the new values through workspace/configuration. A server starting
     * later gets the new values at once. Errors: InvalidArgument (another set of folder paths:
     * restart the service instead). O(configuration size).
     */
    [[nodiscard]] Result<void> changeConfiguration(Json configuration, std::vector<LuaWorkspaceFolder> folders);
    // How many didChangeConfiguration notifications were sent to a server so far.
    [[nodiscard]] std::uint64_t configurationChangesSent() const;
    /*
     * Forwards the changes the server's registered watchers cover as one
     * workspace/didChangeWatchedFiles notification and returns how many it carried (0 when none is
     * covered or no server runs). Errors: those of the notification. O(changes * watchers).
     */
    [[nodiscard]] Result<std::size_t> notifyFileChanges(std::span<const LuaFileChange> changes);
    // The running server's registered workspace/didChangeWatchedFiles watchers (none when stopped).
    [[nodiscard]] std::vector<FileSystemWatcher> fileWatchers() const;

    // Forces a supervised crash. Errors: NotFound when no child exists.
    [[nodiscard]] Result<void> killServerForTesting();
    struct Shared;

private:
    std::shared_ptr<Shared> m_shared;
    sched::JoinHandle<void> m_supervisor;
};
} // namespace frost::lang
