// SPDX-License-Identifier: Apache-2.0
//
// DebugSession: one debug session over an IDebugAdapterClient (PLAN.md M4, ADR 0050), Qt-free. It runs
// the DAP launch sequence, keeps the session's state (running or stopped, why it stopped, threads,
// the frames loaded so far, the selection) and offers the stepping commands, breakpoints per source,
// scopes, variables and evaluate to the Debug tool window.
/*
 * Sequence (DAP 1.71 "Launch Sequencing"):
 *     initialize -> launch -> (event initialized) -> setBreakpoints per source -> configurationDone
 * launch is sent right after the initialize response and its answer is not waited for before the
 * configuration: lldb-dap answers launch only after configurationDone. A failed initialize or launch
 * ends the session with that error.
 *
 * States: Starting (until configurationDone is answered or the first stop), Running, Stopped, Ended.
 * A stop (the stopped event) records its StopInfo, then asks for the threads and the first page of
 * the stopped thread's frames (kFramePage); the selected frame is the top one, or after a crash (an
 * exception or a signal) the first frame with a source that is not marked subtle, so the faulting
 * frame in the user's code is shown rather than a runtime function. More frames load on demand
 * (loadMoreFrames). continueRun, next, stepIn, stepOut and runToLine need Stopped and switch to
 * Running when sent; pause needs Running.
 *
 * Breakpoints. setBreakpoints(path, lines) records the lines of one source and, once the adapter is
 * configured, sends them (DAP replaces a source's breakpoints as a whole). The adapter's answers come
 * back through onBreakpoints, keyed by the caller's ids; later breakpoint events ("changed", for a
 * breakpoint that bound when its module loaded) too. runToLine adds a temporary breakpoint, continues
 * and removes it at the next stop. Function breakpoints (setFunctionBreakpoints) work the same way;
 * they share one setFunctionBreakpoints request with the session's own function breakpoints (abort,
 * and _CxxThrowException below), because DAP replaces all function breakpoints at once.
 *
 * Exception filters (DBG-2a). setExceptionFilters(ids) records the filters to stop on (lldb-dap 23:
 * cpp_throw, cpp_catch, objc_throw, objc_catch, from its exceptionBreakpointFilters capability) and
 * sends them before configurationDone, so a throw early in main stops too. lldb's C++ filters hook the
 * Itanium runtime (__cxa_throw, __cxa_begin_catch), which programs built for the MSVC ABI never call
 * (checked against lldb-dap 23.1.4 on Windows: the filters bind and never fire). With
 * DebugSessionOptions::msvcExceptions (lldb-dap on Windows), cpp_throw therefore becomes a function
 * breakpoint on the MSVC runtime's _CxxThrowException, which every throw calls, and a stop there is
 * reported as an exception stop of filter cpp_throw. lldb binds that name twice when the CRT is a DLL:
 * the program's import thunk (one indirect jmp) and the runtime's function; a stop on the thunk is
 * resumed without being reported (the disassemble request says whether the stop's instruction is a
 * jmp), so one throw stops once. cpp_catch has no exported entry point in the MSVC runtime that lldb
 * can bind without the runtime's symbols, so it is unavailable there (msvcUnsupportedFilter).
 *
 * Thrown types (DBG-2b). At a _CxxThrowException stop that is not the thunk, the session reads which
 * type is thrown from the ThrowInfo the runtime got (msvc_throw.hpp): the frame's pThrowInfo argument
 * when the runtime has symbols (a static CRT), else rdx (the DLL runtime without symbols, where the
 * stop is at its first instruction), then the module that holds it (the modules request and its PE
 * header) and, with readMemory, the first CatchableType's TypeDescriptor name. The stop then carries
 * StopInfo::exceptionType ("std::runtime_error") and the description "C++ exception thrown: <type>".
 * With throw types set (setThrowTypes, DebugSessionOptions::throwTypes), a throw of a type not in the
 * list is resumed without being reported: the list matches the thrown type exactly, never a base or
 * derived class (msvc::throwTypeMatches). A throw whose type cannot be read (an x86 program, memory
 * that cannot be read) is reported, with an "important" output line saying why the filter could
 * not decide. Types read are cached per ThrowInfo address, so a later throw of the same type costs
 * one evaluate request. Sessions without msvcExceptions ignore throw types: lldb's Itanium filter has
 * no type matching, and an "important" output line says so once.
 *
 * Step into a thunk. MSVC's incremental linking (the default for Debug builds) calls every function
 * through a one-jmp thunk of the incremental link table, which has no symbol, line or unwind data;
 * lldb's step-in stops there (seen with lldb-dap 23.1.4 on FrostEditor). When a step-in ends in a
 * frame without a source whose instruction is a jmp (the disassemble request says), the session steps
 * one more instruction (stepIn, granularity "instruction") instead of reporting the stop. That
 * bypasses lldb's step-avoid list, so when the jmp leads into a std:: function (lldb's default
 * target.process.thread.step-avoid-regexp) the session steps out of it and, back on the line the
 * step began on, steps in again towards the line's next call. With stepOutOfNoDebug (lldb-dap on
 * Windows, which launches with lldb's step-in-avoid-nodebug off so that it stops in a thunk it could
 * step out of, as with /ZI; lldb_dap.hpp, "Stepping on Windows") a step-in that stops in a frame
 * without a source whose instruction is not a jmp (a function without debug information, reached
 * directly or through a thunk) is treated the same way: the session steps out of it and, back on the
 * line the step began on, steps in again. The step thus ends in the first non-std callee with debug
 * information; at most kThunkHops extra requests are sent per step. Any other stop is reported as it
 * came.
 *     Where it ends in the callee: the instruction step lands on the callee's first instruction, so
 * the stop is on the callee's opening line (the line of its '{', or of its signature when the brace
 * shares it), before its prologue has run: its arguments and locals are not set up yet, and the
 * next Step Over goes to the first statement. That is where Visual Studio's Step Into stops in a
 * Debug build too. Without a thunk (/INCREMENTAL:NO, clang-cl, Linux) lldb's own step-in runs past
 * the prologue and stops on the first statement, with the arguments set (checked with lldb 23.1.4
 * on cl.exe /Zi /MDd and /ZI /JMC /MDd builds: msvc_debug_test.cpp, MsvcStepInto). A frame "without
 * a source" here includes lldb-dap's disassembly listing of a function (a sourceReference marked
 * "deemphasize").
 *
 * Pause. lldb-dap reports a pause as an exception stop ("signal SIGSTOP" on Linux, "Exception
 * 0x80000003" on Windows, where it lands on a thread the system injected for the break-in). After
 * pause() such a stop is reported with reason "pause", and when the stopped thread has no frame with
 * a source the first other thread that has one is selected (the injected thread only runs
 * DbgBreakPoint).
 *
 * Restart. restart() sends DAP restart with the launch arguments (lldb-dap announces the
 * supportsRestartRequest capability in a capabilities event after configurationDone); the adapter
 * ends the debuggee, launches it again with the same breakpoints and the session continues.
 *
 * The end. stop() sends disconnect, terminating launched debuggees and detaching from attached ones;
 * the terminated event also leads to
 * disconnect; once the adapter answers (or exits) the client is closed. kill() closes it at once (the
 * process transport ends lldb-dap's whole tree, the debuggee with it). onEnded runs once, with the
 * debuggee's exit code when an exited event came and an error when the adapter failed or vanished.
 *
 * Threads. Every member is thread-safe. Callbacks run on the client's reader thread or on the thread
 * that called the member that caused them, never under the session's lock and never after the
 * destructor started; the UI marshals them to its thread. Queries return snapshots.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/debug/dap_client.hpp"
#include "frost/debug/dap_types.hpp"
#include "frost/rpc/json.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::debug {

// Creates the adapter client with the session's handlers (lldb-dap in production, fake-dap in tests).
using AdapterFactory = std::function<Result<std::unique_ptr<IDebugAdapterClient>>(DapHandlers handlers)>;

enum class DebugState : std::uint8_t { Starting, Running, Stopped, Ended };

// An attached process remains owned by its host when the session ends.
enum class DebugTargetMode : std::uint8_t { Launch, Attach };

// "Starting", "Running", "Stopped" or "Ended".
[[nodiscard]] std::string_view toString(DebugState state) noexcept;

// Why the debuggee stopped (the stopped event).
struct StopInfo {
    std::string reason;      // "breakpoint", "step", "pause", "exception", "signal", "entry", ...
    std::string description; // The adapter's text ("Exception 0xc0000005 encountered at ...").
    std::string text;
    std::optional<std::int64_t> threadId;
    bool allThreadsStopped = false;
    std::vector<std::int64_t> hitBreakpointIds;
    // The exception filter that stopped ("cpp_throw", "cpp_catch"); "" for any other stop.
    std::string exceptionFilter{};
    // The thrown C++ type of an MSVC cpp_throw stop ("std::runtime_error"; header comment, "Thrown
    // types"); "" when it was not read.
    std::string exceptionType{};
    // Authenticated FrostEngine extension; never synthesized from DAP threadId or a timeout.
    std::optional<dap::ScriptPauseIdentity> scriptPause;

    // A crash or an unhandled exception: reason "exception" or "signal", not an exception filter's stop.
    [[nodiscard]] bool isCrash() const noexcept;
    // What the Debug window shows: "Paused on breakpoint", "Paused after a step", "Paused",
    // "Paused on exception (<filter label>)", "Exception: <description>", "Signal: <description>",
    // else "Paused (<reason>)".
    [[nodiscard]] std::string summary() const;
};

// A function breakpoint sent with setFunctionBreakpoints, identified by the caller.
struct FunctionBreakpointLine {
    std::uint64_t id = 0;
    std::string name;
    std::string condition{};
    std::string hitCondition{};

    friend bool operator==(const FunctionBreakpointLine&, const FunctionBreakpointLine&) = default;
};

struct DebugThread {
    std::int64_t id = 0;
    std::string name;

    friend bool operator==(const DebugThread&, const DebugThread&) = default;
};

// A stack frame as the Frames list shows it.
struct DebugFrame {
    std::int64_t id = 0;
    std::string name;
    std::string path;                 // The source file; "" without one.
    std::string sourceName;           // Its file name.
    std::int64_t sourceReference = 0; // Adapter-owned loaded source; 0 means a disk path.
    std::int64_t line = 0;            // 1-based; 0 without a source position.
    std::int64_t column = 0;
    std::string module;
    std::string instructionPointerReference; // Exact adapter reference for low-level navigation.
    bool subtle = false; // presentationHint "subtle" or "label".

    [[nodiscard]] bool hasSource() const noexcept { return (!path.empty() || sourceReference > 0) && line > 0; }
};

// The frames of one thread loaded so far.
struct FrameList {
    std::vector<DebugFrame> frames;
    std::optional<std::int64_t> totalFrames; // When the adapter says.
    bool complete = false;                   // Every frame is loaded.
};

// One breakpoint line sent with setBreakpoints, identified by the caller.
struct BreakpointLine {
    std::uint64_t id = 0;
    std::int64_t line = 0; // 1-based.
    bool enabled = true;
    std::string condition{};
    std::string hitCondition{};
    std::string logMessage{};

    friend bool operator==(const BreakpointLine&, const BreakpointLine&) = default;
};

// The adapter's answer for one breakpoint.
struct BreakpointResult {
    std::uint64_t id = 0;
    bool verified = false;
    std::string message;
    std::optional<std::int64_t> line; // Where it bound.

    friend bool operator==(const BreakpointResult&, const BreakpointResult&) = default;
};

using ScopeList = std::vector<dap::Scope>;
using VariableList = std::vector<dap::Variable>;

// How a session ended.
struct DebugOutcome {
    std::optional<int> exitCode;      // The exited event's code.
    std::optional<Error> error;       // The adapter failed to start or launch, or vanished.
    bool stopped = false;             // stop() or kill() ended it.
    bool disconnectConfirmed = false; // A disconnect request completed successfully.
};

// Every callback is optional (header comment for threads).
struct DebugSessionCallbacks {
    std::function<void(DebugState state)> onStateChanged{};
    std::function<void(const StopInfo& stop)> onStopped{};
    std::function<void(const std::vector<DebugThread>& threads)> onThreads{};
    std::function<void(std::int64_t threadId, const FrameList& frames)> onFrames{};
    // The selected frame after a stop, a thread change or selectFrame.
    std::function<void(std::int64_t threadId, const DebugFrame& frame)> onFrameSelected{};
    std::function<void(std::string_view category, std::string_view text)> onOutput{};
    std::function<void(const std::vector<BreakpointResult>& results)> onBreakpoints{};
    std::function<void(int exitCode)> onExited{};
    std::function<void(const DebugOutcome& outcome)> onEnded{};
    std::function<void()> onTerminated{};
    // The adapter's capabilities: after initialize and after each capabilities event.
    std::function<void(const dap::Capabilities& capabilities)> onCapabilities{};
    // Actual process identity events, outside session locks; 0 means the adapter withdrew the PID.
    std::function<void(std::int64_t processId)> onProcess{};
    // Actual configuration barrier: configurationDone succeeded, or the adapter does not use it.
    // A stopped event alone never implies this barrier completed.
    std::function<void()> onConfigured{};
    // Only the adapter's actual continued event, including when command submission already
    // changed the presentation state. Never emitted for an outgoing resume or its ACK.
    std::function<void()> onContinued{};
    // A genuine announced adapter stop, excluding restoration after a refused resume request.
    std::function<void()> onAdapterStopped{};
};

// How a session starts (DebugSession::start).
struct DebugSessionOptions {
    bool breakOnAbort = false; // A function breakpoint on abort (Windows: stop before the CRT ends it).
    std::string adapterId = "lldb-dap";
    DebugTargetMode targetMode = DebugTargetMode::Launch;
    bool msvcExceptions = false;   // Header comment, "Exception filters".
    bool stepOutOfNoDebug = false; // Header comment, "Step into a thunk" (lldb-dap on Windows).
    std::string attachHint{};      // Appended to an attach failure's message (ptraceAttachHint).
    std::vector<FunctionBreakpointLine> functionBreakpoints{};
    std::vector<std::string> exceptionFilters{}; // Filter ids to stop on from the start.
    std::vector<std::string> throwTypes{};       // Header comment, "Thrown types"; empty: every type.
    bool readOnly = false;       // Dump inspection: no execution, mutation, restart or debuggee termination.
    bool privateSession = false; // Native engine bootstrap: protocol diagnostics never contain payloads.
    // Worker-safe exact launch admission, checked by the real DAP writer immediately before launch.
    std::function<Result<void>()> launchAdmission{};
};

/*
 * Why `filter` cannot work for a session with msvcExceptions (header comment, "Exception filters"),
 * or nullopt when it can: cpp_catch (no bindable MSVC runtime entry point; DBG-2b) and the
 * Objective-C filters. Pure.
 */
