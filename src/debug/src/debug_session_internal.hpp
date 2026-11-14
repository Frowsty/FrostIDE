// SPDX-License-Identifier: Apache-2.0
//
// Internal DebugSession state and helpers shared across debug_session*.cpp.

#pragma once

#include "frost/core/mutex.hpp"
#include "frost/debug/debug_session.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::debug {

// The caller id of runToLine's temporary breakpoint (never a caller's id: those start at 1).
inline constexpr std::uint64_t kTemporaryBreakpoint = 0;

// The MSVC runtime's throw entry point (header comment, "Exception filters").
inline constexpr std::string_view kMsvcThrowFunction = "_CxxThrowException";

// Threads tried for a pause stop on a thread without a source frame (header comment, "Pause").
inline constexpr std::size_t kPauseThreadCandidates = 8;
// How many extra requests one step-in may send (header comment, "Step into a thunk"): three per
// skipped std:: call before the call it ends in, one per thunk.
inline constexpr int kThunkHops = 16;
// lldb's default target.process.thread.step-avoid-regexp, "^std::", as a prefix.
inline constexpr std::string_view kStepAvoidPrefix = "std::";

// Where a step-in is (header comment, "Step into a thunk").
enum class StepInPhase : std::uint8_t {
    None,          // No step-in runs.
    Stepping,      // lldb's step-in runs.
    FollowedThunk, // A thunk's jmp was stepped.
    LeftAvoided,   // Stepping out of a std:: function the thunk led into.
};

// Whether lldb-dap's stop is the one a pause request causes (header comment, "Pause").
[[nodiscard]] inline bool isPauseException(const StopInfo& stop) {
    const std::string& detail = !stop.description.empty() ? stop.description : stop.text;
    return stop.reason == "exception" && stop.hitBreakpointIds.empty() &&
           (detail.find("SIGSTOP") != std::string::npos || detail.find("0x80000003") != std::string::npos);
}

// Whether a disassembled instruction is a jump (the import thunk; header comment).
[[nodiscard]] inline bool isJump(std::string_view instruction) {
    while (!instruction.empty() && (instruction.front() == ' ' || instruction.front() == '\t')) {
        instruction.remove_prefix(1);
    }
    return instruction.starts_with("jmp");
}

[[nodiscard]] inline DebugFrame toFrame(const dap::StackFrame& frame) {
    DebugFrame out;
    out.id = frame.id;
    out.instructionPointerReference = frame.instructionPointerReference;
    out.name = frame.name;
    if (frame.source) {
        out.path = frame.source->path;
        out.sourceName = frame.source->name;
        out.sourceReference = frame.source->sourceReference;
        if (frame.source->presentationHint == "deemphasize") {
            out.subtle = true;
        }
    }
    out.line = frame.line;
    out.column = frame.column;
    out.module = frame.moduleName;
    if (frame.presentationHint == "subtle" || frame.presentationHint == "label") {
        out.subtle = true;
    }
    return out;
}

// The frame a stop selects (header comment, "States"); `skipToSource` for crashes and exception
// filter stops, whose top frames are runtime functions.
[[nodiscard]] inline std::optional<DebugFrame> pickFrame(const std::vector<DebugFrame>& frames, bool skipToSource) {
    if (frames.empty()) {
        return std::nullopt;
    }
    if (skipToSource) {
        for (const DebugFrame& frame : frames) {
            if (frame.hasSource() && !frame.subtle) {
                return frame;
            }
        }
    }
    return frames.front();
}

/*
 * Whether `frame` has debug information: a source on disk, or a loaded one that is not lldb-dap's
 * disassembly of a function without it (a sourceReference marked "deemphasize", whose line is a
 * line of the listing; /ZI's thunks come that way, under the __enc$textbss$end symbol).
 */
[[nodiscard]] inline bool hasSource(const dap::StackFrame& frame) {
    if (!frame.source || frame.line <= 0) {
        return false;
    }
    if (frame.source->sourceReference > 0) {
        return frame.source->presentationHint != "deemphasize";
    }
    return !frame.source->path.empty();
}

// Whether the module at `path` is the launched `program` (case-insensitive and either slash on Windows).
[[nodiscard]] inline bool sameProgram(std::string_view path, std::string_view program) {
#ifdef _WIN32
    if (path.size() != program.size())
        return false;
    for (std::size_t i = 0; i < path.size(); ++i) {
        auto fold = [](char c) {
            if (c == '/')
                return '\\';
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        };
        if (fold(path[i]) != fold(program[i]))
            return false;
    }
    return true;
#else
    return path == program;
#endif
}

