// SPDX-License-Identifier: Apache-2.0
//
// lldb-dap, the debug adapter FrostIDE starts (PLAN.md M4, ADR 0050): finding the pinned executable,
// the process spec it runs with, the launch request's arguments for a program, and the trust gate
// that every debug session goes through.
/*
 * Finding it. The settings key tools.lldbDap.path (absolute) wins; otherwise the SDK's pinned copy,
 * <sdk>/lldb-<version>/bin/lldb-dap(.exe), under $FROST_SDK, E:\sdk (tools/bootstrap.ps1) and
 * $HOME/sdk (tools/bootstrap-wsl.sh), in that order. PATH is never searched: a debugger runs the
 * user's program with full rights, so it must be the one FrostIDE ships or one the user named.
 *
 * Environment. lldb-dap starts with FrostIDE's environment minus _NT_SYMBOL_PATH,
 * _NT_ALT_SYMBOL_PATH and DEBUGINFOD_URLS, so a machine-wide symbol server setting never makes a
 * launch download symbols for every system DLL (minutes, and network traffic the user did not ask
 * for). On Windows NoDefaultCurrentDirectoryInExePath goes too: the debuggee inherits lldb-dap's
 * environment, and a user program gets the Run behaviour (ADR 0011). M6 adds a symbol server
 * setting that puts an explicit, cached path back.
 *
 * Trust. startDebugSession refuses (Untrusted) before any adapter starts when a folder the program
 * runs project code from is not trusted, exactly like Build and Run.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/debug/debug_session.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"
#include "frost/rpc/json.hpp"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::project {
class TrustStore;
}

namespace frost::debug {

// The lldb version pinned in sdk/sdk.lock.json (lldb-dap ships with the LLVM release).
[[nodiscard]] std::string_view pinnedLldbVersion() noexcept;

// Where findLldbDap looks (header comment).
struct LldbDapSearch {
    std::string configured{};            // tools.lldbDap.path; "" for none.
    std::vector<std::string> sdkRoots{}; // Each holds lldb-<version>/bin.
};

// The search for this process: `configured` plus the SDK roots of the header comment.
[[nodiscard]] LldbDapSearch defaultLldbDapSearch(std::string configured = {});

// <sdkRoot>/lldb-<pinned>/bin/lldb-dap(.exe). Errors: InvalidArgument (sdkRoot).
[[nodiscard]] Result<std::string> pinnedLldbDapPath(std::string_view sdkRoot);

/*
 * The lldb-dap to start. Errors: InvalidArgument (the configured path is not absolute), NotFound
 * (the configured file is missing, or no SDK root has the pinned one; the message names every
 * path tried and the setting).
 */
[[nodiscard]] Result<std::string> findLldbDap(io::IFileSystem& fs, const LldbDapSearch& search);

// Variables lldb-dap never inherits (header comment, "Environment").
inline constexpr std::array<std::string_view, 3> kStrippedAdapterEnvironment{"_NT_SYMBOL_PATH", "_NT_ALT_SYMBOL_PATH",
                                                                             "DEBUGINFOD_URLS"};

// How lldb-dap at `executable` (absolute) starts: --no-lldbinit (no ~/.lldbinit), stdio pipes,
// the environment of the header comment, contained (killOnClose) so Stop ends the debuggee too.
[[nodiscard]] io::ProcessSpec lldbDapProcessSpec(std::string executable);

// A mapping can only be created after validating its source prefix and trusted target root.
class SourceMap {
public:
    [[nodiscard]] const std::string& from() const noexcept { return m_from; }
    [[nodiscard]] const std::string& to() const noexcept { return m_to; }

private:
    SourceMap(std::string from, std::string to) : m_from(std::move(from)), m_to(std::move(to)) {}
    std::string m_from;
    std::string m_to;
    friend Result<SourceMap> makeSourceMap(io::IFileSystem&, const project::TrustStore&, std::string_view,
                                           std::string_view, const std::vector<std::string>&);
};

// The source is an absolute build-time prefix and need not exist locally; destination must be an
// existing directory and an explicitly trusted debug root. This does not verify source contents.
[[nodiscard]] Result<SourceMap> makeSourceMap(io::IFileSystem& fs, const project::TrustStore& trust,
                                              std::string_view from, std::string_view to,
                                              const std::vector<std::string>& debugRoots);

// The program to debug, as a Debug run resolved it.
struct DebugTarget {
    std::string name{};                   // For the Debug tool window ("App [Debug|x64]").
    std::string program{};                // Absolute.
    std::vector<std::string> arguments{}; // argv[1..] (build::splitWindowsArguments of the run's).
    std::string workingDirectory{};       // Absolute; "" for the program's folder.
    std::vector<std::pair<std::string, std::string>> environment{}; // Set over the inherited one.
    bool inheritEnvironment = true;                                 // false: the program sees `environment` only.
    std::vector<std::string> trustRoots{}; // Folders that must be trusted; empty: the program's folder.
    std::optional<SourceMap> sourceMap{};
};

// lldb-dap's launch request arguments for `target`: program, args, cwd, env ("NAME=VALUE"),
// stopOnEntry false, and initCommands for inheritEnvironment false.
[[nodiscard]] rpc::Json lldbDapLaunchArguments(const DebugTarget& target);

// Starts lldb-dap at `executable` through `launcher` for each session and connects a DAP client.
// The launcher must outlive the sessions.
[[nodiscard]] AdapterFactory lldbDapAdapterFactory(io::IProcessLauncher& launcher, std::string executable);

/*
 * The one way a debug session starts: checks that every folder of target.trustRoots (or the
 * program's folder) is trusted, then DebugSession::start with lldbDapLaunchArguments(target).
 * Errors: InvalidArgument (program not absolute), Untrusted (no adapter was started).
 */
[[nodiscard]] Result<std::unique_ptr<DebugSession>>
startDebugSession(const project::TrustStore& trust, const DebugTarget& target, AdapterFactory factory,
                  std::map<std::string, std::vector<BreakpointLine>> breakpoints, DebugSessionCallbacks callbacks);

// Starts an already-running target using an explicitly supplied adapter and attach arguments.
// The same trusted-root and absolute target checks apply; disconnect never terminates the host.
[[nodiscard]] Result<std::unique_ptr<DebugSession>>
attachDebugSession(const project::TrustStore& trust, const DebugTarget& target, AdapterFactory factory,
                   rpc::Json attachArguments, std::map<std::string, std::vector<BreakpointLine>> breakpoints,
                   DebugSessionCallbacks callbacks, std::string adapterId = "lldb-dap");

} // namespace frost::debug
