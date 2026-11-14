// SPDX-License-Identifier: Apache-2.0
//
// lldb-dap, the debug adapter FrostIDE starts (PLAN.md M4, ADR 0050): finding the pinned executable,
// the process spec it runs with, the launch request's arguments for a program, and the trust gate
// that every debug session goes through.
/*
 * Finding it. The settings key tools.lldbDap.path (absolute) wins; otherwise the pinned copy,
 * <root>/lldb-<version>/bin/lldb-dap(.exe), under the directory of this executable (the copy that
 * tools/stage.ps1 and tools/stage.sh bundle), then under $FROST_SDK when it is set (tools/env.ps1
 * and ~/sdk/env.sh set it for development and tests), in that order. There is no machine-specific
 * default root (FND-10). PATH is never searched: a debugger runs the user's program with full
 * rights, so it must be the one FrostIDE ships or one the user named.
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
 *
 * Values on Linux. lldb's libstdc++ formatters for vector, list, forward_list, deque, map, set, the
 * unordered containers and variant are Python classes (lldb.formatters.cpp.gnu_libstdcpp; lldb's
 * CPlusPlusLanguage.cpp), and the pinned Linux lldb-dap is built without Python (tools/build-lldb.sh).
 * lldb then reports a six-element std::vector as "size=0" with no children (checked against lldb-dap
 * 23.1.4 and libstdc++ 14): a wrong value, not a missing one. On Linux the launch therefore adds
 * lldbDapLibstdcxxCommands: a category above lldb's own that gives those types a summary saying
 * their contents cannot be shown, so FrostIDE never shows lldb's wrong size. Members, std::string,
 * smart pointers and Evaluate (numbers[5]) still work. MSVC STL formatters are C++ and unaffected:
 * they are on by default and show vector, string, map, unique_ptr, optional and more on cl.exe
 * builds (DBG-4a; msvc_debug_test.cpp lists the gaps).
 * TODO(M6): (DBG-4a, R3) remove once the Linux lldb-dap has Python or C++ libstdc++ providers.
 *
 * Stepping on Windows. A Visual Studio Debug build (/ZI, Edit and Continue) calls functions through
 * incremental-link thunks that lldb can step out of, so lldb's own step-in, which steps out of any
 * function without debug information (target.process.thread.step-in-avoid-nodebug), returns from the
 * thunk and Step Into acts as Step Over. The Windows launch therefore sets lldbDapWindowsStepCommand
 * (avoid-nodebug off), and DebugSession steps out of such functions itself after following a thunk
 * (DebugSessionOptions::stepOutOfNoDebug; debug_session.hpp, "Step into a thunk").
 *
 * Attach on Linux. Under the Yama policy ptrace_scope 1 (Ubuntu's default) lldb may attach only to its
 * descendants, and lldb-dap reports only "Trying to resume but the async thread is dead". An attach
 * failure therefore carries ptraceAttachHint: why, and the three ways out.
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
    bool stopOnEntry = false; // Stop before the program's first instruction runs (reason "entry").
};

// Shared trust admission for native debugger backends; checks before starting any adapter.
[[nodiscard]] Result<void> validateDebugTargetTrust(const project::TrustStore& trust, const DebugTarget& target);

// The lldb commands of the header comment ("Values on Linux"), for a libstdc++ program. Pure.
[[nodiscard]] std::vector<std::string> lldbDapLibstdcxxCommands();
// The lldb command of the header comment ("Stepping on Windows").
inline constexpr std::string_view lldbDapWindowsStepCommand =
    "settings set target.process.thread.step-in-avoid-nodebug false";

/*
 * lldb-dap's launch request arguments for `target`: program, args, cwd, env ("NAME=VALUE"),
 * stopOnEntry, and initCommands: "settings set target.inherit-env false" for inheritEnvironment
 * false, when `libstdcxx` (default: on Linux) lldbDapLibstdcxxCommands, and when `windowsStepping`
 * (default: on Windows; the two-argument form leaves it out) lldbDapWindowsStepCommand.
 */
[[nodiscard]] rpc::Json lldbDapLaunchArguments(const DebugTarget& target);
[[nodiscard]] rpc::Json lldbDapLaunchArguments(const DebugTarget& target, bool libstdcxx);
[[nodiscard]] rpc::Json lldbDapLaunchArguments(const DebugTarget& target, bool libstdcxx, bool windowsStepping);

/*
 * The hint an attach failure carries on Linux (header comment), for the content of
 * /proc/sys/kernel/yama/ptrace_scope: "" for 0 or an unreadable value, else why lldb was refused
 * and the fixes (the program calls prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY), `sudo sysctl
 * kernel.yama.ptrace_scope=0`, or a debugger with CAP_SYS_PTRACE; scope 3 allows none). Pure.
 */
[[nodiscard]] std::string ptraceAttachHint(std::string_view ptraceScope);
// The same for this machine: reads the file through `fs`; "" on Windows or when it cannot be read.
[[nodiscard]] std::string ptraceAttachHint(io::IFileSystem& fs);

// The session options lldb-dap needs here: on Windows breakOnAbort (launch only), msvcExceptions
// (debug_session.hpp, "Exception filters") and stepOutOfNoDebug (header comment, "Stepping on
// Windows"), on Linux an attach's ptraceAttachHint from the real file system.
[[nodiscard]] DebugSessionOptions lldbDapSessionOptions(DebugTargetMode mode);

// Starts lldb-dap at `executable` through `launcher` for each session and connects a DAP client.
// The launcher must outlive the sessions.
[[nodiscard]] AdapterFactory lldbDapAdapterFactory(io::IProcessLauncher& launcher, std::string executable,
                                                   DapProcessOutput output = {});
struct NativeDebugPrivacy {
    std::function<Result<void>()> launchAdmission;
};

/*
 * The one way a debug session starts: checks that every folder of target.trustRoots (or the
 * program's folder) is trusted, then DebugSession::start with lldbDapLaunchArguments(target).
 * Errors: InvalidArgument (program not absolute), Untrusted (no adapter was started).
 */
[[nodiscard]] Result<std::unique_ptr<DebugSession>>
startDebugSession(const project::TrustStore& trust, const DebugTarget& target, AdapterFactory factory,
                  std::map<std::string, std::vector<BreakpointLine>> breakpoints, DebugSessionCallbacks callbacks);
// The same with function breakpoints, the exception filters to stop on (DBG-2a) and the thrown types
// cpp_throw stops for (DBG-2b; DebugSessionOptions::throwTypes).
[[nodiscard]] Result<std::unique_ptr<DebugSession>>
startDebugSession(const project::TrustStore& trust, const DebugTarget& target, AdapterFactory factory,
                  std::map<std::string, std::vector<BreakpointLine>> breakpoints, DebugSessionCallbacks callbacks,
                  std::vector<FunctionBreakpointLine> functionBreakpoints, std::vector<std::string> exceptionFilters,
                  std::vector<std::string> throwTypes = {}, std::optional<NativeDebugPrivacy> privacy = {});

// Starts an already-running target using an explicitly supplied adapter and attach arguments.
// The same trusted-root and absolute target checks apply; disconnect never terminates the host.
[[nodiscard]] Result<std::unique_ptr<DebugSession>>
attachDebugSession(const project::TrustStore& trust, const DebugTarget& target, AdapterFactory factory,
                   rpc::Json attachArguments, std::map<std::string, std::vector<BreakpointLine>> breakpoints,
                   DebugSessionCallbacks callbacks, std::string adapterId = "lldb-dap");

} // namespace frost::debug
