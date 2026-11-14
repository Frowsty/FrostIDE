// SPDX-License-Identifier: Apache-2.0
//
// DebugController: the GUI-thread side of U1's debugger (PLAN.md M4, ADR 0050). It turns a Debug run
// (RunService::debug: Save All, build, resolve) into a frost::debug::DebugSession under lldb-dap,
// relays the session's events to the GUI thread for the Debug tool window, keeps the editors'
// breakpoint gutters and execution point in step with the BreakpointManager, and persists the
// breakpoints in the root's .frost/workspace.json.
/*
 * Debug. debug() asks RunService for a Debug run of the startup project. When it is resolved the
 * controller builds a DebugTarget (the program, its arguments split with the CRT's rules, the
 * working directory, the environment and the solution's trust roots) and starts the session with
 * startTarget(); the Debug tool window shows it and the console gets the program's output. One
 * session at a time: debug() while one is live fails with Busy. Stop (stop(), also run.stop through
 * RunActionHooks) ends the debuggee and lldb-dap politely and kills the adapter's process tree when
 * it has not ended after kStopTimeout.
 *
 * Stops. On a stop the frames of the stopped thread load, the selected frame's source opens at its
 * line through hooks.openSource (the workbench's open path) and gets the execution point: a yellow
 * gutter arrow and a highlighted line. If the adapter supplies a sourceReference, navigation and
 * marking are refused with a notice until its loaded bytes can be verified or shown separately.
 * A crash or an unhandled exception selects the faulting frame
 * (the first one with a source) and is reported in the tool window and as a notification
 * (hooks.notify).
 *
 * Breakpoints. attachEditor() connects an EditorView: gutter clicks toggle breakpoints, edits move
 * them (DocumentBreakpointTracker), and the gutter shows them verified (filled red) or not (hollow
 * grey, the adapter's reason as tooltip). Changes reach a live session per source at once and are
 * saved to workspace.json (setWorkspaceRoot) after each change.
 *
 * GUI thread only. Callbacks of a session that has ended are dropped.
 */
#pragma once

#include "frost/build/run_service.hpp"
#include "frost/core/error.hpp"
#include "frost/debug/breakpoints.hpp"
#include "frost/debug/coreclr_dap.hpp"
#include "frost/debug/debug_session.hpp"
#include "frost/debug/lldb_dap.hpp"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace frost::io {
class IFileSystem;
class IProcessLauncher;
} // namespace frost::io

namespace frost::project {
class TrustStore;
}

