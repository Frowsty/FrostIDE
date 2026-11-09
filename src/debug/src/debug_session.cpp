// SPDX-License-Identifier: Apache-2.0
//
// DebugSession (see frost/debug/debug_session.hpp): the launch sequence, the stop handling with lazy
// threads and frames, breakpoints per source, stepping, and the end of the session.

#include "frost/debug/debug_session.hpp"

#include "frost/core/log.hpp"
#include "frost/core/mutex.hpp"
#include "frost/debug/msvc_throw.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <format>
#include <set>

namespace frost::debug {

std::string_view toString(DebugState state) noexcept {
    switch (state) {
    case DebugState::Starting: return "Starting";
    case DebugState::Running: return "Running";
    case DebugState::Stopped: return "Stopped";
    case DebugState::Ended: return "Ended";
    }
    return "Starting";
}

bool StopInfo::isCrash() const noexcept {
    return exceptionFilter.empty() && (reason == "exception" || reason == "signal" || reason == "abort");
}

std::string StopInfo::summary() const {
    const std::string& detail = !description.empty() ? description : text;
    if (!exceptionFilter.empty()) {
        return std::format("Paused on exception ({})", text.empty() ? exceptionFilter : text);
    }
    if (reason == "breakpoint" || reason == "function breakpoint" || reason == "instruction breakpoint") {
        return "Paused on breakpoint";
    }
    if (reason == "step") {
        return "Paused after a step";
    }
    if (reason == "pause") {
        return "Paused";
    }
    if (reason == "entry") {
        return "Paused on entry";
    }
    if (reason == "abort") {
        return "Program called abort()";
    }
    if (reason == "exception") {
        return "Exception: " + (detail.empty() ? std::string("unknown") : detail);
    }
    if (reason == "signal") {
        return "Signal: " + (detail.empty() ? std::string("unknown") : detail);
    }
    return std::format("Paused ({})", reason.empty() ? std::string("unknown reason") : reason);
}

std::optional<std::string> msvcUnsupportedFilter(std::string_view filter) {
    if (filter == "cpp_catch") {
        return std::string("lldb-dap cannot stop where a C++ exception is caught in a program built for the MSVC ABI: "
                           "the MSVC runtime has no exported catch entry point lldb can bind without its symbols "
                           "(DBG-2b)");
    }
    if (filter.starts_with("objc_") || filter.starts_with("swift_")) {
        return std::string("Not available for Windows programs");
    }
    return std::nullopt;
}

namespace {

// The caller id of runToLine's temporary breakpoint (never a caller's id: those start at 1).
constexpr std::uint64_t kTemporaryBreakpoint = 0;

// The MSVC runtime's throw entry point (header comment, "Exception filters").
constexpr std::string_view kMsvcThrowFunction = "_CxxThrowException";

// Threads tried for a pause stop on a thread without a source frame (header comment, "Pause").
constexpr std::size_t kPauseThreadCandidates = 8;
// How many extra requests one step-in may send (header comment, "Step into a thunk"): three per
// skipped std:: call before the call it ends in, one per thunk.
constexpr int kThunkHops = 16;
// lldb's default target.process.thread.step-avoid-regexp, "^std::", as a prefix.
constexpr std::string_view kStepAvoidPrefix = "std::";

// Where a step-in is (header comment, "Step into a thunk").
enum class StepInPhase : std::uint8_t {
    None,          // No step-in runs.
    Stepping,      // lldb's step-in runs.
    FollowedThunk, // A thunk's jmp was stepped.
    LeftAvoided,   // Stepping out of a std:: function the thunk led into.
};

// Whether lldb-dap's stop is the one a pause request causes (header comment, "Pause").
[[nodiscard]] bool isPauseException(const StopInfo& stop) {
    const std::string& detail = !stop.description.empty() ? stop.description : stop.text;
    return stop.reason == "exception" && stop.hitBreakpointIds.empty() &&
           (detail.find("SIGSTOP") != std::string::npos || detail.find("0x80000003") != std::string::npos);
}

// Whether a disassembled instruction is a jump (the import thunk; header comment).
[[nodiscard]] bool isJump(std::string_view instruction) {
    while (!instruction.empty() && (instruction.front() == ' ' || instruction.front() == '\t')) {
        instruction.remove_prefix(1);
    }
    return instruction.starts_with("jmp");
}

DebugFrame toFrame(const dap::StackFrame& frame) {
    DebugFrame out;
    out.id = frame.id;
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
std::optional<DebugFrame> pickFrame(const std::vector<DebugFrame>& frames, bool skipToSource) {
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

} // namespace

struct DebugSession::State : std::enable_shared_from_this<DebugSession::State> {
    State(rpc::Json arguments, std::map<std::string, std::vector<BreakpointLine>> lines,
          DebugSessionCallbacks sessionCallbacks, DebugSessionOptions options)
        : callbacks(std::move(sessionCallbacks)), launchArguments(std::move(arguments)),
          adapterId(std::move(options.adapterId)), attachHint(std::move(options.attachHint)),
          breakpoints(std::move(lines)), functionBreakpoints(std::move(options.functionBreakpoints)),
          exceptionFilters(std::move(options.exceptionFilters)), throwTypes(std::move(options.throwTypes)),
          breakOnAbort(options.breakOnAbort), msvcExceptions(options.msvcExceptions),
          stepOutOfNoDebug(options.stepOutOfNoDebug), targetMode(options.targetMode) {}

    const DebugSessionCallbacks callbacks;
    const rpc::Json launchArguments;
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
    bool stopRequested FROST_GUARDED_BY(mutex) = false;
    bool disconnectSent FROST_GUARDED_BY(mutex) = false;
    bool disconnectConfirmed FROST_GUARDED_BY(mutex) = false;
    bool terminatedSeen FROST_GUARDED_BY(mutex) = false;
    std::optional<StopInfo> stop FROST_GUARDED_BY(mutex);
    std::vector<DebugThread> threads FROST_GUARDED_BY(mutex);
    std::optional<std::int64_t> currentThread FROST_GUARDED_BY(mutex);
    std::map<std::int64_t, FrameList> frames FROST_GUARDED_BY(mutex);
    std::set<std::int64_t> loading FROST_GUARDED_BY(mutex);
    std::optional<DebugFrame> currentFrame FROST_GUARDED_BY(mutex);
    std::uint64_t generation FROST_GUARDED_BY(mutex) = 0; // Bumped by each stop and resume.
    std::map<std::string, std::vector<BreakpointLine>> breakpoints FROST_GUARDED_BY(mutex);
    std::vector<FunctionBreakpointLine> functionBreakpoints FROST_GUARDED_BY(mutex);
    std::vector<std::string> exceptionFilters FROST_GUARDED_BY(mutex);
    std::vector<std::string> throwTypes FROST_GUARDED_BY(mutex); // Header comment, "Exception filters".
    bool throwTypesWarned FROST_GUARDED_BY(mutex) = false;       // The non-MSVC note was printed.
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
            action();
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

    // --- The launch sequence ---

    void sendInitialize() {
        send("initialize", dap::initializeArguments(adapterId), [weak = weak()](Result<rpc::Json> body) {
            if (const auto self = weak.lock()) {
                self->initialized(std::move(body));
            }
        });
    }

    void initialized(Result<rpc::Json> body) {
        withStartup([&] {
            {
                LockGuard lock(mutex);
                if (stopRequested || state == DebugState::Ended)
                    return;
            }
            if (!body) {
                fatal(std::move(body.error()).withContext("while initializing the debug adapter"));
                return;
            }
            const dap::Capabilities found = dap::parseCapabilities(*body);
            {
                LockGuard lock(mutex);
                caps = found;
            }
            if (IDebugAdapterClient* const live = adapter.load()) {
                live->setSupportsCancel(found.supportsCancelRequest);
            }
            afterStartup([weak = weak(), found] {
                if (const auto self = weak.lock()) {
                    self->emit([&] {
                        if (self->callbacks.onCapabilities)
                            self->callbacks.onCapabilities(found);
                    });
                }
            });
            const char* const command = targetMode == DebugTargetMode::Attach ? "attach" : "launch";
            send(command, launchArguments, [weak = weak()](Result<rpc::Json> answer) {
                if (const auto self = weak.lock(); self && !answer) {
                    self->fatal(self->targetMode == DebugTargetMode::Attach
                                    ? self->attachFailure(answer.error())
                                    : Error(answer.error().code(), std::format("The program could not be started: {}",
                                                                               answer.error().message())));
                }
            });
        });
    }

    // An attach that failed, with the platform's hint (DebugSessionOptions::attachHint).
    [[nodiscard]] Error attachFailure(const Error& why) const {
        std::string message = std::format("Could not attach to the program: {}", why.message());
        if (!attachHint.empty()) {
            message += "\n" + attachHint;
        }
        return Error(why.code(), std::move(message));
    }

    // The capabilities event: merges what changed (lldb-dap's supportsRestartRequest arrives here).
    void capabilitiesChanged(const rpc::Json& body) {
        dap::Capabilities merged;
        {
            LockGuard lock(mutex);
            caps = dap::mergeCapabilities(caps, body);
            merged = caps;
        }
        emit([&] {
            if (callbacks.onCapabilities)
                callbacks.onCapabilities(merged);
        });
    }

    // The initialized event: the breakpoints, then configurationDone.
    void configure() {
        withStartup([&] {
            std::vector<std::string> paths;
            bool configurationDone = false;
            {
                LockGuard lock(mutex);
                if (configured || stopRequested || state == DebugState::Ended) {
                    return;
                }
                configured = true;
                for (const auto& [path, lines] : breakpoints) {
                    paths.push_back(path);
                }
                configurationDone = caps.supportsConfigurationDoneRequest;
            }
            for (const std::string& path : paths) {
                sendBreakpoints(path);
            }
            sendFunctionBreakpoints(true);
            sendExceptionFilters(true);
            if (!configurationDone) {
                afterStartup([weak = weak()] {
                    if (const auto self = weak.lock())
                        self->becomeRunning();
                });
                return;
            }
            send("configurationDone", rpc::Json(), [weak = weak()](Result<rpc::Json> answer) {
                if (const auto self = weak.lock()) {
                    if (!answer) {
                        // lldb-dap answers an attach to a process it could not stop only here.
                        self->fatal(self->targetMode == DebugTargetMode::Attach ? self->attachFailure(answer.error())
                                                                                : std::move(answer.error()));
                        return;
                    }
                    self->becomeRunning();
                }
            });
            // lldb-dap's first threads reply is the cached launch-time list (ADR 0049). Consume it
            // now so a later stopped event queries the actual stopped threads.
            send("threads", rpc::Json(), [](Result<rpc::Json>) {});
        });
    }

    // Starting -> Running (a stop that came first wins).
    void becomeRunning() {
        {
            LockGuard lock(mutex);
            if (state != DebugState::Starting || stopRequested) {
                return;
            }
            state = DebugState::Running;
        }
        emit([&] {
            if (callbacks.onStateChanged) {
                callbacks.onStateChanged(DebugState::Running);
            }
        });
    }

    // --- Breakpoints ---

    void sendBreakpoints(const std::string& path) {
        std::vector<BreakpointLine> lines;
        {
            LockGuard lock(mutex);
            if (!configured || state == DebugState::Ended) {
                return;
            }
            if (const auto it = breakpoints.find(path); it != breakpoints.end()) {
                lines = it->second;
            }
            if (runTo && runTo->first == path) {
                const std::int64_t target = runTo->second;
                if (std::ranges::none_of(lines, [&](const BreakpointLine& l) { return l.line == target; })) {
                    lines.push_back(BreakpointLine{kTemporaryBreakpoint, target});
                }
            }
        }
        std::vector<BreakpointLine> activeLines;
        std::vector<dap::SourceBreakpoint> sourceBreakpoints;
        activeLines.reserve(lines.size());
        sourceBreakpoints.reserve(lines.size());
        for (const BreakpointLine& line : lines) {
            if (!line.enabled) {
                continue;
            }
            activeLines.push_back(line);
            sourceBreakpoints.push_back(dap::SourceBreakpoint{
                .line = line.line,
                .enabled = line.enabled,
                .condition = line.condition,
                .hitCondition = line.hitCondition,
                .logMessage = line.logMessage,
            });
        }
        send("setBreakpoints", dap::setBreakpointsArguments(path, sourceBreakpoints),
             [weak = weak(), activeLines](Result<rpc::Json> body) {
                 if (const auto self = weak.lock()) {
                     self->breakpointsAnswered(activeLines, std::move(body));
                 }
             });
    }

    void breakpointsAnswered(const std::vector<BreakpointLine>& lines, Result<rpc::Json> body) {
        std::vector<BreakpointResult> results;
        {
            LockGuard lock(mutex);
            const rpc::Json* answers = body ? body->find("breakpoints") : nullptr;
            for (std::size_t i = 0; i < lines.size(); ++i) {
                if (lines[i].id == kTemporaryBreakpoint) {
                    continue;
                }
                BreakpointResult result{.id = lines[i].id, .verified = false, .message = {}, .line = std::nullopt};
                if (!body) {
                    result.message = body.error().message();
                } else if (answers != nullptr && i < answers->size()) {
                    const dap::Breakpoint breakpoint = dap::parseBreakpoint(answers->items()[i]);
                    result.verified = breakpoint.verified;
                    result.message = breakpoint.message;
                    result.line = breakpoint.line;
                    if (breakpoint.id) {
                        adapterIds[*breakpoint.id] = lines[i].id;
                    }
                } else {
                    result.message = "The debug adapter did not answer for this breakpoint";
                }
                results.push_back(std::move(result));
            }
        }
        if (!results.empty()) {
            emit([&] {
                if (callbacks.onBreakpoints) {
                    callbacks.onBreakpoints(results);
                }
            });
        }
    }

    void breakpointEvent(const rpc::Json& body) {
        const dap::BreakpointEvent event = dap::parseBreakpointEvent(body);
        if (!event.breakpoint.id || event.reason == "removed") {
            return;
        }
        BreakpointResult result;
        {
            LockGuard lock(mutex);
            const auto it = adapterIds.find(*event.breakpoint.id);
            if (it == adapterIds.end()) {
                return;
            }
            result = BreakpointResult{.id = it->second,
                                      .verified = event.breakpoint.verified,
                                      .message = event.breakpoint.message,
                                      .line = event.breakpoint.line};
        }
        emit([&] {
            if (callbacks.onBreakpoints) {
                callbacks.onBreakpoints({result});
            }
        });
    }

    // --- Function breakpoints and exception filters ---

    // Whether cpp_throw is translated to the _CxxThrowException function breakpoint (header comment).
    [[nodiscard]] bool translatesThrow() const FROST_REQUIRES(mutex) {
        return msvcExceptions &&
               std::ranges::find(exceptionFilters, std::string("cpp_throw")) != exceptionFilters.end();
    }

    // One setFunctionBreakpoints request with the caller's breakpoints, then abort and
    // _CxxThrowException when the session needs them; `initial` skips an empty first request.
    void sendFunctionBreakpoints(bool initial) {
        std::vector<FunctionBreakpointLine> user;
        bool withAbort = false;
        bool withThrow = false;
        bool supported = false;
        {
            LockGuard lock(mutex);
            if (!configured || state == DebugState::Ended) {
                return;
            }
            supported = caps.supportsFunctionBreakpoints;
            user = functionBreakpoints;
            withAbort = breakOnAbort;
            withThrow = translatesThrow();
        }
        if (!supported) {
            std::vector<BreakpointResult> results;
            for (const FunctionBreakpointLine& line : user) {
                results.push_back(BreakpointResult{.id = line.id,
                                                   .verified = false,
                                                   .message = "The debug adapter does not support function breakpoints",
                                                   .line = std::nullopt});
            }
            if (!results.empty()) {
                emit([&] {
                    if (callbacks.onBreakpoints)
                        callbacks.onBreakpoints(results);
                });
            }
            return;
        }
        if (initial && user.empty() && !withAbort && !withThrow) {
            return; // Nothing to replace yet.
        }
        std::vector<dap::FunctionBreakpoint> list;
        for (const FunctionBreakpointLine& line : user) {
            list.push_back(dap::FunctionBreakpoint{
                .name = line.name, .condition = line.condition, .hitCondition = line.hitCondition});
        }
        if (withAbort) {
            list.push_back(dap::FunctionBreakpoint{.name = "abort"});
        }
        if (withThrow) {
            list.push_back(dap::FunctionBreakpoint{.name = std::string(kMsvcThrowFunction)});
        }
        send("setFunctionBreakpoints", dap::setFunctionBreakpointsArguments(list),
             [weak = weak(), user, withAbort, withThrow](Result<rpc::Json> body) {
                 if (const auto self = weak.lock()) {
                     self->functionBreakpointsAnswered(user, withAbort, withThrow, std::move(body));
                 }
             });
    }

    void functionBreakpointsAnswered(const std::vector<FunctionBreakpointLine>& user, bool withAbort, bool withThrow,
                                     Result<rpc::Json> body) {
        std::vector<BreakpointResult> results;
        {
            LockGuard lock(mutex);
            const rpc::Json* answers = body ? body->find("breakpoints") : nullptr;
            const auto answerAt = [&](std::size_t index) -> std::optional<dap::Breakpoint> {
                if (answers == nullptr || index >= answers->size()) {
                    return std::nullopt;
                }
                return dap::parseBreakpoint(answers->items()[index]);
            };
            for (std::size_t i = 0; i < user.size(); ++i) {
                BreakpointResult result{.id = user[i].id, .verified = false, .message = {}, .line = std::nullopt};
                if (!body) {
                    result.message = body.error().message();
                } else if (const auto answer = answerAt(i)) {
                    result.verified = answer->verified;
                    result.message = answer->message;
                    result.line = answer->line;
                    if (answer->id) {
                        adapterIds[*answer->id] = user[i].id;
                    }
                } else {
                    result.message = "The debug adapter did not answer for this breakpoint";
                }
                results.push_back(std::move(result));
            }
            std::size_t next = user.size();
            abortBreakpointId.reset();
            throwBreakpointId.reset();
            if (withAbort) {
                if (const auto answer = answerAt(next)) {
                    abortBreakpointId = answer->id;
                }
                ++next;
            }
            if (withThrow) {
                if (const auto answer = answerAt(next)) {
                    throwBreakpointId = answer->id;
                }
            }
        }
        if (!body) {
            log::warn("debug", "setFunctionBreakpoints failed: {}", body.error().message());
        }
        if (!results.empty()) {
            emit([&] {
                if (callbacks.onBreakpoints) {
                    callbacks.onBreakpoints(results);
                }
            });
        }
    }

    // Says once that a session without msvcExceptions ignores throw types (header comment).
    void noteIgnoredThrowTypes() {
        {
            LockGuard lock(mutex);
            if (msvcExceptions || !configured || throwTypes.empty() || throwTypesWarned ||
                std::ranges::find(exceptionFilters, std::string("cpp_throw")) == exceptionFilters.end()) {
                return;
            }
            throwTypesWarned = true;
        }
        emit([&] {
            if (callbacks.onOutput)
                callbacks.onOutput("important", "Exception type filters work only for programs built for the MSVC "
                                                "ABI; this debugger stops at every C++ throw.\n");
        });
    }

    // setExceptionBreakpoints with the filters the adapter handles itself (header comment).
    void sendExceptionFilters(bool initial) {
        noteIgnoredThrowTypes();
        std::vector<std::string> filters;
        bool offered = false;
        {
            LockGuard lock(mutex);
            if (!configured || state == DebugState::Ended) {
                return;
            }
            offered = !caps.exceptionBreakpointFilters.empty();
            for (const std::string& filter : exceptionFilters) {
                if (msvcExceptions && (filter == "cpp_throw" || msvcUnsupportedFilter(filter))) {
                    continue; // Translated (cpp_throw) or unavailable for the MSVC ABI.
                }
                filters.push_back(filter);
            }
        }
        if (initial && filters.empty() && !offered) {
            return;
        }
        send("setExceptionBreakpoints", dap::setExceptionBreakpointsArguments(filters),
             [weak = weak()](Result<rpc::Json> body) {
                 const auto self = weak.lock();
                 if (!self || body) {
                     return;
                 }
                 const std::string message =
                     std::format("Exception breakpoints could not be set: {}\n", body.error().message());
                 self->emit([&] {
                     if (self->callbacks.onOutput)
                         self->callbacks.onOutput("important", message);
                 });
             });
    }

    // --- Stops ---

    void stopped(const rpc::Json& body) {
        const dap::StoppedEvent event = dap::parseStoppedEvent(body);
        StopInfo info{.reason = event.reason,
                      .description = event.description,
                      .text = event.text,
                      .threadId = event.threadId,
                      .allThreadsStopped = event.allThreadsStopped,
                      .hitBreakpointIds = event.hitBreakpointIds};
        bool throwHit = false;
        bool stepInStop = false;
        {
            LockGuard lock(mutex);
            if (state == DebugState::Ended) {
                return;
            }
            stepInStop = stepInPhase != StepInPhase::None && info.reason == "step" && thunkHops < kThunkHops &&
                         caps.supportsSteppingGranularity && caps.supportsDisassembleRequest;
            const auto hit = [&](const std::optional<std::int64_t>& id) {
                return id && std::ranges::find(info.hitBreakpointIds, *id) != info.hitBreakpointIds.end();
            };
            if (hit(abortBreakpointId)) {
                info.reason = "abort";
                info.description = "Program called abort()";
            } else if (hit(throwBreakpointId)) {
                throwHit = true;
                info.reason = "exception";
                info.description = "C++ exception thrown";
                info.exceptionFilter = "cpp_throw";
                info.text = "C++ Throw";
                for (const dap::ExceptionFilter& filter : caps.exceptionBreakpointFilters) {
                    if (filter.filter == "cpp_throw") {
                        info.text = filter.label;
                    }
                }
            } else if (info.reason == "exception" && !info.text.empty()) {
                // lldb-dap names the filter that stopped by its label in `text` ("C++ Throw").
                for (const dap::ExceptionFilter& filter : caps.exceptionBreakpointFilters) {
                    if (filter.label == info.text) {
                        info.exceptionFilter = filter.filter;
                    }
                }
            }
            if (pauseRequested && isPauseException(info)) {
                info.reason = "pause";
            }
        }
        if (throwHit && info.threadId) {
            checkThrowThunk(std::move(info));
            return;
        }
        if (stepInStop && info.threadId) {
            checkStepThunk(std::move(info));
            return;
        }
        announceStop(std::move(info));
    }

    /*
     * The top frame of `thread` and whether its instruction is a jmp (asked only for a frame without
     * a source); `then` gets nullopt when the stack trace failed. A failed disassemble counts as "not
     * a jmp".
     */
    void topFrame(std::int64_t thread, std::function<void(std::optional<dap::StackFrame> top, bool jump)> then) {
        send("stackTrace", rpc::Json::object().set("threadId", thread).set("startFrame", 0).set("levels", 1),
             [weak = weak(), then = std::move(then)](Result<rpc::Json> body) mutable {
                 const auto self = weak.lock();
                 if (!self) {
                     return;
                 }
                 std::optional<dap::StackFrame> top;
                 if (body) {
                     if (const rpc::Json* stack = body->find("stackFrames"); stack != nullptr && stack->size() > 0) {
                         top = dap::parseStackFrame(stack->items().front());
                     }
                 }
                 if (!top || hasSource(*top) || top->instructionPointerReference.empty()) {
                     then(std::move(top), false);
                     return;
                 }
                 const std::string address = top->instructionPointerReference;
                 self->send("disassemble", dap::disassembleArguments(address, 1, 0),
                            [then = std::move(then), top = std::move(top)](Result<rpc::Json> answer) mutable {
                                const auto instructions = answer ? dap::parseDisassembleResponse(*answer)
                                                                 : std::vector<dap::DisassembledInstruction>{};
                                then(std::move(top), !instructions.empty() && isJump(instructions.front().instruction));
                            });
             });
    }

    /*
     * Whether `frame` has debug information: a source on disk, or a loaded one that is not lldb-dap's
     * disassembly of a function without it (a sourceReference marked "deemphasize", whose line is a
     * line of the listing; /ZI's thunks come that way, under the __enc$textbss$end symbol).
     */
    static bool hasSource(const dap::StackFrame& frame) {
        if (!frame.source || frame.line <= 0) {
            return false;
        }
        if (frame.source->sourceReference > 0) {
            return frame.source->presentationHint != "deemphasize";
        }
        return !frame.source->path.empty();
    }

    // A step-in's stop: follows a thunk, or reports the stop (header comment, "Step into a thunk").
    void checkStepThunk(StopInfo info) {
        const std::int64_t thread = *info.threadId;
        topFrame(thread, [weak = weak(), info = std::move(info), thread](std::optional<dap::StackFrame> top,
                                                                         bool jump) mutable {
            const auto self = weak.lock();
            if (!self) {
                return;
            }
            std::string command;
            rpc::Json arguments = rpc::Json::object().set("threadId", thread);
            {
                LockGuard lock(self->mutex);
                if (self->state == DebugState::Ended || self->stopRequested) {
                    return;
                }
                const StepInPhase phase = self->stepInPhase;
                if (top && jump) {
                    command = "stepIn"; // Through the thunk's jmp.
                    arguments.set("granularity", "instruction");
                    self->stepInPhase = StepInPhase::FollowedThunk;
                } else if (top && phase == StepInPhase::FollowedThunk && hasSource(*top) &&
                           top->name.starts_with(kStepAvoidPrefix)) {
                    command = "stepOut"; // A function lldb's own step-in would not stop in.
                    self->stepInPhase = StepInPhase::LeftAvoided;
                } else if (top && !hasSource(*top) && self->stepOutOfNoDebug &&
                           (phase == StepInPhase::Stepping || phase == StepInPhase::FollowedThunk)) {
                    command = "stepOut"; // No debug information: lldb's step-in-avoid-nodebug, done here.
                    self->stepInPhase = StepInPhase::LeftAvoided;
                } else if (top && phase == StepInPhase::LeftAvoided && self->stepInOrigin &&
                           top->name == self->stepInOrigin->first && top->line == self->stepInOrigin->second) {
                    command = "stepIn"; // Back on the line the step began on: on to its next call.
                    self->stepInPhase = StepInPhase::Stepping;
                }
                if (!command.empty()) {
                    ++self->thunkHops;
                }
            }
            if (command.empty()) {
                self->announceStop(std::move(info));
                return;
            }
            self->send(command, std::move(arguments), [weak = self->weak(), info](Result<rpc::Json> answer) mutable {
                const auto state = weak.lock();
                if (state && !answer) {
                    state->announceStop(std::move(info)); // The stop as it came.
                }
            });
        });
    }

    // A _CxxThrowException stop: resumes it when it is the import thunk, else reads the thrown type
    // (header comment, "Exception filters").
    void checkThrowThunk(StopInfo info) {
        const std::int64_t thread = *info.threadId;
        send("stackTrace", rpc::Json::object().set("threadId", thread).set("startFrame", 0).set("levels", 1),
             [weak = weak(), info, thread](Result<rpc::Json> body) mutable {
                 const auto self = weak.lock();
                 if (!self) {
                     return;
                 }
                 std::optional<dap::StackFrame> top;
                 if (body) {
                     if (const rpc::Json* stack = body->find("stackFrames"); stack != nullptr && stack->size() > 0) {
                         top = dap::parseStackFrame(stack->items().front());
                     }
                 }
                 if (!top || top->instructionPointerReference.empty()) {
                     self->announceStop(std::move(info));
                     return;
                 }
                 const std::string address = top->instructionPointerReference;
                 self->send("disassemble", dap::disassembleArguments(address, 1, 0),
                            [weak = self->weak(), info = std::move(info), thread,
                             top = std::move(*top)](Result<rpc::Json> answer) mutable {
                                const auto state = weak.lock();
                                if (!state) {
                                    return;
                                }
                                const auto instructions = answer ? dap::parseDisassembleResponse(*answer)
                                                                 : std::vector<dap::DisassembledInstruction>{};
                                if (instructions.empty() || !isJump(instructions.front().instruction)) {
                                    state->readThrownType(std::move(info), thread, top);
                                    return;
                                }
                                // The import thunk: the runtime's location of the same breakpoint stops next.
                                state->resumeThrow(thread);
                            });
             });
    }

    // Continues a throw stop that is not reported (a thunk, a type the filter leaves out).
    void resumeThrow(std::int64_t thread) {
        {
            LockGuard lock(mutex);
            if (state == DebugState::Ended || stopRequested) {
                return;
            }
        }
        send("continue", rpc::Json::object().set("threadId", thread), [](Result<rpc::Json>) {});
    }

    // --- MSVC thrown types (DBG-2b; msvc_throw.hpp for the structures) ---

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

    /*
     * Reads the thrown type of a _CxxThrowException stop and reports or resumes it (header comment,
     * "Exception filters"): the ThrowInfo pointer is the runtime frame's pThrowInfo argument when the
     * runtime has symbols, else rdx (the stop is then at the function's first instruction, so the
     * argument register still holds it). Cached per ThrowInfo address for the session.
     */
    void readThrownType(StopInfo info, std::int64_t thread, const dap::StackFrame& top) {
        auto read =
            std::make_shared<ThrowRead>(ThrowRead{.info = std::move(info), .thread = thread, .frameId = top.id});
        evaluateThrowInfo(read, hasSource(top) ? "pThrowInfo" : "$rdx");
    }

    void evaluateThrowInfo(const ThrowReadPtr& read, std::string expression) {
        rpc::Json arguments =
            rpc::Json::object().set("expression", expression).set("frameId", read->frameId).set("context", "watch");
        send("evaluate", std::move(arguments), [weak = weak(), read, expression](Result<rpc::Json> body) {
            const auto self = weak.lock();
            if (!self) {
                return;
            }
            std::optional<std::uint64_t> address;
            if (body) {
                address = msvc::parseAddress(body->stringAt("result"));
            }
            if (!address || *address == 0) {
                if (expression != "$rdx") {
                    self->evaluateThrowInfo(read, "$rdx"); // The argument has no location here.
                    return;
                }
                self->throwTypeRead(read, fail(ErrorCode::NotFound, "the debugger did not show the ThrowInfo "
                                                                    "argument of _CxxThrowException"));
                return;
            }
            read->throwInfo = *address;
            std::optional<std::pair<std::string, std::string>> known;
            {
                LockGuard lock(self->mutex);
                if (const auto found = self->thrownTypes.find(*address); found != self->thrownTypes.end()) {
                    known = found->second;
                }
                for (const auto& [base, size] : self->imageSizes) {
                    if (*address >= base && *address - base < size) {
                        read->base = base;
                        read->imageSize = size;
                    }
                }
            }
            if (known) {
                self->throwTypeRead(read, std::move(*known));
            } else if (read->base != 0) {
                self->readThrowInfo(read);
            } else {
                self->findThrowModule(read);
            }
        });
    }

    // The module holding the ThrowInfo: the highest module base below it, confirmed by its PE header.
    void findThrowModule(const ThrowReadPtr& read) {
        send("modules", rpc::Json::object(), [weak = weak(), read](Result<rpc::Json> body) {
            const auto self = weak.lock();
            if (!self) {
                return;
            }
            if (!body) {
                self->throwTypeRead(read, std::unexpected(std::move(body.error())));
                return;
            }
            std::uint64_t best = 0;
            if (const rpc::Json* modules = body->find("modules"); modules != nullptr && modules->isArray()) {
                for (const rpc::Json& module : modules->items()) {
                    const auto base = msvc::parseAddress(module.stringAt("addressRange"));
                    if (base && *base <= read->throwInfo && *base > best) {
                        best = *base;
                    }
                }
            }
            if (best == 0) {
                self->throwTypeRead(read, fail(ErrorCode::NotFound, "no loaded module holds the ThrowInfo"));
                return;
            }
            read->base = best;
            self->readBytes(best, msvc::kImageHeaderBytes, [weak = self->weak(), read](Result<std::string> bytes) {
                const auto state = weak.lock();
                if (!state) {
                    return;
                }
                if (!bytes) {
                    state->throwTypeRead(read, std::unexpected(std::move(bytes.error())));
                    return;
                }
                auto header = msvc::parseImageHeader(*bytes);
                if (!header) {
                    state->throwTypeRead(read, std::unexpected(std::move(header.error())));
                    return;
                }
                if (header->machine != msvc::kMachineAmd64) {
                    state->throwTypeRead(read,
                                         fail(ErrorCode::Unsupported, "exception type filters need an x64 program"));
                    return;
                }
                if (read->throwInfo - read->base >= header->sizeOfImage) {
                    state->throwTypeRead(read, fail(ErrorCode::NotFound, "no loaded module holds the ThrowInfo"));
                    return;
                }
                read->imageSize = header->sizeOfImage;
                {
                    LockGuard lock(state->mutex);
                    state->imageSizes[read->base] = header->sizeOfImage;
                }
                state->readThrowInfo(read);
            });
        });
    }

    // ThrowInfo -> CatchableTypeArray[0] -> CatchableType -> TypeDescriptor name.
    void readThrowInfo(const ThrowReadPtr& read) {
        // An image-relative offset inside the module, or an error.
        const auto at = [read](std::optional<std::uint32_t> rva) -> Result<std::uint64_t> {
            if (!rva || *rva == 0 || *rva >= read->imageSize) {
                return fail(ErrorCode::Corrupt, "the ThrowInfo points outside its module");
            }
            return read->base + *rva;
        };
        readBytes(read->throwInfo, msvc::kThrowInfoSize, [weak = weak(), read, at](Result<std::string> info) {
            const auto self = weak.lock();
            if (!self) {
                return;
            }
            auto array = info ? at(msvc::readU32(*info, msvc::kThrowInfoCatchableTypeArray))
                              : Result<std::uint64_t>(std::unexpected(std::move(info.error())));
            if (!array) {
                self->throwTypeRead(read, std::unexpected(std::move(array.error())));
                return;
            }
            self->readBytes(*array, 8, [weak = self->weak(), read, at](Result<std::string> types) {
                const auto state = weak.lock();
                if (!state) {
                    return;
                }
                const auto count = types ? msvc::readU32(*types, 0) : std::nullopt;
                auto first = types && count && *count > 0
                                 ? at(msvc::readU32(*types, 4))
                                 : Result<std::uint64_t>(fail(ErrorCode::Corrupt, "the ThrowInfo lists no type"));
                if (!first) {
                    state->throwTypeRead(read, std::unexpected(std::move(first.error())));
                    return;
                }
                state->readBytes(*first, 8, [weak = state->weak(), read, at](Result<std::string> catchable) {
                    const auto again = weak.lock();
                    if (!again) {
                        return;
                    }
                    auto descriptor = catchable ? at(msvc::readU32(*catchable, msvc::kCatchableTypeTypeDescriptor))
                                                : Result<std::uint64_t>(std::unexpected(std::move(catchable.error())));
                    if (!descriptor) {
                        again->throwTypeRead(read, std::unexpected(std::move(descriptor.error())));
                        return;
                    }
                    again->readBytes(
                        *descriptor + msvc::kTypeDescriptorName64, msvc::kMaxTypeNameBytes,
                        [weak = again->weak(), read](Result<std::string> name) {
                            const auto last = weak.lock();
                            if (!last) {
                                return;
                            }
                            if (!name) {
                                last->throwTypeRead(read, std::unexpected(std::move(name.error())));
                                return;
                            }
                            std::string decorated(msvc::cString(*name));
                            if (!decorated.starts_with('.')) {
                                last->throwTypeRead(read, fail(ErrorCode::Corrupt, "the thrown type has no RTTI name"));
                                return;
                            }
                            std::string undecorated = msvc::undecorateTypeName(decorated).value_or(decorated);
                            std::pair names{std::move(decorated), std::move(undecorated)};
                            {
                                LockGuard lock(last->mutex);
                                last->thrownTypes[read->throwInfo] = names;
                            }
                            last->throwTypeRead(read, std::move(names));
                        });
                });
            });
        });
    }

    // readMemory of `count` bytes at `address`; an error when the adapter read none.
    void readBytes(std::uint64_t address, std::uint64_t count, std::function<void(Result<std::string>)> then) {
        send("readMemory",
             dap::readMemoryArguments(std::format("0x{:X}", address), static_cast<std::int64_t>(count), 0),
             [then = std::move(then), address](Result<rpc::Json> body) {
                 if (!body) {
                     then(std::unexpected(std::move(body.error())));
                     return;
                 }
                 dap::ReadMemoryResult result = dap::parseReadMemoryResponse(*body);
                 if (result.decodedBytes.empty()) {
                     then(fail(ErrorCode::IoError,
                               std::format("the debugger could not read memory at 0x{:X}", address)));
                     return;
                 }
                 then(std::move(result.decodedBytes));
             });
    }

    /*
     * The read's end: the type ({decorated, undecorated}) or why it could not be read. A type the
     * filter leaves out resumes the program; anything else is reported, an unread type with a
     * filter set with a note saying why the filter could not decide.
     */
    void throwTypeRead(const ThrowReadPtr& read, Result<std::pair<std::string, std::string>> names) {
        std::vector<std::string> filter;
        {
            LockGuard lock(mutex);
            filter = throwTypes;
        }
        StopInfo info = std::move(read->info);
        if (names) {
            info.exceptionType = names->second;
            info.description = "C++ exception thrown: " + names->second;
            if (!msvc::throwTypeMatches(filter, names->second, names->first)) {
                resumeThrow(read->thread);
                return;
            }
        } else if (!filter.empty()) {
            const std::string note =
                std::format("The thrown type could not be read ({}), so the debugger stopped at this throw "
                            "although the exception type filter may not include it.\n",
                            names.error().message());
            emit([&] {
                if (callbacks.onOutput)
                    callbacks.onOutput("important", note);
            });
        }
        announceStop(std::move(info));
    }

    void announceStop(StopInfo info) {
        std::uint64_t gen = 0;
        std::optional<std::string> restore;
        {
            LockGuard lock(mutex);
            if (state == DebugState::Ended) {
                return;
            }
            pauseRequested = false;
            stepInPhase = StepInPhase::None;
            thunkHops = 0;
            stepInOrigin.reset();
            pauseCandidates.clear();
            pauseOrigin = info.reason == "pause" ? info.threadId : std::nullopt;
            state = DebugState::Stopped;
            stop = info;
            gen = ++generation;
            frames.clear();
            loading.clear();
            currentFrame.reset();
            if (info.threadId) {
                currentThread = info.threadId;
            }
            if (runTo) {
                restore = runTo->first;
                runTo.reset();
            }
        }
        if (restore) {
            sendBreakpoints(*restore);
        }
        emit([&] {
            if (callbacks.onStateChanged) {
                callbacks.onStateChanged(DebugState::Stopped);
            }
            if (callbacks.onStopped) {
                callbacks.onStopped(info);
            }
        });
        requestThreads(gen, std::nullopt);
    }

    // Asks for the threads; then loads the selected thread's frames (and, with `then`, pauses it).
    /*
     * A thread request (pause) before any thread is known: lldb-dap answers threads with an empty or
     * stale list while the program runs, and pauses the whole process whatever threadId says, so
     * the request goes out with thread 0 rather than not at all.
     */
    void sendWithoutThread(const std::string& command) {
        send(command, rpc::Json::object().set("threadId", 0), [](Result<rpc::Json>) {});
    }

    void requestThreads(std::uint64_t gen, std::optional<std::string> then) {
        send("threads", rpc::Json(), [weak = weak(), gen, then](Result<rpc::Json> body) {
            const auto self = weak.lock();
            if (!self) {
                return;
            }
            if (!body) {
                log::warn("debug", "threads failed: {}", body.error().message());
                if (then) {
                    self->sendWithoutThread(*then);
                }
                return;
            }
            std::vector<DebugThread> list;
            if (const rpc::Json* threads = body->find("threads"); threads != nullptr) {
                for (const rpc::Json& thread : threads->items()) {
                    const dap::Thread parsed = dap::parseThread(thread);
                    list.push_back(DebugThread{parsed.id, parsed.name});
                }
            }
            std::optional<std::int64_t> selected;
            {
                LockGuard lock(self->mutex);
                if (gen != self->generation) {
                    return;
                }
                self->threads = list;
                const std::optional<std::int64_t> current = self->currentThread;
                if (!current || std::ranges::none_of(list, [&](const DebugThread& t) { return t.id == *current; })) {
                    self->currentThread = list.empty() ? std::nullopt : std::optional<std::int64_t>(list.front().id);
                }
                selected = self->currentThread;
                self->pauseCandidates.clear();
                if (self->pauseOrigin) {
                    for (const DebugThread& thread : list) {
                        if (thread.id != *self->pauseOrigin && self->pauseCandidates.size() < kPauseThreadCandidates) {
                            self->pauseCandidates.push_back(thread.id);
                        }
                    }
                }
            }
            self->emit([&] {
                if (self->callbacks.onThreads) {
                    self->callbacks.onThreads(list);
                }
            });
            if (then && selected) {
                self->send(*then, rpc::Json::object().set("threadId", *selected), [](Result<rpc::Json>) {});
                return;
            }
            if (then) {
                self->sendWithoutThread(*then);
                return;
            }
            if (selected) {
                self->loadFrames(*selected, gen, true);
            }
        });
    }

    void loadFrames(std::int64_t threadId, std::uint64_t gen, bool select) {
        std::int64_t start = 0;
        {
            LockGuard lock(mutex);
            if (gen != generation || state != DebugState::Stopped || loading.contains(threadId)) {
                return;
            }
            const FrameList& list = frames[threadId];
            if (list.complete) {
                return;
            }
            start = static_cast<std::int64_t>(list.frames.size());
            loading.insert(threadId);
        }
        send("stackTrace",
             rpc::Json::object().set("threadId", threadId).set("startFrame", start).set("levels", kFramePage),
             [weak = weak(), threadId, gen, select](Result<rpc::Json> body) {
                 if (const auto self = weak.lock()) {
                     self->framesAnswered(threadId, gen, select, std::move(body));
                 }
             });
    }

    void framesAnswered(std::int64_t threadId, std::uint64_t gen, bool select, Result<rpc::Json> body) {
        FrameList snapshot;
        std::optional<DebugFrame> chosen;
        std::int64_t chosenThread = threadId;
        std::optional<std::int64_t> tryNext; // Header comment, "Pause".
        {
            LockGuard lock(mutex);
            loading.erase(threadId);
            if (gen != generation) {
                return;
            }
            FrameList& list = frames[threadId];
            if (!body) {
                list.complete = true;
                log::warn("debug", "stackTrace failed: {}", body.error().message());
            } else {
                std::size_t received = 0;
                if (const rpc::Json* stack = body->find("stackFrames"); stack != nullptr) {
                    for (const rpc::Json& frame : stack->items()) {
                        list.frames.push_back(toFrame(dap::parseStackFrame(frame)));
                        ++received;
                    }
                }
                list.totalFrames = body->integerAt("totalFrames");
                list.complete =
                    received < static_cast<std::size_t>(kFramePage) ||
                    (list.totalFrames && static_cast<std::int64_t>(list.frames.size()) >= *list.totalFrames);
            }
            snapshot = list;
            if (select && !currentFrame && currentThread == threadId) {
                const bool pausedWithoutSource = stop && stop->reason == "pause" && pauseOrigin &&
                                                 std::ranges::none_of(list.frames, [](const DebugFrame& frame) {
                                                     return frame.hasSource() && !frame.subtle;
                                                 });
                if (pausedWithoutSource && !pauseCandidates.empty()) {
                    tryNext = pauseCandidates.front();
                    pauseCandidates.erase(pauseCandidates.begin());
                    currentThread = tryNext;
                } else if (pausedWithoutSource && threadId != *pauseOrigin) {
                    // No other thread has a source frame either: back to the thread that stopped.
                    chosenThread = *pauseOrigin;
                    currentThread = chosenThread;
                    currentFrame = pickFrame(frames[chosenThread].frames, false);
                    chosen = currentFrame;
                } else {
                    // A pause lands in system code (a sleep, a wait) as often as not: show the program's frame.
                    currentFrame = pickFrame(list.frames, stop && (stop->isCrash() || !stop->exceptionFilter.empty() ||
                                                                   stop->reason == "pause"));
                    chosen = currentFrame;
                }
            }
        }
        emit([&] {
            if (callbacks.onFrames) {
                callbacks.onFrames(threadId, snapshot);
            }
            if (chosen && callbacks.onFrameSelected) {
                callbacks.onFrameSelected(chosenThread, *chosen);
            }
        });
        if (tryNext) {
            loadFrames(*tryNext, gen, true);
        }
    }

    // --- Resuming ---

    // Sends `command` for the current thread after switching to Running.
    Result<void> resume(const std::string& command) {
        std::int64_t thread = 0;
        std::uint64_t commandGeneration = 0;
        std::optional<StopInfo> oldStop;
        std::map<std::int64_t, FrameList> oldFrames;
        std::optional<DebugFrame> oldFrame;
        {
            LockGuard lock(mutex);
            if (state != DebugState::Stopped) {
                return fail(ErrorCode::Busy, std::format("{} needs a paused program", command));
            }
            thread = currentThread.value_or(threads.empty() ? 1 : threads.front().id);
            oldStop = stop;
            oldFrames = frames;
            oldFrame = currentFrame;
            state = DebugState::Running;
            stepInPhase = command == "stepIn" ? StepInPhase::Stepping : StepInPhase::None;
            thunkHops = 0;
            stepInOrigin.reset();
            if (const auto top = frames.find(thread); top != frames.end() && !top->second.frames.empty()) {
                stepInOrigin.emplace(top->second.frames.front().name, top->second.frames.front().line);
            }
            commandGeneration = ++generation;
            stop.reset();
            frames.clear();
            loading.clear();
            currentFrame.reset();
        }
        emit([&] {
            if (callbacks.onStateChanged)
                callbacks.onStateChanged(DebugState::Running);
        });
        send(command, rpc::Json::object().set("threadId", thread),
             [weak = weak(), command, commandGeneration, thread, oldStop = std::move(oldStop),
              oldFrames = std::move(oldFrames), oldFrame = std::move(oldFrame)](Result<rpc::Json> answer) mutable {
                 const auto self = weak.lock();
                 if (!self || answer)
                     return;
                 bool restored = false;
                 {
                     LockGuard lock(self->mutex);
                     if (!self->stopRequested && self->state == DebugState::Running &&
                         self->generation == commandGeneration) {
                         self->state = DebugState::Stopped;
                         ++self->generation;
                         self->stop = oldStop;
                         self->frames = oldFrames;
                         self->currentFrame = oldFrame;
                         restored = true;
                     }
                 }
                 if (!restored)
                     return;
                 const std::string message = std::format("{} failed: {}\n", command, answer.error().message());
                 self->emit([&] {
                     if (self->callbacks.onOutput)
                         self->callbacks.onOutput("important", message);
                     if (self->callbacks.onStateChanged)
                         self->callbacks.onStateChanged(DebugState::Stopped);
                     if (oldStop && self->callbacks.onStopped)
                         self->callbacks.onStopped(*oldStop);
                     if (const auto it = oldFrames.find(thread); it != oldFrames.end() && self->callbacks.onFrames)
                         self->callbacks.onFrames(thread, it->second);
                     if (oldFrame && self->callbacks.onFrameSelected)
                         self->callbacks.onFrameSelected(thread, *oldFrame);
                 });
             });
        return {};
    }

    void continued() {
        {
            LockGuard lock(mutex);
            if (state == DebugState::Running) {
                ++generation; // A continued event supersedes any outstanding resume reply.
                return;
            }
            if (state != DebugState::Stopped) {
                return;
            }
            state = DebugState::Running;
            ++generation;
            stop.reset();
            frames.clear();
            loading.clear();
            currentFrame.reset();
        }
        emit([&] {
            if (callbacks.onStateChanged) {
                callbacks.onStateChanged(DebugState::Running);
            }
        });
    }

    // --- The end ---

    void disconnect(bool terminateDebuggee) {
        {
            LockGuard lock(mutex);
            if (disconnectSent || state == DebugState::Ended) {
                return;
            }
            disconnectSent = true;
        }
        send("disconnect", rpc::Json::object().set("terminateDebuggee", terminateDebuggee).set("restart", false),
             [weak = weak()](Result<rpc::Json> response) {
                 if (const auto self = weak.lock()) {
                     {
                         LockGuard lock(self->mutex);
                         if (response && self->state != DebugState::Ended)
                             self->disconnectConfirmed = true;
                         if (!response && self->state != DebugState::Ended) {
                             const auto detail =
                                 std::format("Debugger disconnect was not confirmed: {}", response.error().message());
                             if (self->fatalError)
                                 self->fatalError = Error(self->fatalError->code(),
                                                          std::format("{}; {}", self->fatalError->message(), detail));
                             else
                                 self->fatalError = Error(ErrorCode::IoError, detail);
                         }
                     }
                     if (IDebugAdapterClient* const client = self->adapter.load()) {
                         client->close();
                     }
                 }
             });
    }

    // A failed start: remembers the first error and ends the adapter.
    void fatal(Error error) {
        {
            LockGuard lock(mutex);
            if (!fatalError) {
                fatalError = std::move(error);
            }
        }
        disconnect(targetMode == DebugTargetMode::Launch);
    }

    void closed(const Error& why) {
        DebugOutcome result;
        {
            LockGuard lock(mutex);
            if (state == DebugState::Ended) {
                return;
            }
            state = DebugState::Ended;
            result.exitCode = exitCode;
            result.stopped = stopRequested;
            result.disconnectConfirmed = disconnectConfirmed;
            if (fatalError) {
                result.error = fatalError;
            } else if (!stopRequested && !terminatedSeen && !disconnectSent) {
                result.error =
                    Error(ErrorCode::IoError, std::format("The debug adapter ended unexpectedly: {}", why.message()));
            }
            outcome = result;
            stop.reset();
            currentFrame.reset();
        }
        emit([&] {
            if (callbacks.onStateChanged) {
                callbacks.onStateChanged(DebugState::Ended);
            }
            if (callbacks.onEnded) {
                callbacks.onEnded(result);
            }
        });
    }

    // --- Events ---

    void event(std::string_view name, const rpc::Json& body) {
        if (name == "initialized") {
            configure();
        } else if (name == "stopped") {
            stopped(body);
        } else if (name == "continued") {
            continued();
        } else if (name == "output") {
            const dap::OutputEvent output = dap::parseOutputEvent(body);
            if (output.category != "telemetry" && !output.output.empty()) {
                emit([&] {
                    if (callbacks.onOutput) {
                        callbacks.onOutput(output.category, output.output);
                    }
                });
            }
        } else if (name == "exited") {
            const int code = static_cast<int>(body.integerAt("exitCode").value_or(0));
            {
                LockGuard lock(mutex);
                exitCode = code;
            }
            emit([&] {
                if (callbacks.onExited) {
                    callbacks.onExited(code);
                }
            });
        } else if (name == "terminated") {
            {
                LockGuard lock(mutex);
                terminatedSeen = true;
            }
            emit([&] {
                if (callbacks.onTerminated)
                    callbacks.onTerminated();
            });
            disconnect(false);
        } else if (name == "breakpoint") {
            breakpointEvent(body);
        } else if (name == "capabilities") {
            capabilitiesChanged(body);
        } else if (name == "process") {
            const dap::ProcessEvent process = dap::parseProcessEvent(body);
            LockGuard lock(mutex);
            processId =
                process.systemProcessId > 0 ? std::optional<std::int64_t>(process.systemProcessId) : std::nullopt;
        }
    }
};

DebugSession::DebugSession(std::shared_ptr<State> state) noexcept : m_state(std::move(state)) {}

std::unique_ptr<DebugSession> DebugSession::start(AdapterFactory factory, rpc::Json arguments,
                                                  std::map<std::string, std::vector<BreakpointLine>> breakpoints,
                                                  DebugSessionCallbacks callbacks, bool breakOnAbort,
                                                  DebugTargetMode targetMode) {
    return start(std::move(factory), std::move(arguments), std::move(breakpoints), std::move(callbacks), breakOnAbort,
                 "lldb-dap", targetMode);
}

std::unique_ptr<DebugSession> DebugSession::start(AdapterFactory factory, rpc::Json launchArguments,
                                                  std::map<std::string, std::vector<BreakpointLine>> breakpoints,
                                                  DebugSessionCallbacks callbacks, bool breakOnAbort,
                                                  std::string adapterId, DebugTargetMode targetMode) {
    return start(
        std::move(factory), std::move(launchArguments), std::move(breakpoints), std::move(callbacks),
        DebugSessionOptions{.breakOnAbort = breakOnAbort, .adapterId = std::move(adapterId), .targetMode = targetMode});
}

std::unique_ptr<DebugSession> DebugSession::start(AdapterFactory factory, rpc::Json launchArguments,
                                                  std::map<std::string, std::vector<BreakpointLine>> breakpoints,
                                                  DebugSessionCallbacks callbacks, DebugSessionOptions options) {
    auto state = std::make_shared<State>(std::move(launchArguments), std::move(breakpoints), std::move(callbacks),
                                         std::move(options));
    std::unique_ptr<DebugSession> session(new DebugSession(state));
    const std::weak_ptr<State> weak = state;
    DapHandlers handlers{
        .onEvent =
            [weak](std::string_view event, const rpc::Json& body) {
                if (const auto self = weak.lock()) {
                    self->afterStartup([self, event = std::string(event), body] { self->event(event, body); });
                }
            },
        .onReverseRequest = {},
        .onProtocolError = {},
        .onClosed =
            [weak](const Error& why) {
                if (const auto self = weak.lock()) {
                    self->afterStartup([self, why] { self->closed(why); });
                }
            },
    };
    auto client = factory ? factory(std::move(handlers))
                          : Result<std::unique_ptr<IDebugAdapterClient>>(
                                fail(ErrorCode::InvalidArgument, "no debug adapter is configured"));
    if (!client) {
        {
            LockGuard lock(state->mutex);
            state->fatalError = client.error();
            state->disconnectSent = true;
        }
        state->closed(client.error());
        return session;
    }
    {
        LockGuard lock(state->mutex);
        state->client = std::move(*client);
        state->adapter.store(state->client.get());
    }
    state->sendInitialize();
    return session;
}

DebugSession::~DebugSession() {
    m_state->silent.store(true);
    std::unique_ptr<IDebugAdapterClient> client;
    {
        LockGuard lock(m_state->mutex);
        client = std::move(m_state->client);
    }
    if (client) {
        client->close();
        client.reset(); // Joins the client's threads; their handlers see `silent`.
    }
    m_state->adapter.store(nullptr);
}

DebugState DebugSession::state() const {
    LockGuard lock(m_state->mutex);
    return m_state->state;
}

dap::Capabilities DebugSession::capabilities() const {
    LockGuard lock(m_state->mutex);
    return m_state->caps;
}

std::optional<StopInfo> DebugSession::stopInfo() const {
    LockGuard lock(m_state->mutex);
    return m_state->stop;
}

std::vector<DebugThread> DebugSession::threads() const {
    LockGuard lock(m_state->mutex);
    return m_state->threads;
}

std::optional<std::int64_t> DebugSession::selectedThread() const {
    LockGuard lock(m_state->mutex);
    return m_state->currentThread;
}

FrameList DebugSession::frames(std::int64_t threadId) const {
    LockGuard lock(m_state->mutex);
    const auto it = m_state->frames.find(threadId);
    return it == m_state->frames.end() ? FrameList{} : it->second;
}

std::optional<DebugFrame> DebugSession::selectedFrame() const {
    LockGuard lock(m_state->mutex);
    return m_state->currentFrame;
}

std::optional<DebugOutcome> DebugSession::outcome() const {
    LockGuard lock(m_state->mutex);
    return m_state->outcome;
}

std::optional<std::int64_t> DebugSession::processId() const {
    LockGuard lock(m_state->mutex);
    return m_state->processId;
}

void DebugSession::setFunctionBreakpoints(std::vector<FunctionBreakpointLine> breakpoints) {
    {
        LockGuard lock(m_state->mutex);
        m_state->functionBreakpoints = std::move(breakpoints);
    }
    m_state->sendFunctionBreakpoints(false);
}

void DebugSession::setExceptionFilters(std::vector<std::string> filters) {
    bool throwChanged = false;
    {
        LockGuard lock(m_state->mutex);
        const bool before = m_state->translatesThrow();
        m_state->exceptionFilters = std::move(filters);
        throwChanged = before != m_state->translatesThrow();
    }
    m_state->sendExceptionFilters(false);
    if (throwChanged) {
        m_state->sendFunctionBreakpoints(false);
    }
}

std::vector<std::string> DebugSession::exceptionFilters() const {
    LockGuard lock(m_state->mutex);
    return m_state->exceptionFilters;
}

bool DebugSession::msvcExceptions() const noexcept { return m_state->msvcExceptions; }

void DebugSession::setThrowTypes(std::vector<std::string> types) {
    {
        LockGuard lock(m_state->mutex);
        m_state->throwTypes = std::move(types);
    }
    m_state->noteIgnoredThrowTypes();
}

std::vector<std::string> DebugSession::throwTypes() const {
    LockGuard lock(m_state->mutex);
    return m_state->throwTypes;
}

Result<void> DebugSession::restart() {
    {
        LockGuard lock(m_state->mutex);
        if (m_state->state == DebugState::Ended || m_state->stopRequested) {
            return fail(ErrorCode::Busy, "the debug session has ended");
        }
        if (m_state->targetMode == DebugTargetMode::Attach) {
            return fail(ErrorCode::Unsupported, "an attached program cannot be restarted");
        }
        if (!m_state->caps.supportsRestartRequest) {
            return fail(ErrorCode::Unsupported, "the debug adapter does not support restart");
        }
        if (!m_state->configured || m_state->state == DebugState::Starting) {
            return fail(ErrorCode::Busy, "the debug session is still starting");
        }
        m_state->state = DebugState::Running;
        ++m_state->generation;
        m_state->stop.reset();
        m_state->frames.clear();
        m_state->loading.clear();
        m_state->currentFrame.reset();
        m_state->exitCode.reset();
        m_state->processId.reset();
        m_state->pauseRequested = false;
        m_state->pauseOrigin.reset();
    }
    m_state->emit([&] {
        if (m_state->callbacks.onStateChanged)
            m_state->callbacks.onStateChanged(DebugState::Running);
    });
    const std::weak_ptr<State> weak = m_state;
    m_state->send("restart", rpc::Json::object().set("arguments", m_state->launchArguments),
                  [weak](Result<rpc::Json> answer) {
                      const auto self = weak.lock();
                      if (!self || answer) {
                          return;
                      }
                      const std::string message = std::format("restart failed: {}\n", answer.error().message());
                      self->emit([&] {
                          if (self->callbacks.onOutput)
                              self->callbacks.onOutput("important", message);
                      });
                  });
    return {};
}

void DebugSession::setBreakpoints(std::string path, std::vector<BreakpointLine> lines) {
    {
        LockGuard lock(m_state->mutex);
        if (lines.empty()) {
            // An empty list is still sent once, so the adapter drops the source's breakpoints.
            m_state->breakpoints[path].clear();
        } else {
            m_state->breakpoints[path] = std::move(lines);
        }
    }
    m_state->sendBreakpoints(path);
}

Result<void> DebugSession::continueRun() { return m_state->resume("continue"); }
Result<void> DebugSession::next() { return m_state->resume("next"); }
Result<void> DebugSession::stepIn() { return m_state->resume("stepIn"); }
Result<void> DebugSession::stepOut() { return m_state->resume("stepOut"); }

Result<void> DebugSession::pause() {
    std::optional<std::int64_t> thread;
    std::uint64_t gen = 0;
    {
        LockGuard lock(m_state->mutex);
        if (m_state->state != DebugState::Running) {
            return fail(ErrorCode::Busy, "pause needs a running program");
        }
        thread = m_state->currentThread;
        if (!thread && !m_state->threads.empty()) {
            thread = m_state->threads.front().id;
        }
        gen = m_state->generation;
        m_state->pauseRequested = true; // Header comment, "Pause".
    }
    if (!thread) {
        m_state->requestThreads(gen, std::string("pause"));
        return {};
    }
    m_state->send("pause", rpc::Json::object().set("threadId", *thread), [](Result<rpc::Json>) {});
    return {};
}

Result<void> DebugSession::runToLine(std::string path, std::int64_t line) {
    {
        LockGuard lock(m_state->mutex);
        if (m_state->state != DebugState::Stopped) {
            return fail(ErrorCode::Busy, "Run to Cursor needs a paused program");
        }
        m_state->runTo = std::pair{path, line};
    }
    m_state->sendBreakpoints(path);
    return m_state->resume("continue");
}

Result<void> DebugSession::selectThread(std::int64_t threadId) {
    std::optional<DebugFrame> chosen;
    std::uint64_t gen = 0;
    bool load = false;
    {
        LockGuard lock(m_state->mutex);
        if (std::ranges::none_of(m_state->threads, [&](const DebugThread& t) { return t.id == threadId; })) {
            return fail(ErrorCode::NotFound, std::format("thread {} does not exist", threadId));
        }
        m_state->currentThread = threadId;
        m_state->currentFrame.reset();
        gen = m_state->generation;
        const auto it = m_state->frames.find(threadId);
        if (it != m_state->frames.end() && !it->second.frames.empty()) {
            m_state->currentFrame = it->second.frames.front();
            chosen = m_state->currentFrame;
        } else {
            load = true;
        }
    }
    if (chosen) {
        m_state->emit([&] {
            if (m_state->callbacks.onFrameSelected) {
                m_state->callbacks.onFrameSelected(threadId, *chosen);
            }
        });
    } else if (load) {
        m_state->loadFrames(threadId, gen, true);
    }
    return {};
}

Result<void> DebugSession::selectFrame(std::int64_t threadId, std::int64_t frameId) {
    DebugFrame chosen;
    {
        LockGuard lock(m_state->mutex);
        const auto it = m_state->frames.find(threadId);
        if (it == m_state->frames.end()) {
            return fail(ErrorCode::NotFound, std::format("thread {} has no frames loaded", threadId));
        }
        const auto frame = std::ranges::find(it->second.frames, frameId, &DebugFrame::id);
        if (frame == it->second.frames.end()) {
            return fail(ErrorCode::NotFound, std::format("frame {} is not loaded", frameId));
        }
        m_state->currentThread = threadId;
        m_state->currentFrame = *frame;
        chosen = *frame;
    }
    m_state->emit([&] {
        if (m_state->callbacks.onFrameSelected) {
            m_state->callbacks.onFrameSelected(threadId, chosen);
        }
    });
    return {};
}

void DebugSession::loadMoreFrames(std::int64_t threadId) {
    std::uint64_t gen = 0;
    {
        LockGuard lock(m_state->mutex);
        gen = m_state->generation;
    }
    m_state->loadFrames(threadId, gen, false);
}

std::int64_t DebugSession::source(std::int64_t sourceReference, SourceDone done) {
    std::uint64_t generation = 0;
    bool stopped = false;
    {
        LockGuard lock(m_state->mutex);
        stopped = sourceReference > 0 && m_state->state == DebugState::Stopped;
        generation = m_state->generation;
    }
    if (!stopped) {
        done(fail(ErrorCode::Busy, "loaded source needs a stopped session"));
        return 0;
    }
    const std::weak_ptr<State> weak = m_state;
    return m_state->send("source", rpc::Json::object().set("sourceReference", sourceReference),
                         [weak, generation, done = std::move(done)](Result<rpc::Json> body) {
                             const auto state = weak.lock();
                             if (!state) {
                                 done(fail(ErrorCode::Cancelled, "debug session ended"));
                                 return;
                             }
                             bool current = false;
                             {
                                 LockGuard lock(state->mutex);
                                 current = state->generation == generation && state->state == DebugState::Stopped;
                             }
                             if (!current) {
                                 done(fail(ErrorCode::Cancelled, "loaded source reference expired"));
                                 return;
                             }
                             if (!body) {
                                 done(std::unexpected(std::move(body.error())));
                                 return;
                             }
                             const rpc::Json* content = body->find("content");
                             if (!content || !content->asString()) {
                                 done(fail(ErrorCode::Corrupt, "adapter returned invalid loaded source"));
                                 return;
                             }
                             done(*content->asString());
                         });
}

std::int64_t DebugSession::scopes(std::int64_t frameId, ScopesDone done) {
    return m_state->send("scopes", rpc::Json::object().set("frameId", frameId),
                         [done = std::move(done)](Result<rpc::Json> body) {
                             if (!body) {
                                 done(std::unexpected(std::move(body.error())));
                                 return;
                             }
                             ScopeList list;
                             if (const rpc::Json* scopes = body->find("scopes"); scopes != nullptr) {
                                 for (const rpc::Json& scope : scopes->items()) {
                                     list.push_back(dap::parseScope(scope));
                                 }
                             }
                             done(std::move(list));
                         });
}

std::int64_t DebugSession::variables(std::int64_t variablesReference, std::int64_t start, std::int64_t count,
                                     VariablesDone done) {
    rpc::Json arguments = rpc::Json::object().set("variablesReference", variablesReference);
    if (count > 0) {
        arguments.set("start", start).set("count", count);
    }
    return m_state->send("variables", std::move(arguments), [done = std::move(done)](Result<rpc::Json> body) {
        if (!body) {
            done(std::unexpected(std::move(body.error())));
            return;
        }
        VariableList list;
        if (const rpc::Json* variables = body->find("variables"); variables != nullptr) {
            for (const rpc::Json& variable : variables->items()) {
                list.push_back(dap::parseVariable(variable));
            }
        }
        done(std::move(list));
    });
}

std::int64_t DebugSession::evaluate(std::string expression, std::optional<std::int64_t> frameId, std::string context,
                                    EvaluateDone done) {
    rpc::Json arguments = rpc::Json::object().set("expression", expression).set("context", context);
    if (frameId) {
        arguments.set("frameId", *frameId);
    }
    return m_state->send("evaluate", std::move(arguments),
                         [done = std::move(done), expression](Result<rpc::Json> body) {
                             if (!body) {
                                 done(std::unexpected(std::move(body.error())));
                                 return;
                             }
                             done(dap::parseEvaluateResult(*body, expression));
                         });
}

std::int64_t DebugSession::setVariable(std::int64_t variablesReference, std::string_view name, std::string_view value,
                                       SetVariableDone done) {
    std::string varName(name);
    return m_state->send("setVariable", dap::setVariableArguments(variablesReference, name, value),
                         [done = std::move(done), varName = std::move(varName)](Result<rpc::Json> body) {
                             if (!done) {
                                 return;
                             }
                             if (!body) {
                                 done(std::unexpected(std::move(body.error())));
                                 return;
                             }
                             done(dap::parseSetVariableResponse(*body, varName));
                         });
}

std::int64_t DebugSession::setExceptionBreakpoints(const std::vector<std::string>& filters,
                                                   ExceptionBreakpointsDone done) {
    return m_state->send("setExceptionBreakpoints", dap::setExceptionBreakpointsArguments(filters),
                         [done = std::move(done)](Result<rpc::Json> body) {
                             if (!done) {
                                 return;
                             }
                             if (!body) {
                                 done(std::unexpected(std::move(body.error())));
                                 return;
                             }
                             done({});
                         });
}

std::int64_t DebugSession::disassemble(std::string_view memoryReference, std::int64_t instructionCount,
                                       DisassembleDone done, std::int64_t instructionOffset) {
    return m_state->send("disassemble", dap::disassembleArguments(memoryReference, instructionCount, instructionOffset),
                         [done = std::move(done)](Result<rpc::Json> body) {
                             if (!done) {
                                 return;
                             }
                             if (!body) {
                                 done(std::unexpected(std::move(body.error())));
                                 return;
                             }
                             done(dap::parseDisassembleResponse(*body));
                         });
}

std::int64_t DebugSession::readMemory(std::string_view memoryReference, std::int64_t count, ReadMemoryDone done,
                                      std::int64_t offset) {
    return m_state->send("readMemory", dap::readMemoryArguments(memoryReference, count, offset),
                         [done = std::move(done)](Result<rpc::Json> body) {
                             if (!done) {
                                 return;
                             }
                             if (!body) {
                                 done(std::unexpected(std::move(body.error())));
                                 return;
                             }
                             done(dap::parseReadMemoryResponse(*body));
                         });
}

std::int64_t DebugSession::gotoLine(std::int64_t threadId, std::string_view path, std::int64_t line,
                                    GotoLineDone done) {
    const std::weak_ptr<State> weak = m_state;
    return m_state->send("gotoTargets", dap::gotoTargetsArguments(path, line),
                         [weak, threadId, done = std::move(done)](Result<rpc::Json> body) mutable {
                             const auto state = weak.lock();
                             if (!state) {
                                 if (done) {
                                     done(fail(ErrorCode::Cancelled, "debug session ended"));
                                 }
                                 return;
                             }
                             if (!body) {
                                 if (done) {
                                     done(std::unexpected(std::move(body.error())));
                                 }
                                 return;
                             }
                             const std::vector<dap::GotoTarget> targets = dap::parseGotoTargetsResponse(*body);
                             if (targets.empty()) {
                                 if (done) {
                                     done(fail(ErrorCode::NotFound, "No executable goto target found on that line"));
                                 }
                                 return;
                             }
                             state->send("goto", dap::gotoArguments(threadId, targets.front().id),
                                         [done = std::move(done)](Result<rpc::Json> gotoRes) {
                                             if (!done) {
                                                 return;
                                             }
                                             if (!gotoRes) {
                                                 done(std::unexpected(std::move(gotoRes.error())));
                                                 return;
                                             }
                                             done({});
                                         });
                         });
}

std::int64_t DebugSession::stopPlay(StopPlayDone done) {
    return m_state->send("frost.stopPlay", rpc::Json::object(), std::move(done));
}

void DebugSession::cancel(std::int64_t seq) {
    if (IDebugAdapterClient* const client = m_state->adapter.load(); client != nullptr && seq != 0) {
        client->cancel(seq);
    }
}

void DebugSession::stop() {
    m_state->withStartup([&] {
        {
            LockGuard lock(m_state->mutex);
            if (m_state->state == DebugState::Ended)
                return;
            m_state->stopRequested = true;
        }
        m_state->disconnect(m_state->targetMode == DebugTargetMode::Launch);
    });
}

void DebugSession::kill() {
    m_state->withStartup([&] {
        {
            LockGuard lock(m_state->mutex);
            m_state->stopRequested = true;
        }
        if (IDebugAdapterClient* const client = m_state->adapter.load())
            client->close();
    });
}

} // namespace frost::debug
