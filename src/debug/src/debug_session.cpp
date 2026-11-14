// SPDX-License-Identifier: Apache-2.0
//
// DebugSession (see frost/debug/debug_session.hpp): the launch and configuration sequence, event
// routing, session state queries, and session teardown.

#include "debug_session_internal.hpp"

#include <format>
#include <utility>

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

// --- The launch sequence ---

void DebugSession::State::sendInitialize() {
    send("initialize", dap::initializeArguments(adapterId), [weak = weak()](Result<rpc::Json> body) {
        if (const auto self = weak.lock()) {
            self->initialized(std::move(body));
        }
    });
}

void DebugSession::State::initialized(Result<rpc::Json> body) {
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
            if (const auto self = weak.lock(); self && answer && self->targetMode == DebugTargetMode::Attach) {
                LockGuard lock(self->mutex);
                if (!self->stopRequested && self->state != DebugState::Ended)
                    self->attachAcknowledged = true;
            }
            if (const auto self = weak.lock(); self && !answer) {
                self->fatal(self->targetMode == DebugTargetMode::Attach
                                ? self->attachFailure(answer.error())
                                : Error(answer.error().code(),
                                        std::format("The program could not be started: {}", answer.error().message())));
            }
        });
    });
}

// An attach that failed, with the platform's hint (DebugSessionOptions::attachHint).
Error DebugSession::State::attachFailure(const Error& why) const {
    std::string message = std::format("Could not attach to the program: {}", why.message());
    if (!attachHint.empty()) {
        message += "\n" + attachHint;
    }
    return Error(why.code(), std::move(message));
}

// The capabilities event: merges what changed (lldb-dap's supportsRestartRequest arrives here).
void DebugSession::State::capabilitiesChanged(const rpc::Json& body) {
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
void DebugSession::State::configure() {
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
        if (!readOnly) {
            for (const std::string& path : paths)
                sendBreakpoints(path);
            sendFunctionBreakpoints(true);
            sendExceptionFilters(true);
        }
        if (!configurationDone) {
            afterStartup([weak = weak()] {
                if (const auto self = weak.lock()) {
                    self->configurationAccepted();
                }
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
                self->configurationAccepted();
            }
        });
        // lldb-dap's first threads reply is the cached launch-time list (ADR 0049). Consume it
        // now so a later stopped event queries the actual stopped threads.
        send("threads", rpc::Json(), [](Result<rpc::Json>) {});
    });
}

void DebugSession::State::configurationAccepted() {
    {
        LockGuard lock(mutex);
        if (state == DebugState::Ended || stopRequested)
            return;
        configurationAcknowledged = true;
    }
    emit([&] {
        if (callbacks.onConfigured)
            callbacks.onConfigured();
    });
    becomeRunning();
}

// Starting -> Running (a stop that came first wins).
void DebugSession::State::becomeRunning() {
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

// --- The end ---

void DebugSession::State::disconnect(bool terminateDebuggee) {
    auto completed = std::make_shared<std::promise<Result<void>>>();
    {
        LockGuard lock(mutex);
        if (disconnectSent || state == DebugState::Ended) {
            return;
        }
        disconnectSent = true;
        disconnectResult = completed->get_future().share();
    }
    send("disconnect", rpc::Json::object().set("terminateDebuggee", terminateDebuggee).set("restart", false),
         [weak = weak(), completed](Result<rpc::Json> response) {
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
                 completed->set_value(response ? Result<void>{} : Result<void>(std::unexpected(response.error())));
                 if (IDebugAdapterClient* const client = self->adapter.load()) {
                     client->close();
                 }
             } else {
                 completed->set_value(
                     fail(ErrorCode::Cancelled, "The debugger disappeared before detach confirmation"));
             }
         });
}

// A failed start: remembers the first error and ends the adapter.
void DebugSession::State::fatal(Error error) {
    {
        LockGuard lock(mutex);
        if (stopRequested || state == DebugState::Ended)
            return; // A late startup reply cannot turn an explicit cancelled detach into group failure.
        if (!fatalError) {
            fatalError =
                privateSession ? Error(error.code(), "The private native debug operation failed") : std::move(error);
        }
    }
    disconnect(!readOnly && targetMode == DebugTargetMode::Launch);
}