namespace frost::ui {

class EditorView;
class RunConsole;
class DebugRetirementBatch;

struct DebugControllerHooks {
    // Opens `path` (absolute) at 1-based `line` and returns its editor (the workbench's open path);
    // empty or null: the execution point shows only in editors attached already.
    std::function<EditorView*(const QString& path, int line)> openSource{};
    // The editor Toggle Breakpoint, Run to Cursor and Evaluate act on; empty: none.
    std::function<EditorView*()> currentEditor{};
    // A notification (a crash stop, a session that could not start); empty: logged.
    std::function<void(const QString& title, const QString& text, bool error)> notify{};
    // Shows the Debug tool window (a session started); empty: nothing.
    std::function<void()> showToolWindow{};
    std::function<bool()> alternateAvailable{};
    std::function<bool()> mayStart{};
    std::function<Result<void>(std::function<void(Result<debug::DebugTarget>)>)> startAlternate{};
    std::function<void()> stopAlternate{};
    // RUN-5: Debug starts some other way than a launch (an attach configuration attaches; a compound
    // one is refused) when this returns a value, which debug() returns; nullopt or empty: as usual.
    std::function<std::optional<Result<void>>()> startOverride{};
};

// Where the controller gets its adapter and services; every pointer must outlive the controller.
struct DebugControllerOptions {
    const project::TrustStore* trust = nullptr;    // Required: no session starts without it.
    io::IFileSystem* fs = nullptr;                 // For lldb-dap discovery and workspace.json.
    io::IProcessLauncher* launcher = nullptr;      // Starts lldb-dap.
    std::function<std::string()> lldbDapSetting{}; // tools.lldbDap.path; empty: "".
    std::function<std::string()> nativeBackendSetting{}; // lldb-dap (default), gdb (Linux), frost-dbgeng (Windows).
    std::function<std::string()> dbgEngPathSetting{};
    // GUI-only discovery admits project/dependency/user Natvis files before starting the adapter.
    std::function<Result<std::vector<std::string>>(const debug::DebugTarget&)> natvisFiles{};
    std::function<std::string()> gdbSetting{}; // absolute user-installed GDB >=16.
    std::function<std::pair<std::string, std::string>()> sourceMapSetting{};
    // Overrides lldb-dap (tests: the fake-dap double). Empty: lldb-dap through `launcher`.
    debug::AdapterFactory adapterFactory{};
    bool allowRerun = true; // A registry-created session has no independent configuration/build resolver.
    // Workspace-owned lifetime barrier, required for asynchronous acknowledged attach shutdown.
    // Drain after GUI teardown and before the services captured by transport admission die.
    std::shared_ptr<DebugRetirementBatch> retirements;
};

// The DebugTarget a Debug run's hand-off describes (header comment, "Debug"). Pure.
[[nodiscard]] debug::DebugTarget debugTargetFor(const build::RunOutcome& outcome, const build::DebugLaunch& launch);

class DebugController final : public QObject {
    Q_OBJECT

public:
    // How long stop() waits for lldb-dap to end the session before it kills the tree.
    static constexpr std::chrono::milliseconds kStopTimeout{3000};

    // `breakpoints` must outlive the controller.
    DebugController(debug::BreakpointManager& breakpoints, DebugControllerOptions options,
                    DebugControllerHooks hooks = {}, QObject* parent = nullptr);
    ~DebugController() override;
    DebugController(const DebugController&) = delete ("a QObject");
    DebugController& operator=(const DebugController&) = delete ("a QObject");

    // The run service Debug builds through (null detaches).
    void setRunService(build::RunService* service) noexcept { m_service = service; }
    [[nodiscard]] build::RunService* runService() const noexcept { return m_service; }
    // The console the program's output goes to (optional; it must outlive the controller or be
    // destroyed with it).
    void setConsole(RunConsole* console);
    // That console, or null when none was set or it was destroyed (UI thread).
    [[nodiscard]] RunConsole* console() const noexcept;
    // Loads the breakpoints of `root` (absolute) and saves there from now on; "" stops saving.
    [[nodiscard]] Result<void> setWorkspaceRoot(std::string root);

