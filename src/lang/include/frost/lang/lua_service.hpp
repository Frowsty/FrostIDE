// SPDX-License-Identifier: Apache-2.0
// LuaLS discovery, configuration and supervised LSP document service.
#pragma once

#include "frost/lang/clangd.hpp" // RestartPolicy and the shared backoff calculation.

namespace frost::lang {

struct LuaSearch {
    std::string configuredPath;           // Absolute user-selected executable, if any.
    std::vector<std::string> directories; // Absolute installation directories in priority order.
    std::string pathVariable;             // PATH; relative entries are ignored.
};

// Search without launching, O(directories + PATH entries). Returns InvalidArgument for a relative
// configured path and NotFound if none exists; a bad configured path never silently falls back.
[[nodiscard]] Result<std::string> findLuaLanguageServer(io::IFileSystem& fs, const LuaSearch& search);
// Reads this process's PATH; caller may prepend a trusted installation directory.
[[nodiscard]] LuaSearch defaultLuaSearch(std::string configuredPath = {});

// Client defaults; .luarc.json has priority in LuaLS. FrostEngine generates and owns its API files.
[[nodiscard]] Json luaDefaults(std::string runtimeVersion = "Lua 5.4", bool frostEngine = false);
// Returns a copy of the requested Lua section, or null for an unknown section, O(size).
[[nodiscard]] Json luaConfigurationSection(const Json& configuration, std::string_view section);

enum class LuaState : std::uint8_t { Stopped, Starting, Running, Restarting, Failed };
struct LuaServiceOptions {
    std::string serverPath;    // Absolute executable, discovered by findLuaLanguageServer.
    std::string workspaceRoot; // Absolute.
    bool trusted = false;      // Must be true before any process is launched.
    Json configuration = luaDefaults();
    RestartPolicy restart;
    IClock::Duration initializeTimeout = std::chrono::seconds(60);
    IClock::Duration shutdownTimeout = std::chrono::seconds(5);
    std::size_t historyPerDocument = 32;
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
    // Forces a supervised crash. Errors: NotFound when no child exists.
    [[nodiscard]] Result<void> killServerForTesting();
    struct Shared;

private:
    std::shared_ptr<Shared> m_shared;
    sched::JoinHandle<void> m_supervisor;
};
} // namespace frost::lang