void DebugSession::State::closed(const Error& why) {
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

/*
 * A module event (DAP "module"): when the program itself loads without symbols (lldb-dap's
 * symbolStatus "Symbols not found."), says so once in the console, since none of its line
 * breakpoints can bind and the program would otherwise run past them without a word. The
 * status is lldb's; the hint names the cause j2_msvc found: lldb 23.1.4 refuses a PDB whose
 * size is not a whole number of MSF blocks, which an incremental MSVC link (LinkIncremental,
 * on for Debug by default) writes when it relinks in place (ADR 0081 item 8, DBG-0).
 */
void DebugSession::State::moduleEvent(const rpc::Json& body) {
    const rpc::Json* module = body.find("module");
    if (module == nullptr || !module->isObject())
        return;
    if (module->stringAt("symbolStatus") != "Symbols not found.")
        return;
    const std::string path(module->stringAt("path"));
    if (path.empty() || !sameProgram(path, launchArguments.stringAt("program")))
        return;
    {
        LockGuard lock(mutex);
        if (symbolsWarned)
            return;
        symbolsWarned = true;
    }
    std::string note = std::format("The debugger could not read the symbols of {} (lldb: \"Symbols not "
                                   "found.\"), so its breakpoints cannot bind.",
                                   path);
#ifdef _WIN32
    note += " lldb cannot read a PDB that an incremental MSVC link (LinkIncremental, on for Debug by "
            "default) rewrote in place; a full link (LinkIncremental=false, or a Rebuild) writes one it can read.";
#endif
    note += '\n';
    emit([&] {
        if (callbacks.onOutput)
            callbacks.onOutput("important", note);
    });
}

void DebugSession::State::event(std::string_view name, const rpc::Json& body) {
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
    } else if (name == "module") {
        moduleEvent(body);
    } else if (name == "process") {
        const dap::ProcessEvent process = dap::parseProcessEvent(body);
        {
            LockGuard lock(mutex);
            processId =
                process.systemProcessId > 0 ? std::optional<std::int64_t>(process.systemProcessId) : std::nullopt;
        }
        emit([&] {
            if (callbacks.onProcess)
                callbacks.onProcess(process.systemProcessId > 0 ? process.systemProcessId : 0);
        });
    }
}

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