struct DebugSession::State : std::enable_shared_from_this<DebugSession::State> {
    // One throw stop's type read: the stop, its thread and runtime frame, and what was read so far.
    struct ThrowRead {
        StopInfo info;
        std::int64_t thread = 0;
        std::int64_t frameId = 0;
        std::uint64_t throwInfo = 0;
        std::uint64_t base = 0;
        std::uint32_t imageSize = 0;
    };
    using ThrowReadPtr = std::shared_ptr<ThrowRead>;

    State(rpc::Json arguments, std::map<std::string, std::vector<BreakpointLine>> lines,
          DebugSessionCallbacks sessionCallbacks, DebugSessionOptions options)
        : callbacks(std::move(sessionCallbacks)), launchArguments(std::move(arguments)),
          privateSession(options.privateSession), launchAdmission(std::move(options.launchAdmission)),
          adapterId(std::move(options.adapterId)), attachHint(std::move(options.attachHint)),
          breakpoints(std::move(lines)), functionBreakpoints(std::move(options.functionBreakpoints)),
          exceptionFilters(std::move(options.exceptionFilters)), throwTypes(std::move(options.throwTypes)),
          breakOnAbort(options.breakOnAbort), msvcExceptions(options.msvcExceptions),
          stepOutOfNoDebug(options.stepOutOfNoDebug), targetMode(options.targetMode), readOnly(options.readOnly) {}

    const DebugSessionCallbacks callbacks;
    const rpc::Json launchArguments;
    const bool privateSession;
    const std::function<Result<void>()> launchAdmission;
    const std::string adapterId;
    const std::string attachHint;
    std::atomic<IDebugAdapterClient*> adapter{nullptr};
    std::atomic<bool> silent{false}; // The destructor started: no more callbacks.

