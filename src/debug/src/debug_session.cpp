// SPDX-License-Identifier: Apache-2.0
//
// DebugSession (see frost/debug/debug_session.hpp): the launch sequence, the stop handling with lazy
// threads and frames, breakpoints per source, stepping, and the end of the session.

#include "frost/debug/debug_session.hpp"

#include "frost/core/log.hpp"
#include "frost/core/mutex.hpp"

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

bool StopInfo::isCrash() const noexcept { return reason == "exception" || reason == "signal" || reason == "abort"; }

std::string StopInfo::summary() const {
    const std::string& detail = !description.empty() ? description : text;
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

namespace {

// The caller id of runToLine's temporary breakpoint (never a caller's id: those start at 1).
constexpr std::uint64_t kTemporaryBreakpoint = 0;

DebugFrame toFrame(const dap::StackFrame& frame) {
    DebugFrame out;
    out.id = frame.id;
    out.name = frame.name;
    if (frame.source) {
        out.path = frame.source->path;
        out.sourceName = frame.source->name;
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

// The frame a stop selects (header comment, "States").
std::optional<DebugFrame> pickFrame(const std::vector<DebugFrame>& frames, bool crash) {
    if (frames.empty()) {
        return std::nullopt;
    }
    if (crash) {
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
          DebugSessionCallbacks sessionCallbacks, bool catchAbort)
        : callbacks(std::move(sessionCallbacks)), launchArguments(std::move(arguments)), breakpoints(std::move(lines)),
          breakOnAbort(catchAbort) {}

    const DebugSessionCallbacks callbacks;
    const rpc::Json launchArguments;
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
    bool terminatedSeen FROST_GUARDED_BY(mutex) = false;
    std::optional<StopInfo> stop FROST_GUARDED_BY(mutex);
    std::vector<DebugThread> threads FROST_GUARDED_BY(mutex);
    std::optional<std::int64_t> currentThread FROST_GUARDED_BY(mutex);
    std::map<std::int64_t, FrameList> frames FROST_GUARDED_BY(mutex);
    std::set<std::int64_t> loading FROST_GUARDED_BY(mutex);
    std::optional<DebugFrame> currentFrame FROST_GUARDED_BY(mutex);
    std::uint64_t generation FROST_GUARDED_BY(mutex) = 0; // Bumped by each stop and resume.
    std::map<std::string, std::vector<BreakpointLine>> breakpoints FROST_GUARDED_BY(mutex);
    std::map<std::int64_t, std::uint64_t> adapterIds FROST_GUARDED_BY(mutex); // Adapter id -> caller id.
    std::optional<std::pair<std::string, std::int64_t>> runTo FROST_GUARDED_BY(mutex);
    const bool breakOnAbort;
    std::optional<std::int64_t> abortBreakpointId FROST_GUARDED_BY(mutex);
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
        send("initialize", dap::initializeArguments(), [weak = weak()](Result<rpc::Json> body) {
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
            send("launch", launchArguments, [weak = weak()](Result<rpc::Json> answer) {
                if (const auto self = weak.lock(); self && !answer) {
                    self->fatal(Error(answer.error().code(),
                                      std::format("The program could not be started: {}", answer.error().message())));
                }
            });
        });
    }

    // The initialized event: the breakpoints, then configurationDone.
    void configure() {
        withStartup([&] {
            std::vector<std::string> paths;
            bool configurationDone = false;
            bool setAbortBreakpoint = false;
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
                setAbortBreakpoint = breakOnAbort && caps.supportsFunctionBreakpoints;
            }
            for (const std::string& path : paths) {
                sendBreakpoints(path);
            }
            if (setAbortBreakpoint) {
                rpc::Json args = rpc::Json::object().set("breakpoints",
                                                         rpc::Json::array({rpc::Json::object().set("name", "abort")}));
                send("setFunctionBreakpoints", std::move(args), [weak = weak()](Result<rpc::Json> body) {
                    const auto self = weak.lock();
                    if (!self || !body)
                        return;
                    const rpc::Json* list = body->find("breakpoints");
                    if (!list || list->items().empty())
                        return;
                    const auto breakpoint = dap::parseBreakpoint(list->items().front());
                    LockGuard lock(self->mutex);
                    self->abortBreakpointId = breakpoint.id;
                });
            }
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
                        self->fatal(std::move(answer.error()));
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
        std::vector<std::int64_t> numbers;
        numbers.reserve(lines.size());
        for (const BreakpointLine& line : lines) {
            numbers.push_back(line.line);
        }
        send("setBreakpoints", dap::setBreakpointsArguments(path, numbers),
             [weak = weak(), lines](Result<rpc::Json> body) {
                 if (const auto self = weak.lock()) {
                     self->breakpointsAnswered(lines, std::move(body));
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

    // --- Stops ---

    void stopped(const rpc::Json& body) {
        const dap::StoppedEvent event = dap::parseStoppedEvent(body);
        StopInfo info{.reason = event.reason,
                      .description = event.description,
                      .text = event.text,
                      .threadId = event.threadId,
                      .allThreadsStopped = event.allThreadsStopped,
                      .hitBreakpointIds = event.hitBreakpointIds};
        std::uint64_t gen = 0;
        std::optional<std::string> restore;
        {
            LockGuard lock(mutex);
            if (state == DebugState::Ended) {
                return;
            }
            if (abortBreakpointId &&
                std::ranges::find(info.hitBreakpointIds, *abortBreakpointId) != info.hitBreakpointIds.end()) {
                info.reason = "abort";
                info.description = "Program called abort()";
            }
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
    void requestThreads(std::uint64_t gen, std::optional<std::string> then) {
        send("threads", rpc::Json(), [weak = weak(), gen, then](Result<rpc::Json> body) {
            const auto self = weak.lock();
            if (!self) {
                return;
            }
            if (!body) {
                log::warn("debug", "threads failed: {}", body.error().message());
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
                currentFrame = pickFrame(list.frames, stop && stop->isCrash());
                chosen = currentFrame;
            }
        }
        emit([&] {
            if (callbacks.onFrames) {
                callbacks.onFrames(threadId, snapshot);
            }
            if (chosen && callbacks.onFrameSelected) {
                callbacks.onFrameSelected(threadId, *chosen);
            }
        });
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
             [weak = weak()](Result<rpc::Json>) {
                 if (const auto self = weak.lock()) {
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
        disconnect(true);
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
            disconnect(false);
        } else if (name == "breakpoint") {
            breakpointEvent(body);
        }
    }
};

DebugSession::DebugSession(std::shared_ptr<State> state) noexcept : m_state(std::move(state)) {}

std::unique_ptr<DebugSession> DebugSession::start(AdapterFactory factory, rpc::Json launchArguments,
                                                  std::map<std::string, std::vector<BreakpointLine>> breakpoints,
                                                  DebugSessionCallbacks callbacks, bool breakOnAbort) {
    auto state =
        std::make_shared<State>(std::move(launchArguments), std::move(breakpoints), std::move(callbacks), breakOnAbort);
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
        m_state->disconnect(true);
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