// NOLINTNEXTLINE(performance-unnecessary-value-param): Own the callable while its invocation can reenter the caller.
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
        .privateSession = state->privateSession,
        .beforeWrite = [weak](std::string_view command) -> Result<void> {
            const auto self = weak.lock();
            if (!self)
                return fail(ErrorCode::Cancelled, "The debug session ended before transmission");
            if (command != "launch")
                return {};
            {
                LockGuard lock(self->mutex);
                if (self->stopRequested || self->state == DebugState::Ended)
                    return fail(ErrorCode::Cancelled, "The debug launch was cancelled before transmission");
            }
            if (!self->launchAdmission)
                return self->privateSession
                           ? Result<void>(fail(ErrorCode::PermissionDenied, "Private launch admission is missing"))
                           : Result<void>{};
            if (auto admitted = self->launchAdmission(); !admitted)
                return admitted;
            {
                LockGuard lock(self->mutex);
                if (self->stopRequested || self->state == DebugState::Ended)
                    return fail(ErrorCode::Cancelled, "The debug launch was cancelled during admission");
            }
            return {};
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

DebugTargetMode DebugSession::targetMode() const noexcept { return m_state->targetMode; }
std::uint64_t DebugSession::inspectionGeneration() const {
    LockGuard lock(m_state->mutex);
    return m_state->generation;
}

bool DebugSession::readOnly() const noexcept { return m_state->readOnly; }
std::string_view DebugSession::adapterId() const noexcept { return m_state->adapterId; }
std::optional<std::uint64_t> DebugSession::sentMessageCount() const {
    LockGuard lock(m_state->mutex);
    return m_state->client ? m_state->client->sentMessageCount() : std::nullopt;
}

bool DebugSession::canTerminateDebuggee() const {
    LockGuard lock(m_state->mutex);
    return !m_state->readOnly && (m_state->state == DebugState::Running || m_state->state == DebugState::Stopped) &&
           !m_state->stopRequested && !m_state->terminateRequested && m_state->processId.has_value() &&
           (m_state->caps.supportsTerminateRequest || m_state->targetMode == DebugTargetMode::Launch);
}

Result<void> DebugSession::terminateDebuggee(std::int64_t expectedPid, TerminateDone done) {
    if (m_state->readOnly)
        return fail(ErrorCode::Unsupported, "A dump session cannot terminate a process");
    Result<void> admitted;
    m_state->withStartup([&] {
        bool terminateRequest = false;
        {
            LockGuard lock(m_state->mutex);
            if (expectedPid <= 0 || m_state->processId != expectedPid) {
                admitted = fail(ErrorCode::PermissionDenied,
                                "The observed debuggee PID no longer matches the requested process");
                return;
            }
            if ((m_state->state != DebugState::Running && m_state->state != DebugState::Stopped) ||
                m_state->stopRequested || m_state->terminateRequested) {
                admitted = fail(ErrorCode::Busy, "The debugger is not ready for an explicit termination");
                return;
            }
            terminateRequest = m_state->caps.supportsTerminateRequest;
            if (!terminateRequest && m_state->targetMode != DebugTargetMode::Launch) {
                admitted = fail(ErrorCode::Unsupported, "The attached debugger does not support DAP terminate");
                return;
            }
            m_state->terminateRequested = true;
            m_state->stopRequested = true;
            if (!terminateRequest)
                m_state->disconnectSent = true;
        }
        const auto arguments = terminateRequest
                                   ? rpc::Json::object().set("restart", false)
                                   : rpc::Json::object().set("terminateDebuggee", true).set("restart", false);
        m_state->send(
            terminateRequest ? "terminate" : "disconnect", arguments,
            [weak = m_state->weak(), terminateRequest, done = std::move(done)](Result<rpc::Json> response) mutable {
                const auto self = weak.lock();
                if (!self)
                    return;
                {
                    LockGuard lock(self->mutex);
                    if (!response) {
                        // A refused request leaves the actual stopped/running session available.
                        // Never turn a DAP refusal into closing/killing the adapter's process tree.
                        self->terminateRequested = false;
                        if (!self->ordinaryStopRequested && !self->terminatedSeen && self->state != DebugState::Ended) {
                            self->stopRequested = false;
                            if (!terminateRequest)
                                self->disconnectSent = false;
                        }
                    } else if (!terminateRequest) {
                        self->disconnectConfirmed = true;
                    }
                }
                self->emit([&] {
                    if (done)
                        done(response ? Result<void>{} : Result<void>(std::unexpected(response.error())));
                });
                if (response && !terminateRequest)
                    if (auto* client = self->adapter.load())
                        client->close();
            });
    });
    return admitted;
}

Result<void> DebugSession::restart() {
    if (m_state->privateSession)
        return fail(ErrorCode::Unsupported, "A private engine session needs a fresh authenticated launch");
    if (m_state->readOnly)
        return fail(ErrorCode::Unsupported, "A dump session cannot restart a process");
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

Result<void> DebugSession::detachForShutdown(std::chrono::milliseconds grace) {
    if (m_state->targetMode != DebugTargetMode::Attach && !m_state->readOnly)
        return fail(ErrorCode::Unsupported, "Only an attached or dump session may detach during shutdown");
    {
        LockGuard lock(m_state->mutex);
        if (m_state->disconnectConfirmed)
            return {};
        if (m_state->state == DebugState::Ended)
            return fail(ErrorCode::IoError, "The debugger ended without confirming its detach");
    }
    stop();
    std::shared_future<Result<void>> result;
    {
        LockGuard lock(m_state->mutex);
        result = m_state->disconnectResult;
    }
    if (!result.valid() || result.wait_for(grace) != std::future_status::ready)
        return fail(ErrorCode::Timeout, "The debugger detach was not confirmed before shutdown");
    return result.get();
}

void DebugSession::stop() {
    m_state->withStartup([&] {
        {
            LockGuard lock(m_state->mutex);
            if (m_state->state == DebugState::Ended)
                return;
            m_state->ordinaryStopRequested = true;
            m_state->stopRequested = true;
        }
        m_state->disconnect(!m_state->readOnly && m_state->targetMode == DebugTargetMode::Launch);
    });
}

Result<void> DebugSession::cancelAttachedPreparation() {
    if (m_state->targetMode != DebugTargetMode::Attach)
        return fail(ErrorCode::Unsupported, "Only an unfinished attached adapter may be cancelled locally");
    Result<void> result;
    m_state->withStartup([&] {
        {
            LockGuard lock(m_state->mutex);
            if (m_state->state == DebugState::Ended)
                return;
            if (m_state->attachAcknowledged && m_state->configurationAcknowledged) {
                result = fail(ErrorCode::Busy, "The attached debugger is already configured; use ordinary detach");
                return;
            }
            m_state->ordinaryStopRequested = true;
            m_state->stopRequested = true;
        }
        if (IDebugAdapterClient* const client = m_state->adapter.load())
            client->close();
    });
    return result;
}

void DebugSession::kill() {
    m_state->withStartup([&] {
        {
            LockGuard lock(m_state->mutex);
            m_state->ordinaryStopRequested = true;
            m_state->stopRequested = true;
        }
        if (IDebugAdapterClient* const client = m_state->adapter.load())
            client->close();
    });
}

} // namespace frost::debug