[[nodiscard]] std::optional<std::string> msvcUnsupportedFilter(std::string_view filter);

class DebugSession {
public:
    // Frames asked for per stackTrace request.
    static constexpr std::int64_t kFramePage = 20;

    /*
     * Creates the adapter client through `factory` and sends initialize; the launch request carries
     * `launchArguments` as they are (lldbDapLaunchArguments builds them). `breakpoints` are the lines
     * per source to configure; `adapterId` identifies the adapter in initialize. A factory error
     * ends the session at once (onEnded with the error, and the session is returned in state Ended).
     * `targetMode` selects launch ownership or a nonterminating attach.
     */
    [[nodiscard]] static std::unique_ptr<DebugSession>
    start(AdapterFactory factory, rpc::Json launchArguments,
          std::map<std::string, std::vector<BreakpointLine>> breakpoints, DebugSessionCallbacks callbacks,
          bool breakOnAbort = false, std::string adapterId = "lldb-dap",
          DebugTargetMode targetMode = DebugTargetMode::Launch);
    [[nodiscard]] static std::unique_ptr<DebugSession>
    start(AdapterFactory factory, rpc::Json launchArguments,
          std::map<std::string, std::vector<BreakpointLine>> breakpoints, DebugSessionCallbacks callbacks,
          bool breakOnAbort, DebugTargetMode targetMode);
    [[nodiscard]] static std::unique_ptr<DebugSession>
    start(AdapterFactory factory, rpc::Json launchArguments,
          std::map<std::string, std::vector<BreakpointLine>> breakpoints, DebugSessionCallbacks callbacks,
          DebugSessionOptions options);