    mutable Mutex mutex;
    Mutex startMutex; // Serializes launch/configuration admission with stop.
    Mutex deferredMutex;
    unsigned startupDepth FROST_GUARDED_BY(deferredMutex) = 0;
    bool draining FROST_GUARDED_BY(deferredMutex) = false;
    std::deque<std::function<void()>> deferred FROST_GUARDED_BY(deferredMutex);
    std::unique_ptr<IDebugAdapterClient> client FROST_GUARDED_BY(mutex);
    DebugState state FROST_GUARDED_BY(mutex) = DebugState::Starting;
    dap::Capabilities caps FROST_GUARDED_BY(mutex);
    bool configured FROST_GUARDED_BY(mutex) = false;
    bool configurationAcknowledged FROST_GUARDED_BY(mutex) = false;
    bool attachAcknowledged FROST_GUARDED_BY(mutex) = false;
    bool stopRequested FROST_GUARDED_BY(mutex) = false;
    bool terminateRequested FROST_GUARDED_BY(mutex) = false;
    bool ordinaryStopRequested FROST_GUARDED_BY(mutex) = false;
    bool disconnectSent FROST_GUARDED_BY(mutex) = false;
    bool disconnectConfirmed FROST_GUARDED_BY(mutex) = false;
    std::shared_future<Result<void>> disconnectResult FROST_GUARDED_BY(mutex);
    bool terminatedSeen FROST_GUARDED_BY(mutex) = false;
    std::optional<StopInfo> stop FROST_GUARDED_BY(mutex);
    std::vector<DebugThread> threads FROST_GUARDED_BY(mutex);
    std::optional<std::int64_t> currentThread FROST_GUARDED_BY(mutex);
    std::map<std::int64_t, FrameList> frames FROST_GUARDED_BY(mutex);
    std::set<std::int64_t> loading FROST_GUARDED_BY(mutex);
    std::optional<DebugFrame> currentFrame FROST_GUARDED_BY(mutex);
    std::set<std::int64_t> stepTargets FROST_GUARDED_BY(mutex);
    std::uint64_t stepTargetsGeneration FROST_GUARDED_BY(mutex) = 0;
    std::int64_t stepTargetsFrame FROST_GUARDED_BY(mutex) = 0;
    std::optional<std::int64_t> stepTargetsThread FROST_GUARDED_BY(mutex);
    std::uint64_t generation FROST_GUARDED_BY(mutex) = 0; // Bumped by each stop and resume.
    std::map<std::string, std::vector<BreakpointLine>> breakpoints FROST_GUARDED_BY(mutex);
    std::vector<FunctionBreakpointLine> functionBreakpoints FROST_GUARDED_BY(mutex);
    std::vector<std::string> exceptionFilters FROST_GUARDED_BY(mutex);
    std::vector<std::string> throwTypes FROST_GUARDED_BY(mutex); // Header comment, "Exception filters".
    bool throwTypesWarned FROST_GUARDED_BY(mutex) = false;       // The non-MSVC note was printed.
    bool symbolsWarned FROST_GUARDED_BY(mutex) = false;          // The program's "no symbols" note was printed.
    // ThrowInfo address -> {decorated, undecorated} type names read for it (DBG-2b).
    std::map<std::uint64_t, std::pair<std::string, std::string>> thrownTypes FROST_GUARDED_BY(mutex);
    // Module base -> SizeOfImage of the modules whose PE headers were read.
    std::map<std::uint64_t, std::uint32_t> imageSizes FROST_GUARDED_BY(mutex);
    std::map<std::int64_t, std::uint64_t> adapterIds FROST_GUARDED_BY(mutex); // Adapter id -> caller id.
    std::optional<std::pair<std::string, std::int64_t>> runTo FROST_GUARDED_BY(mutex);
    const bool breakOnAbort;
    const bool msvcExceptions;
    const bool stepOutOfNoDebug;
    const DebugTargetMode targetMode;
    const bool readOnly;
    std::optional<std::int64_t> abortBreakpointId FROST_GUARDED_BY(mutex);
    std::optional<std::int64_t> throwBreakpointId FROST_GUARDED_BY(mutex); // _CxxThrowException's adapter id.
    std::optional<std::int64_t> processId FROST_GUARDED_BY(mutex);
    bool pauseRequested FROST_GUARDED_BY(mutex) = false;
    StepInPhase stepInPhase FROST_GUARDED_BY(mutex) = StepInPhase::None;
    int thunkHops FROST_GUARDED_BY(mutex) = 0; // Requests the current step-in added.
    // The top frame's name and line when the step-in began.
    std::optional<std::pair<std::string, std::int64_t>> stepInOrigin FROST_GUARDED_BY(mutex);
    std::vector<std::int64_t> pauseCandidates FROST_GUARDED_BY(mutex); // Header comment, "Pause".
    std::optional<std::int64_t> pauseOrigin FROST_GUARDED_BY(mutex);
    std::optional<int> exitCode FROST_GUARDED_BY(mutex);
    std::optional<Error> fatalError FROST_GUARDED_BY(mutex);
    std::optional<DebugOutcome> outcome FROST_GUARDED_BY(mutex);

    template<typename F>
    void emit(F&& call) const {
        if (!silent.load()) {
            std::forward<F>(call)();
        }
    }

    // Drain in arrival order, never while a startup operation holds its serialization lock.
    void drainDeferred() {
        for (;;) {
            std::function<void()> next;
            {
                LockGuard lock(deferredMutex);
                if (startupDepth != 0 || deferred.empty()) {
                    draining = false;
                    return;
                }
                next = std::move(deferred.front());
                deferred.pop_front();
            }
            next();
        }
    }
    void afterStartup(std::function<void()> next) {
        bool run = false;
        {
            LockGuard lock(deferredMutex);
            deferred.push_back(std::move(next));
            if (startupDepth == 0 && !draining) {
                draining = true;
                run = true;
            }
        }
        if (run)
            drainDeferred();
    }
    template<typename F>
    void withStartup(F&& action) {
        {
            LockGuard startLock(startMutex);
            {
                LockGuard lock(deferredMutex);
                ++startupDepth;
            }
            std::forward<F>(action)();
        }
        bool run = false;
        {
            LockGuard lock(deferredMutex);
            --startupDepth;
            if (startupDepth == 0 && !draining && !deferred.empty()) {
                draining = true;
                run = true;
            }
        }
        if (run)
            drainDeferred();
    }