    // Debug: a Debug run of `project` (default: the startup project). Errors: Busy (a session is
    // live), InvalidArgument (no run service), and RunService::debug's.
    [[nodiscard]] Result<void> debug(std::optional<project::ProjectId> project = std::nullopt);
    // Starts a session on `target` (debug() ends here; tests call it directly). Errors: Busy,
    // InvalidArgument (no trust store), NotFound (lldb-dap is missing), Untrusted (no adapter
    // started), the adapter factory's.
    [[nodiscard]] Result<void> startTarget(const debug::DebugTarget& target);
    using PrivateOutput = std::function<void(std::string_view category, std::string_view text)>;
    // Built-in default pinned LLDB only. Raw output goes exclusively to private worker-safe
    // consumers before the generic console/log path. Final launch admission runs on the writer.
    [[nodiscard]] Result<void> startNativeEngineTarget(const debug::DebugTarget& target, PrivateOutput output,
                                                       std::function<void(std::string_view)> adapterStderr,
                                                       std::function<Result<void>()> launchAdmission);
    // Generic explicitly selected adapter (FER-9/FER-16). Factory/arguments remain caller-owned
    // admission decisions, but every trust root is rechecked here before the factory starts.
    // readOnly disables execution/mutation and disconnects without terminating a host.
    [[nodiscard]] Result<void> startAdapterTarget(const debug::DebugTarget& target, debug::AdapterFactory factory,
                                                  rpc::Json arguments, std::string adapterId,
                                                  debug::DebugTargetMode mode = debug::DebugTargetMode::Launch,
                                                  bool readOnly = false,
                                                  std::function<Result<void>()> launchAdmission = {},
                                                  const std::function<bool()>& ownerAdmission = {});
    [[nodiscard]] bool readOnlySession() const noexcept { return m_session && m_session->readOnly(); }
    // The caller must finish the trusted managed build and verify assembly/PDB/source revisions.
    // Starts the supplied adapter through the same UI and session lifetime as native debugging.
    [[nodiscard]] Result<void>
    startCoreClrTarget(const debug::CoreClrTarget& target, const std::vector<std::string>& trustRoots,
                       debug::AdapterFactory factory, std::function<bool(const debug::DebugFrame&)> frameAllowed = {},
                       std::vector<std::string> allowedSources = {},
                       std::function<void(std::string, std::function<void(bool)>)> validateBreakpoint = {});
    // Attach to an existing host using a preconnected adapter. Trust is checked again here; Stop
    // disconnects the adapter without terminating that host.
    [[nodiscard]] Result<void> attachTarget(const debug::DebugTarget& target, debug::AdapterFactory factory,
                                            rpc::Json attachArguments,
                                            const std::function<bool()>& ownerAdmission = {});

    // Attaches a verified managed module with a source allowlist and per-change validator.
    // The caller must establish process identity, trust, and source freshness before calling.
    [[nodiscard]] Result<void>
    attachManagedTarget(const debug::DebugTarget& target, debug::AdapterFactory factory,
                        std::function<bool(const debug::DebugFrame&)> frameAllowed,
                        std::vector<std::string> allowedSources,
                        std::map<std::string, std::vector<debug::BreakpointLine>> verifiedBreakpoints,
                        std::function<void(std::string, std::function<void(bool)>)> validateBreakpoint,
                        const std::function<bool()>& ownerAdmission);