    // Closes the client (kill()) without running callbacks; joins the client's threads.
    ~DebugSession();
    DebugSession(const DebugSession&) = delete ("owns the adapter connection");
    DebugSession& operator=(const DebugSession&) = delete ("owns the adapter connection");

    [[nodiscard]] DebugState state() const;
    [[nodiscard]] dap::Capabilities capabilities() const;
    [[nodiscard]] std::uint64_t inspectionGeneration() const;
    // The last stop; empty while running.
    [[nodiscard]] std::optional<StopInfo> stopInfo() const;
    [[nodiscard]] std::vector<DebugThread> threads() const;
    [[nodiscard]] std::optional<std::int64_t> selectedThread() const;
    [[nodiscard]] FrameList frames(std::int64_t threadId) const;
    [[nodiscard]] std::optional<DebugFrame> selectedFrame() const;
    [[nodiscard]] std::optional<DebugOutcome> outcome() const;
    // The debuggee's process id from the adapter's process event; nullopt before it (or when 0).
    [[nodiscard]] std::optional<std::int64_t> processId() const;
    [[nodiscard]] DebugTargetMode targetMode() const noexcept;
    [[nodiscard]] bool readOnly() const noexcept;
    [[nodiscard]] std::string_view adapterId() const noexcept;
    // Actual successful DAP frame writes; nullopt means the adapter does not expose this oracle.
    [[nodiscard]] std::optional<std::uint64_t> sentMessageCount() const;
    // True when explicit termination is supported, or launch ownership permits standard DAP
    // disconnect(terminateDebuggee=true). An ordinary attached-session stop remains a detach.
    [[nodiscard]] bool canTerminateDebuggee() const;
    using TerminateDone = std::function<void(Result<void>)>;
    // Explicit user termination of this exact observed PID. O(1), thread-safe. Refuses a missing/
    // different PID, Starting/Ended session, duplicate request or unsupported attached termination.
    // done receives the actual DAP response on the session callback thread. A successful response
    // is not a process-exit observation; onTerminated/onEnded carry that separately. No kill fallback.
    [[nodiscard]] Result<void> terminateDebuggee(std::int64_t expectedPid, TerminateDone done = {});

