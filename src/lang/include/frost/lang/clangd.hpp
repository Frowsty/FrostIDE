// SPDX-License-Identifier: Apache-2.0
//
// clangd for C++ intelligence (PLAN.md section 2 "Semantic engine"; W2, ADR 0036): finding the
// executable, its command line, and ClangdService, which runs clangd as a supervised child
// (restart with backoff after a crash, clean shutdown with a time limit, stderr to frost::log),
// keeps the open documents in sync and collects the live diagnostics.
/*
 * Finding clangd (findClangd): the tools.clangd.path setting when it is set (it must then name
 * an existing absolute file; there is no fallback, so a typo is reported, not papered over); else
 * the pinned LLVM (sdk/sdk.lock.json): $FROST_LLVM_ROOT/bin, then <FROST_SDK or the bootstrap's
 * default root>/llvm-<pinned version>/bin (E:\sdk on Windows, ~/sdk on Linux); else the
 * directories of PATH, searched here (never by the OS) so the result is an absolute path that
 * IProcessLauncher can start. TODO(M5): the clangd bundled with the installer comes first.
 *
 * Command line (clangdArguments): --background-index, --header-insertion=never, --log=error,
 * --pch-storage=memory, -j=<cores / 2, at most 8> (ADR 0008 decision 4: a first index build of R3
 * at -j=4 needed about 8.1 GiB of commit; TODO(M3): the ResourceGovernor sizes -j by free commit),
 * and --compile-commands-dir=<dir> when a provider found a database. stdout carries LSP, stdin
 * takes it, stderr is clangd's log.
 *
 * Supervision. start() spawns the supervisor on the Background lane. Each round asks the
 * compile-commands provider, launches clangd through IProcessLauncher::launchAsync (contained:
 * Job Object or process group, so nothing outlives FrostIDE), runs the initialize handshake within
 * initializeTimeout, re-opens every open document, and waits for the process to exit. An exit
 * that stop() did not ask for is a crash: the next round starts after restartDelay() (doubling from
 * initialDelay up to maxDelay); a server that ran for stableAfter resets the count; more than
 * maxConsecutiveCrashes in a row ends in ClangdState::Failed. A failed launch (no such file)
 * fails at once. Diagnostics survive a restart until clangd republishes them.
 *
 * Documents. openDocument/changeDocument/saveDocument/closeDocument record the document and,
 * while clangd runs, post the matching notification at once (they never wait on clangd; PLAN.md
 * 3.4 "Keystroke -> paint"). LSP versions start at 1 per document and increase by one per change,
 * across restarts. Each document keeps the snapshots of its last historyPerDocument versions, so
 * a publish for an older version maps to the text clangd saw (and is marked stale).
 *
 * Inactive regions (ADR 0047). clangd's textDocument/inactiveRegions notifications are mapped to
 * the snapshot of the version clangd last published diagnostics for (both come from the same AST
 * build; the notification names no version), kept per open document and passed to the inactive
 * regions listeners, on the reader thread like the diagnostics. Closing a document clears them.
 *
 * Threads. Every member is thread-safe. Diagnostics are published from the connection's reader
 * (a scheduler worker). Lifetime: call stop() and wait for it before shutting down the reactor
 * and the scheduler; the destructor only requests the stop and kills the process (never blocks).
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/async_process.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/diagnostics.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/semantic_tokens.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"
#include "frost/text/document.hpp"
#include "frost/text/snapshot.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::settings {
class SettingsSnapshot;
} // namespace frost::settings

namespace frost::lang {

// --- Finding clangd ------------------------------------------------------------------------------

enum class ClangdOrigin : std::uint8_t { Setting, PinnedSdk, Path };

[[nodiscard]] std::string_view toString(ClangdOrigin origin) noexcept;

struct ClangdLocation {
    std::string path; // Absolute, normalised.
    ClangdOrigin origin = ClangdOrigin::Path;
};

// The inputs of findClangd, separated from the process environment so tests can supply them.
struct ClangdSearch {
    std::string configuredPath;              // tools.clangd.path; "" when unset.
    std::vector<std::string> sdkDirectories; // Pinned-LLVM bin directories, in order.
    std::string pathVariable;                // The PATH value.
};

// The LLVM version pinned in sdk/sdk.lock.json at build time ("23.1.4").
[[nodiscard]] std::string_view pinnedLlvmVersion() noexcept;

// tools.clangd.path from a settings snapshot.
[[nodiscard]] std::string configuredClangdPath(const settings::SettingsSnapshot& settings);

// The search this process would run: `configuredPath`, the SDK directories from FROST_LLVM_ROOT,
// FROST_SDK and the bootstrap default, and PATH (see the header comment).
[[nodiscard]] ClangdSearch defaultClangdSearch(std::string configuredPath = {});

// Errors: NotFound (the configured path is missing, or no candidate exists; the message lists
// what was tried), InvalidArgument (the configured path is not absolute).
[[nodiscard]] Result<ClangdLocation> findClangd(io::IFileSystem& fs, const ClangdSearch& search);

// --- Command line --------------------------------------------------------------------------------

struct ClangdLaunchOptions {
    bool backgroundIndex = true;
    unsigned jobs = 0; // 0: defaultClangdJobs().
    std::string logLevel = "error";
    std::vector<std::string> extraArguments; // Appended last.
};

// max(1, min(hardwareThreads / 2, 8)); see the header comment.
[[nodiscard]] unsigned defaultClangdJobs(unsigned hardwareThreads) noexcept;
[[nodiscard]] unsigned defaultClangdJobs() noexcept;

[[nodiscard]] std::vector<std::string> clangdArguments(const ClangdLaunchOptions& options,
                                                       const std::optional<CompileCommandsLocation>& compileCommands);

// --- Supervision ---------------------------------------------------------------------------------

struct RestartPolicy {
    IClock::Duration initialDelay = std::chrono::milliseconds(500);
    IClock::Duration maxDelay = std::chrono::seconds(30);
    unsigned maxConsecutiveCrashes = 5;
    IClock::Duration stableAfter = std::chrono::seconds(60);
};

// The wait before restarting after the `crash`-th consecutive crash (1-based):
// min(initialDelay * 2^(crash - 1), maxDelay). Pure.
[[nodiscard]] IClock::Duration restartDelay(const RestartPolicy& policy, unsigned crash) noexcept;

enum class ClangdState : std::uint8_t {
    Stopped,    // Not started, or stopped.
    Starting,   // Launching or in the initialize handshake.
    Running,    // Initialized; documents are synchronised.
    Restarting, // Crashed; waiting for the backoff to pass.
    Failed,     // Could not launch, or crashed too often (lastError() says why).
};

[[nodiscard]] std::string_view toString(ClangdState state) noexcept;

struct ClangdServiceOptions {
    std::string clangdPath;    // Absolute (findClangd).
    std::string workspaceRoot; // Absolute: the solution or CMake root (working directory and rootUri).
    ClangdLaunchOptions launch;
    std::shared_ptr<ICompileCommandsProvider> compileCommands; // May be null: clangd's fallback flags.
    RestartPolicy restart;
    IClock::Duration initializeTimeout = std::chrono::seconds(60);
    IClock::Duration shutdownTimeout = std::chrono::seconds(5);
    Json initializationOptions;          // Passed through (clangd: fallbackFlags, clangdFileStatus).
    std::size_t historyPerDocument = 32; // Snapshots kept per document for mapping older publishes.
};

class ClangdService final : public ILspRequester {
public:
    // `launcher`, the context's reactor and scheduler must outlive the service and its stop().
    ClangdService(io::IProcessLauncher& launcher, io::AsyncContext context, ClangdServiceOptions options);
    // Requests the stop and kills clangd without waiting (see the header comment).
    ~ClangdService() override;
    ClangdService(const ClangdService&) = delete ("owns a supervised process");
    ClangdService& operator=(const ClangdService&) = delete ("owns a supervised process");

    // Starts supervising (returns at once). Errors: InvalidArgument (paths not absolute), Busy
    // (already started).
    [[nodiscard]] Result<void> start();

    // The clean shutdown: stops the supervisor, sends shutdown and exit within shutdownTimeout,
    // waits for clangd to exit within the same limit, and kills it otherwise. Idempotent.
    [[nodiscard]] sched::Task<void> stop();

    [[nodiscard]] ClangdState state() const;
    // Restarts after crashes so far.
    [[nodiscard]] unsigned restarts() const;
    [[nodiscard]] std::optional<io::ProcessId> serverPid() const;
    // The negotiated capabilities while Running.
    [[nodiscard]] std::optional<ServerCapabilities> capabilities() const;
    // The database the current (or last) clangd was started with.
    [[nodiscard]] std::optional<CompileCommandsLocation> compileCommands() const;
    // Why the last round failed (launch, initialize or crash), if one did.
    [[nodiscard]] std::optional<Error> lastError() const;
    // The last stderr lines (at most 64), for crash reports and Collect Logs.
    [[nodiscard]] std::vector<std::string> recentStderr() const;

    [[nodiscard]] DiagnosticsStore& diagnostics() noexcept;

    // The last inactive regions clangd sent for `path` (an open document), if any.
    [[nodiscard]] std::optional<InactiveRegions> inactiveRegions(std::string_view path) const;
    // Called with each new set (an empty one when the document closes), on the publishing thread
    // (a scheduler worker), outside the service's lock; must be quick. Returns an id for
    // removeInactiveRegionsListener. Listeners are called in id order.
    using InactiveRegionsListener = std::function<void(const InactiveRegions&)>;
    std::uint64_t addInactiveRegionsListener(InactiveRegionsListener listener);
    void removeInactiveRegionsListener(std::uint64_t id);

    // Opens `path` (absolute) with its current text. Errors: InvalidArgument (path), AlreadyExists.
    [[nodiscard]] Result<void> openDocument(std::string_view path, text::Snapshot snapshot);
    // Applies `changes`, which took the document from its last snapshot to `after`. A ChangeSet
    // that does not start at the recorded version is sent as a full-text change. Errors: NotFound.
    [[nodiscard]] Result<void> changeDocument(std::string_view path, const text::ChangeSet& changes,
                                              text::Snapshot after);
    // Errors: NotFound.
    [[nodiscard]] Result<void> saveDocument(std::string_view path);
    // Closes the document and drops its diagnostics. Errors: NotFound.
    [[nodiscard]] Result<void> closeDocument(std::string_view path);

    // A request to the running clangd (completion, definition, ... for the next slice). Errors:
    // Busy (not Running), and those of LspClient::request.
    [[nodiscard]] sched::Task<Result<Json>> request(std::string method, Json params,
                                                    std::stop_token token = {}) override;
    // ILspRequester: the negotiated encoding while Running, UTF-16 otherwise.
    [[nodiscard]] PositionEncoding encoding() const override;
    // ILspRequester: capabilities().
    [[nodiscard]] std::optional<ServerCapabilities> serverCapabilities() const override { return capabilities(); }

    // Kills the current clangd as a crash would (tests of the restart path). Errors: NotFound.
    [[nodiscard]] Result<void> killServerForTesting();

    struct Shared; // src/lang/src/clangd_service.cpp.

private:
    std::shared_ptr<Shared> m_shared;
    sched::JoinHandle<void> m_supervisor;
};

} // namespace frost::lang