    // Even an adapter that completes inline cannot invoke session callbacks under startMutex.
    std::int64_t send(std::string command, rpc::Json arguments, IDebugAdapterClient::Completion done) {
        if (readOnly && command == "disconnect")
            arguments.set("terminateDebuggee", false);
        if (readOnly && command != "initialize" && command != "launch" && command != "attach" &&
            command != "configurationDone" && command != "threads" && command != "stackTrace" && command != "scopes" &&
            command != "variables" && command != "readMemory" && command != "disassemble" && command != "source" &&
            command != "modules" && command != "frost.setSymbolPaths" && command != "frost.reloadSymbols" &&
            command != "frost.setSourceMap" && command != "frost.reloadNatvis" &&
            command != "cancel" && command != "disconnect" &&
            !(command == "evaluate" && arguments.stringAt("context") != "repl")) {
            afterStartup([done = std::move(done)]() mutable {
                if (done)
                    done(fail(ErrorCode::Unsupported, "This dump session permits inspection only"));
            });
            return 0;
        }
        auto safeDone = [weak = weak(), done = std::move(done)](Result<rpc::Json> result) mutable {
            if (const auto self = weak.lock()) {
                self->afterStartup(
                    [done = std::move(done), result = std::move(result)]() mutable { done(std::move(result)); });
            }
        };
        IDebugAdapterClient* const live = adapter.load();
        if (live == nullptr || silent.load()) {
            auto failure = fail(ErrorCode::IoError, std::format("{}: the debug session has ended", command));
            auto callback = std::move(safeDone);
            callback(std::move(failure));
            return 0;
        }
        return live->request(std::move(command), std::move(arguments), std::move(safeDone));
    }

    std::weak_ptr<State> weak() { return weak_from_this(); }

    // Whether cpp_throw is translated to the _CxxThrowException function breakpoint (header comment).
    [[nodiscard]] bool translatesThrow() const FROST_REQUIRES(mutex) {
        return msvcExceptions &&
               std::ranges::find(exceptionFilters, std::string("cpp_throw")) != exceptionFilters.end();
    }

    // Defined in debug_session.cpp
    void sendInitialize();
    void initialized(Result<rpc::Json> body);
    [[nodiscard]] Error attachFailure(const Error& why) const;
    void capabilitiesChanged(const rpc::Json& body);
    void configure();
    void configurationAccepted();
    void becomeRunning();
    void disconnect(bool terminateDebuggee);
    void fatal(Error error);
    void closed(const Error& why);
    void moduleEvent(const rpc::Json& body);
    void event(std::string_view name, const rpc::Json& body);

    // Defined in debug_session_breakpoints.cpp
    void sendBreakpoints(const std::string& path);
    void breakpointsAnswered(const std::vector<BreakpointLine>& lines, Result<rpc::Json> body);
    void breakpointEvent(const rpc::Json& body);
    void sendFunctionBreakpoints(bool initial);
    void functionBreakpointsAnswered(const std::vector<FunctionBreakpointLine>& user, bool withAbort, bool withThrow,
                                     Result<rpc::Json> body);
    void noteIgnoredThrowTypes();
    void sendExceptionFilters(bool initial);

    // Defined in debug_session_stops.cpp
    void stopped(const rpc::Json& body);
    void topFrame(std::int64_t thread, std::function<void(std::optional<dap::StackFrame> top, bool jump)> then);
    void checkStepThunk(StopInfo info);
    void checkThrowThunk(StopInfo info);
    void resumeThrow(std::int64_t thread);
    void readThrownType(StopInfo info, std::int64_t thread, const dap::StackFrame& top);
    void evaluateThrowInfo(const ThrowReadPtr& read, std::string expression);
    void findThrowModule(const ThrowReadPtr& read);
    void readThrowInfo(const ThrowReadPtr& read);
    void readBytes(std::uint64_t address, std::uint64_t count, std::function<void(Result<std::string>)> then);
    void throwTypeRead(const ThrowReadPtr& read, Result<std::pair<std::string, std::string>> names);
    void announceStop(StopInfo info);
    void sendWithoutThread(const std::string& command);
    void requestThreads(std::uint64_t gen, std::optional<std::string> then);
    void loadFrames(std::int64_t threadId, std::uint64_t gen, bool select);
    void framesAnswered(std::int64_t threadId, std::uint64_t gen, bool select, Result<rpc::Json> body);
    std::int64_t inspect(std::string command, rpc::Json arguments, DebugSession::InspectionDone done);
    Result<void> resume(const std::string& command, rpc::Json extra = rpc::Json::object(), std::optional<std::uint64_t> expectedGeneration = std::nullopt,
                        std::optional<std::int64_t> expectedFrame = std::nullopt);
    void continued();
};

} // namespace frost::debug