    // Whether a session is live (Starting, Running or Stopped).
    [[nodiscard]] bool debugging() const noexcept {
        return m_session != nullptr || m_pendingRun.has_value() || m_alternatePending;
    }
    [[nodiscard]] bool canDebug() const {
        return !m_shutdown && (!m_hooks.mayStart || m_hooks.mayStart()) &&
               (m_service != nullptr || (m_hooks.alternateAvailable && m_hooks.alternateAvailable()));
    }
    void shutdown();
    void rerun();
    void cancelRerun();
    [[nodiscard]] bool rerunPending() const noexcept { return m_rerunPending; }
    [[nodiscard]] debug::DebugState state() const noexcept { return m_state; }
    // Changes on every session start and end; use to reject stale asynchronous work.
    [[nodiscard]] std::uint64_t sessionGeneration() const noexcept { return m_generation; }
    [[nodiscard]] bool paused() const noexcept { return m_state == debug::DebugState::Stopped; }
    // Actual adapter stop evidence persists across outgoing resume/step commands until the real
    // continued event or session end. Use for native all-stop heartbeat admission, never paused().
    [[nodiscard]] bool adapterStoppedEvidence() const noexcept {
        return m_session && m_adapterStopped && m_session->state() != debug::DebugState::Ended;
    }
    [[nodiscard]] std::uint64_t adapterEvidenceSequence() const noexcept { return m_adapterEvidenceSequence; }
    [[nodiscard]] bool configured() const {
        return m_session && m_configured && m_session->state() != debug::DebugState::Ended;
    }
    [[nodiscard]] const std::optional<debug::StopInfo>& stopInfo() const noexcept { return m_stop; }
    /*
     * How the last Debug run ended, as its console's closing line says: the debuggee's exit code
     * (lldb-dap's exited event), stopped, or the error (a failed build, a launch that failed, an
     * adapter that vanished). Empty before the first Debug run ends; a new run keeps the previous
     * outcome until it ends itself.
     */
    [[nodiscard]] const std::optional<build::RunOutcome>& lastOutcome() const noexcept { return m_lastOutcome; }
    // "Paused on breakpoint", "Exception: ..." while paused; "Running"; "" without a session.
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] const std::vector<debug::DebugThread>& threads() const noexcept { return m_threads; }
    [[nodiscard]] std::optional<std::int64_t> selectedThread() const noexcept { return m_thread; }
    [[nodiscard]] const debug::FrameList& frames() const noexcept { return m_frames; }
    [[nodiscard]] const std::optional<debug::DebugFrame>& selectedFrame() const noexcept { return m_frame; }
    [[nodiscard]] debug::DebugSession* session() const noexcept { return m_session.get(); }
    // Native live adapters only; managed, embedded Lua and dump sessions never bind an engine PID overlay.
    [[nodiscard]] bool nativeSession() const noexcept { return m_session != nullptr && m_nativeSession; }
    // Captured during default pinned LLDB resolution only; configured/custom factories remain
    // unverified. Never recomputed from mutable settings after this session starts.
    [[nodiscard]] bool nativeOsThreadIdsVerified() const noexcept {
        return nativeSession() && m_nativeOsThreadIdsVerified;
    }
    // GUI thread, O(1). Explicit engine Stop must match both the observed OS PID and current debug
    // generation. Refusal never starts the ordinary stop() kill timeout. done is GUI-thread scoped
    // to that session; actual state changes continue to come only from DAP events.
    [[nodiscard]] Result<void> terminateTarget(std::int64_t expectedPid, std::uint64_t expectedGeneration,
                                               debug::DebugSession::TerminateDone done = {});
    [[nodiscard]] debug::BreakpointManager& breakpoints() noexcept { return m_breakpoints; }
    // The session's title ("App [Debug|x64]").
    [[nodiscard]] const QString& title() const noexcept { return m_title; }

    // Stepping; each reports a failure through hooks.notify and returns false.
    bool resume();
    bool pause();
    bool stepOver();
    bool stepInto();
    bool stepOut();
    // Runs to the current editor's caret line (hooks.currentEditor).
    bool runToCursor();
    // Jumps execution to the current editor's caret line via DAP gotoTargets + goto.
    bool setNextStatement();
    // Optional exact-owned-operation Stop, scoped to the current controller generation. It must
    // report only actual admission and retain its own asynchronous cleanup ownership. No default
    // disconnect/kill timer follows this handler; ordinary attaches never install it. Empty clears.
    void setStopHandler(std::function<Result<void>()> handler);
    // Ends the session (header comment). Idempotent.
    void stop();

    // The Frames list and the thread combo.
    void selectThread(std::int64_t threadId);
    void selectFrame(std::int64_t frameId);
    void loadMoreFrames();

    // Breakpoints. Toggles one at 0-based `line` of `path`.
    void toggleBreakpoint(const std::string& path, int line);
    // Toggles one on the current editor's caret line.
    bool toggleBreakpointAtCaret();
    // Mutes or unmutes all breakpoints without deleting them.
    void setMutedBreakpoints(bool muted);
    void toggleMuteBreakpoints();
    [[nodiscard]] bool breakpointsMuted() const noexcept { return m_breakpoints.muted(); }
    // Edits the breakpoint on the current editor's caret line, or opens the Breakpoints tab.
    void requestEditBreakpoint();
    // Opens the Breakpoints tab in the Debug tool window (Ctrl+Shift+F8).
    void requestViewBreakpoints();
    // Asks for Evaluate Expression (evaluateRequested) with the current editor's selection.
    void requestEvaluate();
    // Connects `view` (header comment); it detaches itself when destroyed.
    void attachEditor(EditorView* view);
    void detachEditor(EditorView* view);
    // A registry selects exactly one controller to present shared editor decorations/navigation.
    // Inactive controllers retain their actual frames and source data but never overwrite another
    // session's execution point, inline values, gutter verification or hover provider. GUI thread.
    void setEditorPresentationEnabled(bool enabled);
    [[nodiscard]] bool editorPresentationEnabled() const noexcept { return m_editorPresentation; }