    // Replaces the breakpoint lines of `path` and sends them when configured (header comment).
    void setBreakpoints(const std::string& path, std::vector<BreakpointLine> lines);
    // Replaces the function breakpoints and sends them when configured (header comment).
    void setFunctionBreakpoints(std::vector<FunctionBreakpointLine> breakpoints);
    // Replaces the exception filters to stop on and sends them when configured (header comment).
    void setExceptionFilters(std::vector<std::string> filters);
    // The exception filters set last.
    [[nodiscard]] std::vector<std::string> exceptionFilters() const;
    // Whether this session translates C++ exception filters for the MSVC ABI (DebugSessionOptions).
    [[nodiscard]] bool msvcExceptions() const noexcept;
    // Replaces the C++ types cpp_throw stops for (header comment, "Thrown types"); empty: every type.
    // Takes effect at the next throw.
    void setThrowTypes(std::vector<std::string> types);
    [[nodiscard]] std::vector<std::string> throwTypes() const;

    // Restarts the debuggee (header comment). Errors: Unsupported (the adapter does not take restart,
    // or the session is attached), Busy (still starting or ended), IoError (closed).
    [[nodiscard]] Result<void> restart();

    // Stepping (header comment). Errors: Busy (not Stopped; pause: not Running), IoError (closed).
    [[nodiscard]] Result<void> continueRun();
    [[nodiscard]] Result<void> next();
    [[nodiscard]] Result<void> stepIn();
    // A target ID must come from stepInTargets for this stop and selected frame.
    [[nodiscard]] Result<void> stepInstruction(bool into = true);
    [[nodiscard]] Result<void> stepIntoTarget(std::int64_t targetId);
    using InspectionDone = std::function<void(Result<rpc::Json>)>;
    // Capability-gated stopped inspection; references expire on resume, stop or selection change.
    std::int64_t stepInTargets(InspectionDone done);
    std::int64_t dataBreakpointInfo(std::int64_t reference, std::string name, InspectionDone done);
    // Replaces the complete adapter data-breakpoint set, following DAP semantics.
    std::int64_t setDataBreakpoints(rpc::Json breakpoints, InspectionDone done);

