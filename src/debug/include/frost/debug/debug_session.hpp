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
 * and removes it at the next stop.
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

    // A crash or an unhandled exception: reason "exception" or "signal".
    [[nodiscard]] bool isCrash() const noexcept;
    // What the Debug window shows: "Paused on breakpoint", "Paused after a step", "Paused",
    // "Exception: <description>", "Signal: <description>", else "Paused (<reason>)".
    [[nodiscard]] std::string summary() const;
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
    std::optional<int> exitCode; // The exited event's code.
    std::optional<Error> error;  // The adapter failed to start or launch, or vanished.
    bool stopped = false;        // stop() or kill() ended it.
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
};

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

    // Closes the client (kill()) without running callbacks; joins the client's threads.
    ~DebugSession();
    DebugSession(const DebugSession&) = delete ("owns the adapter connection");
    DebugSession& operator=(const DebugSession&) = delete ("owns the adapter connection");

    [[nodiscard]] DebugState state() const;
    [[nodiscard]] dap::Capabilities capabilities() const;
    // The last stop; empty while running.
    [[nodiscard]] std::optional<StopInfo> stopInfo() const;
    [[nodiscard]] std::vector<DebugThread> threads() const;
    [[nodiscard]] std::optional<std::int64_t> selectedThread() const;
    [[nodiscard]] FrameList frames(std::int64_t threadId) const;
    [[nodiscard]] std::optional<DebugFrame> selectedFrame() const;
    [[nodiscard]] std::optional<DebugOutcome> outcome() const;

    // Replaces the breakpoint lines of `path` and sends them when configured (header comment).
    void setBreakpoints(std::string path, std::vector<BreakpointLine> lines);

    // Stepping (header comment). Errors: Busy (not Stopped; pause: not Running), IoError (closed).
    [[nodiscard]] Result<void> continueRun();
    [[nodiscard]] Result<void> next();
    [[nodiscard]] Result<void> stepIn();
    [[nodiscard]] Result<void> stepOut();
    [[nodiscard]] Result<void> pause();
    // Continues to `line` of `path` through a temporary breakpoint (header comment). Errors: as next().
    [[nodiscard]] Result<void> runToLine(std::string path, std::int64_t line);

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
    // Retrieves the adapter-owned bytes for a stopped source reference; stale stops return Cancelled.
    std::int64_t source(std::int64_t sourceReference, SourceDone done);
    // The scopes of frame `frameId`. Returns the request's seq (for cancel), 0 when not sent.
    std::int64_t scopes(std::int64_t frameId, ScopesDone done);
    // The children of `variablesReference`; `count` 0 asks for all, else `start`.. `start + count`.
    std::int64_t variables(std::int64_t variablesReference, std::int64_t start, std::int64_t count, VariablesDone done);
    // Evaluates `expression` in `frameId` (none: the global context) for `context` ("watch", "repl",
    // "hover"); the result is a Variable named after the expression.
    std::int64_t evaluate(std::string expression, std::optional<std::int64_t> frameId, std::string context,
                          EvaluateDone done);
    // Cancels a source, scopes, variables or evaluate request (its completion runs with Cancelled).
    void cancel(std::int64_t seq);

    // Requests the Lua adapter to stop its owned play operation. Completion reports admission;
    // the terminated event reports completion separately.
    using StopPlayDone = std::function<void(Result<rpc::Json>)>;
    std::int64_t stopPlay(StopPlayDone done);

    // Ends the session politely; an attached debuggee is not terminated. Idempotent.
    void stop();
    // Ends the adapter's process tree now. Idempotent.
    void kill();

    struct State; // Shared with the client's handlers; src/debug/src/debug_session.cpp.

private:
    explicit DebugSession(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> m_state;
};

} // namespace frost::debug