    // Data for the tool window's models; results arrive on the GUI thread (dropped after the session
    // ends). `done` gets Cancelled when the session ended first.
    using VariablesDone = std::function<void(Result<debug::VariableList>)>;
    using ScopesDone = std::function<void(Result<debug::ScopeList>)>;
    using EvaluateDone = std::function<void(Result<debug::dap::Variable>)>;
    using DisassembleDone = std::function<void(Result<std::vector<debug::dap::DisassembledInstruction>>)>;
    using ReadMemoryDone = std::function<void(Result<debug::dap::ReadMemoryResult>)>;
    void scopes(std::int64_t frameId, ScopesDone done);
    void variables(std::int64_t reference, std::int64_t start, std::int64_t count, VariablesDone done);
    // Evaluates in the selected frame.
    void evaluate(const QString& expression, const QString& context, EvaluateDone done);
    void submitConsoleInput(const QString& line);
    void setVariableValue(std::int64_t variablesReference, const QString& name, const QString& value,
                          EvaluateDone done = {});
    // Turns one of the adapter's exception filters on or off (BreakpointManager); a running lldb-dap
    // session gets the new set. False when the filter is unknown or unavailable.
    bool setExceptionFilterEnabled(std::string_view filter, bool enabled);
    // Adds a function breakpoint by name. Errors: InvalidArgument (empty, or more than one line).
    [[nodiscard]] Result<debug::BreakpointId> addFunctionBreakpoint(const QString& name);
    void disassemble(const QString& memoryReference, int instructionCount, DisassembleDone done);
    void readMemory(const QString& memoryReference, int count, ReadMemoryDone done);

signals:
    // The session console's user Stop button, before stop(). Automatic cleanup calls stop directly.
    void userStopRequested();
    // Adapter-confirmed termination, tagged with the session that emitted it.
    void adapterTerminated(std::uint64_t generation);
    void adapterContinued(std::uint64_t generation);
    // Started, paused, resumed or ended (the toolbar's enabled states).
    void stateChanged();
    // configurationDone ACK (or adapter without that request), generation-scoped like all events.
    void configuredChanged();
    // A stop: its summary, and whether it is a crash or an unhandled exception.
    void stopped(const QString& summary, bool crash);
    void threadsChanged();
    void framesChanged();
    // The selected frame changed (the Variables view reloads; watches are evaluated again).
    void frameSelected();
    // Exact adapter-owned source for the selected frame; empty name clears the read-only view.
    void loadedSourceChanged(const QString& name, const QString& content, int line);
    // The session ended: why, in one line ("Process finished with exit code 0").
    void sessionEnded(const QString& summary);
    // Evaluate Expression (Alt+F8) was asked for, with the editor's selection as the expression.
    void evaluateRequested(const QString& expression);
    void breakpointsChanged();
    void editBreakpointRequested(debug::BreakpointId id);
    void viewBreakpointsRequested();

private:
    struct Relay;
    struct Attached;

    // GUI thread: the session's events.
    void onState(debug::DebugState state);
    void onStopped(const debug::StopInfo& stop);
    void onThreads(std::vector<debug::DebugThread> threads);
    void onFrames(std::int64_t threadId, debug::FrameList frames);
    void onFrameSelected(std::int64_t threadId, const debug::DebugFrame& frame);
    void onOutput(const std::string& category, const std::string& text);
    void onBreakpoints(const std::vector<debug::BreakpointResult>& results);
    // lldb-dap's capabilities: its exception filters go to the BreakpointManager.
    void onCapabilities(const debug::dap::Capabilities& capabilities);
    void onEnded(std::uint64_t generation, const debug::DebugOutcome& outcome);
    void onRunFinished(const build::RunOutcome& outcome, const std::optional<build::DebugLaunch>& launch);