    [[nodiscard]] Result<void> stepOut();
    [[nodiscard]] Result<void> pause();
    // Continues to `line` of `path` through a temporary breakpoint (header comment). Errors: as next().
    [[nodiscard]] Result<void> runToLine(const std::string& path, std::int64_t line);

    // Selects a thread; its first frames load when they are not loaded yet. Errors: NotFound.
    [[nodiscard]] Result<void> selectThread(std::int64_t threadId);
    // Selects a loaded frame of `threadId` (onFrameSelected runs). Errors: NotFound.
    [[nodiscard]] Result<void> selectFrame(std::int64_t threadId, std::int64_t frameId);
    // Loads the next page of frames of `threadId`; nothing when they are complete or loading.
    void loadMoreFrames(std::int64_t threadId);

    using ScopesDone = std::function<void(Result<ScopeList>)>;
    using VariablesDone = std::function<void(Result<VariableList>)>;
    using EvaluateDone = std::function<void(Result<dap::Variable>)>;
    using SourceDone = std::function<void(Result<std::string>)>;
    using SetVariableDone = std::function<void(Result<dap::Variable>)>;
    using ExceptionBreakpointsDone = std::function<void(Result<void>)>;
    using DisassembleDone = std::function<void(Result<std::vector<dap::DisassembledInstruction>>)>;
    using ReadMemoryDone = std::function<void(Result<dap::ReadMemoryResult>)>;
    using GotoLineDone = std::function<void(Result<void>)>;
    // Retrieves the adapter-owned bytes for a stopped source reference; stale stops return Cancelled.
    std::int64_t source(std::int64_t sourceReference, SourceDone done);
    // The scopes of frame `frameId`. Returns the request's seq (for cancel), 0 when not sent.
    std::int64_t scopes(std::int64_t frameId, ScopesDone done);
    // The children of `variablesReference`; `count` 0 asks for all, else `start`.. `start + count`.
    std::int64_t variables(std::int64_t variablesReference, std::int64_t start, std::int64_t count, VariablesDone done, bool hexadecimal = false);
    // Evaluates `expression` in `frameId` (none: the global context) for `context` ("watch", "repl",
    // "hover"); the result is a Variable named after the expression.
    std::int64_t evaluate(std::string expression, std::optional<std::int64_t> frameId, std::string context,
                          EvaluateDone done, bool hexadecimal = false);
    // Modifies variable `name` inside `variablesReference` via DAP `setVariable`.
    std::int64_t setVariable(std::int64_t variablesReference, std::string_view name, std::string_view value,
                             SetVariableDone done = {});
    // Sends DAP setExceptionBreakpoints with the adapter's filter ids as they are, without the MSVC
    // translation of setExceptionFilters (which the IDE uses; this one is for other adapters' callers).
    std::int64_t setExceptionBreakpoints(const std::vector<std::string>& filters, ExceptionBreakpointsDone done = {});
    // Disassembles `instructionCount` instructions starting at `memoryReference`.
    std::int64_t disassemble(std::string_view memoryReference, std::int64_t instructionCount, DisassembleDone done,
                             std::int64_t instructionOffset = 0);
    // Reads `count` bytes from `memoryReference` via DAP `readMemory`.
    std::int64_t readMemory(std::string_view memoryReference, std::int64_t count, ReadMemoryDone done,
                            std::int64_t offset = 0);
    // Resolves `gotoTargets` for `path`:`line` and jumps `threadId` to the first target via DAP `goto`.
    std::int64_t gotoLine(std::int64_t threadId, std::string_view path, std::int64_t line, GotoLineDone done = {});
    // Cancels a source, scopes, variables or evaluate request (its completion runs with Cancelled).
    void cancel(std::int64_t seq);

    // Requests the Lua adapter to stop its owned play operation. Completion reports admission;
    // the terminated event reports completion separately.
    using StopPlayDone = std::function<void(Result<rpc::Json>)>;
    std::int64_t stopPlay(StopPlayDone done);

    // Final teardown only: sends/joins the ordinary DAP detach of an attached/read-only session.
    // Blocks at most grace for its real ACK; never sends terminateDebuggee=true. Errors report
    // refusal/uncertainty, not a fabricated detach success. The caller still owns adapter cleanup.
    [[nodiscard]] Result<void> detachForShutdown(std::chrono::milliseconds grace);
    // Ends the session politely; an attached debuggee is not terminated. Idempotent.
    void stop();
    // Cancels an unfinished attached adapter locally. Sends no disconnect, grants no host
    // termination and records no detach ACK. Refuses once attach and configuration both succeeded.
    [[nodiscard]] Result<void> cancelAttachedPreparation();
    // Ends the adapter's process tree now. Idempotent.
    void kill();

    struct State; // Shared with the client's handlers; src/debug/src/debug_session.cpp.

private:
    explicit DebugSession(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> m_state;
};

} // namespace frost::debug