    // Breakpoints.
    void onBreakpointsChanged(std::string_view path, debug::BreakpointManager::Change change);
    void refreshMarkers(EditorView& view);
    void showExecutionPoint();
    void refreshInlineDebuggerValues();
    [[nodiscard]] QString debugHoverFor(std::string_view expression, LineIndex line);
    void clearLoadedSource();
    void save();
    bool validateCoreClrPath(std::string path, std::function<void(DebugController&)> approved);

    [[nodiscard]] Result<debug::AdapterFactory> adapterFactory(bool* nativeOsThreadIdsVerified = nullptr,
                                                               debug::DapProcessOutput output = {}, std::string_view backendOverride = {});
    [[nodiscard]] Result<void> startNativeTarget(const debug::DebugTarget& target, PrivateOutput output,
                                                 debug::DapProcessOutput processOutput,
                                                 std::optional<debug::NativeDebugPrivacy> privacy);
    using SessionStarter = std::function<Result<std::unique_ptr<debug::DebugSession>>(debug::DebugSessionCallbacks)>;
    [[nodiscard]] Result<void> startSession(std::string title, std::string program, std::string adapterName,
                                            const SessionStarter& starter, bool attach = false,
                                            PrivateOutput privateOutput = {});
    bool report(Result<void> result, const QString& what);
    void endSession();

    debug::BreakpointManager& m_breakpoints;
    DebugControllerOptions m_options;
    DebugControllerHooks m_hooks;
    build::RunService* m_service = nullptr;
    QPointer<RunConsole> m_console;
    std::string m_root;
    std::uint64_t m_listener = 0;

    std::shared_ptr<Relay> m_relay;
    std::unique_ptr<debug::DebugSession> m_session;
    std::function<bool(const debug::DebugFrame&)> m_coreClrFrameAllowed;
    std::vector<std::string> m_coreClrAllowedSources;
    std::function<void(std::string, std::function<void(bool)>)> m_coreClrBreakpointValidator;
    std::map<std::string, std::uint64_t> m_coreClrPathRequests;
    std::uint64_t m_coreClrValidationEpoch = 0;
    bool m_coreClrSession = false;
    bool m_lastCoreClrSession = false;
    bool m_alternatePending = false;
    bool m_shutdown = false;
    bool m_configured = false;
    bool m_adapterStopped = false;
    std::uint64_t m_adapterEvidenceSequence = 0;
    std::uint64_t m_stopHandlerGeneration = 0;
    std::function<Result<void>()> m_stopHandler;
    bool m_rerunPending = false;
    bool m_rerunAllowed = true;
    QTimer m_rerunTimer;
    std::uint64_t m_generation = 0; // Bumped per session; stale events are dropped.
    std::uint64_t m_sourceEpoch = 0;
    bool m_sourceEpochExhausted = false;
    std::optional<build::RunId> m_pendingRun;
    QString m_title;
    debug::DebugState m_state = debug::DebugState::Ended;
    std::optional<debug::StopInfo> m_stop;
    std::optional<build::RunOutcome> m_lastOutcome;
    std::vector<debug::DebugThread> m_threads;
    std::optional<std::int64_t> m_thread;
    debug::FrameList m_frames;
    std::optional<debug::DebugFrame> m_frame;
    // The selected frame's own (top-level, cheap scope) variables for inline values and hovers; a
    // child's members never count, as they would name an unrelated identifier on a line.
    std::vector<debug::dap::Variable> m_currentFrameVariables;
    std::vector<std::int64_t> m_frameScopeReferences; // The scopes those come from.
    bool m_editorPresentation = true;
    bool m_nativeSession = false; // The session is lldb-dap's (launch or attach).
    bool m_nativeOsThreadIdsVerified = false;
    std::map<std::string, std::string, std::less<>> m_hoverCache;
    QTimer m_killTimer;

    std::map<EditorView*, std::unique_ptr<Attached>> m_editors;
};

} // namespace frost::ui
