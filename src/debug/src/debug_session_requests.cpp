// SPDX-License-Identifier: Apache-2.0
//
// DebugSession (see frost/debug/debug_session.hpp): stepping, thread and frame selection, and DAP
// inspection requests (source, scopes, variables, evaluate, setVariable, disassemble, readMemory,
// gotoLine).

#include "debug_session_internal.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace frost::debug {

// All opaque references below belong to one stop and selection. A delayed answer must not
// populate a newer frame or a new stop that happens to reuse the adapter's numeric IDs.
std::int64_t DebugSession::State::inspect(std::string command, rpc::Json arguments, DebugSession::InspectionDone done) {
    std::uint64_t epoch = 0;
    std::optional<std::int64_t> frame, thread;
    bool stopped = false;
    {
        LockGuard lock(mutex);
        stopped = state == DebugState::Stopped;
        epoch = generation;
        if (currentFrame) frame = currentFrame->id;
        thread = currentThread;
    }
    if (!stopped) { done(fail(ErrorCode::Busy, "Inspection needs a paused session")); return 0; }
    return send(command, std::move(arguments),
        [weak = weak(), epoch, frame, thread, done = std::move(done)](Result<rpc::Json> body) {
            auto self = weak.lock();
            bool current = false;
            if (self) {
                LockGuard lock(self->mutex);
                current = self->state == DebugState::Stopped && self->generation == epoch &&
                          self->currentThread == thread &&
                          (self->currentFrame ? std::optional(self->currentFrame->id) : std::nullopt) == frame;
            }
            if (!current) done(fail(ErrorCode::Cancelled, "Inspection reference expired"));
            else done(std::move(body));
        });
}

Result<void> DebugSession::continueRun() { return m_state->resume("continue"); }
Result<void> DebugSession::next() { return m_state->resume("next"); }
Result<void> DebugSession::stepIn() { return m_state->resume("stepIn"); }
Result<void> DebugSession::stepOut() { return m_state->resume("stepOut"); }

Result<void> DebugSession::stepInstruction(bool into) {
    if (!capabilities().supportsSteppingGranularity)
        return fail(ErrorCode::Unsupported, "Adapter does not support instruction stepping");
    return m_state->resume(into ? "stepIn" : "next", rpc::Json::object().set("granularity", "instruction"));
}

Result<void> DebugSession::stepIntoTarget(std::int64_t targetId) {
    std::uint64_t generation = 0;
    std::int64_t frame = 0;
    {
        LockGuard lock(m_state->mutex);
        if (!m_state->caps.supportsStepInTargetsRequest || m_state->readOnly)
            return fail(ErrorCode::Unsupported, "Targeted stepping is unavailable");
        if (m_state->state != DebugState::Stopped || !m_state->currentFrame ||
            m_state->stepTargetsGeneration != m_state->generation ||
            m_state->stepTargetsFrame != m_state->currentFrame->id ||
            m_state->stepTargetsThread != m_state->currentThread || !m_state->stepTargets.contains(targetId))
            return fail(ErrorCode::InvalidArgument, "Step target expired; refresh the call targets");
        generation = m_state->generation;
        frame = m_state->currentFrame->id;
    }
    return m_state->resume("stepIn", rpc::Json::object().set("targetId", targetId), generation, frame);
}

std::int64_t DebugSession::stepInTargets(InspectionDone done) {
    std::optional<std::int64_t> thread;
    std::uint64_t generation = 0;
    std::int64_t frame = 0;
    std::optional<Error> error;
    {
        LockGuard lock(m_state->mutex);
        if (!m_state->caps.supportsStepInTargetsRequest || m_state->readOnly)
            error = Error{ErrorCode::Unsupported, "Adapter does not support call targets"};
        else if (m_state->state != DebugState::Stopped || !m_state->currentFrame)
            error = Error{ErrorCode::Busy, "Call targets need a selected paused frame"};
        else {
            generation = m_state->generation;
            frame = m_state->currentFrame->id;
            thread = m_state->currentThread;
            m_state->stepTargets.clear();
        }
    }
    if (error) { done(std::unexpected(*error)); return 0; }
    return m_state->send("stepInTargets", rpc::Json::object().set("frameId", frame),
        [weak = std::weak_ptr<State>(m_state), generation, frame, thread, done = std::move(done)](Result<rpc::Json> body) {
            auto state = weak.lock();
            bool valid = false;
            if (state) {
                LockGuard lock(state->mutex);
                valid = state->state == DebugState::Stopped && state->generation == generation &&
                        state->currentFrame && state->currentFrame->id == frame && state->currentThread == thread;
                if (valid && body) {
                    state->stepTargets.clear();
                    if (const auto* targets = body->find("targets"))
                        for (const auto& target : targets->items())
                            if (auto id = target.integerAt("id")) state->stepTargets.insert(*id);
                    state->stepTargetsGeneration = generation;
                    state->stepTargetsFrame = frame;
                    state->stepTargetsThread = state->currentThread;
                }
            }
            if (!valid) done(fail(ErrorCode::Cancelled, "Call targets expired"));
            else done(std::move(body));
        });
}

std::int64_t DebugSession::requestInspection(std::string command, rpc::Json arguments, InspectionDone done) {
    const auto caps = capabilities();
    const bool supported = (command == "modules" && caps.supportsModulesRequest) ||
        ((command == "frost.setSymbolPaths" || command == "frost.reloadSymbols") && caps.frostSupportsSymbolControl) ||
        (command == "frost.setSourceMap" && caps.frostSupportsSourceMap) ||
        (command == "frost.reloadNatvis" && caps.frostSupportsNatvisReload);
    if (!supported) { done(fail(ErrorCode::Unsupported, "Adapter does not support this inspection operation")); return 0; }
    if (!arguments.isObject() || arguments.dump().size() > 65536) {
        done(fail(ErrorCode::InvalidArgument, "Inspection arguments must be a bounded object")); return 0;
    }
    if (command == "modules") return m_state->inspect(std::move(command), std::move(arguments), std::move(done));
    // Symbol paths/maps/visualizer reload change adapter inspection configuration. Their successful
    // ACK stays authoritative across frame selection, unlike frame-scoped inspection data.
    if (state() != DebugState::Stopped) { done(fail(ErrorCode::Busy, "Pause before changing debugger inspection settings")); return 0; }
    return m_state->send(std::move(command), std::move(arguments), std::move(done));
}

std::int64_t DebugSession::setThreadFrozen(std::int64_t threadId, bool frozen, InspectionDone done) {
    std::optional<Error> error;
    {
        LockGuard lock(m_state->mutex);
        if (!m_state->caps.frostSupportsThreadFreeze || m_state->readOnly)
            error = Error{ErrorCode::Unsupported, "This debugger cannot freeze or thaw threads"};
        else if (m_state->state != DebugState::Stopped)
            error = Error{ErrorCode::Busy, "Pause before changing thread freeze state"};
        else if (std::ranges::none_of(m_state->threads, [threadId](const DebugThread& thread) { return thread.id == threadId; }))
            error = Error{ErrorCode::NotFound, "Thread no longer exists"};
    }
    if (error) { done(std::unexpected(*error)); return 0; }
    return m_state->send("frost.setThreadFrozen", rpc::Json::object().set("threadId", threadId).set("frozen", frozen),
        [weak = std::weak_ptr<State>(m_state), threadId, done = std::move(done)](Result<rpc::Json> body) {
            auto self = weak.lock();
            if (!self) { done(fail(ErrorCode::Cancelled, "Debug session ended")); return; }
            if (!body) { done(std::move(body)); return; }
            if (body->integerAt("threadId") != threadId ||
                body->boolAt("operationApplied") != true || body->boolAt("commandSucceeded") != true) {
                done(fail(ErrorCode::Corrupt, "Adapter did not confirm the thread command completed")); return;
            }
            std::uint64_t epoch = 0;
            { LockGuard lock(self->mutex); epoch = self->generation; }
            self->requestThreads(epoch, std::nullopt);
            done(std::move(body));
        });
}

std::int64_t DebugSession::dataBreakpointInfo(std::int64_t reference, std::string name, InspectionDone done) {
    bool supported = false, stopped = false;
    { LockGuard lock(m_state->mutex);
      supported = m_state->caps.supportsDataBreakpoints && !m_state->readOnly;
      stopped = m_state->state == DebugState::Stopped; }
    if (!supported) { done(fail(ErrorCode::Unsupported, "Hardware data breakpoints are unavailable")); return 0; }
    if (!stopped || reference <= 0 || name.empty()) {
        done(fail(ErrorCode::InvalidArgument, "Choose a variable in a paused session")); return 0;
    }
    return m_state->inspect("dataBreakpointInfo", rpc::Json::object().set("variablesReference", reference).set("name", std::move(name)), std::move(done));
}

std::int64_t DebugSession::setDataBreakpoints(rpc::Json breakpoints, InspectionDone done) {
    bool supported = false, stopped = false;
    { LockGuard lock(m_state->mutex);
      supported = m_state->caps.supportsDataBreakpoints && !m_state->readOnly;
      stopped = m_state->state == DebugState::Stopped; }
    if (!supported) { done(fail(ErrorCode::Unsupported, "Hardware data breakpoints are unavailable")); return 0; }
    if (!stopped) { done(fail(ErrorCode::Busy, "Data breakpoint changes need a paused session")); return 0; }
    return m_state->send("setDataBreakpoints", rpc::Json::object().set("breakpoints", std::move(breakpoints)), std::move(done));
}

Result<void> DebugSession::pause() {
    if (m_state->readOnly)
        return fail(ErrorCode::Unsupported, "A dump session cannot pause a process");
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

Result<void> DebugSession::runToLine(const std::string& path, std::int64_t line) {
    if (m_state->readOnly)
        return fail(ErrorCode::Unsupported, "A dump session cannot execute a process");
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
                                     VariablesDone done, bool hexadecimal) {
    rpc::Json arguments = rpc::Json::object().set("variablesReference", variablesReference);
    if (hexadecimal && capabilities().supportsValueFormattingOptions)
        arguments.set("format", rpc::Json::object().set("hex", true));
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
                                    EvaluateDone done, bool hexadecimal) {
    rpc::Json arguments = rpc::Json::object().set("expression", expression).set("context", std::move(context));
    if (hexadecimal && capabilities().supportsValueFormattingOptions)
        arguments.set("format", rpc::Json::object().set("hex", true));
    if (frameId) {
        arguments.set("frameId", *frameId);
    }
    return m_state->send("evaluate", std::move(arguments),
                         [done = std::move(done), expression = std::move(expression)](Result<rpc::Json> body) {
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
    if (!capabilities().supportsDisassembleRequest) {
        if (done) done(fail(ErrorCode::Unsupported, "Adapter does not support disassembly"));
        return 0;
    }
    if (memoryReference.empty() || instructionCount <= 0 || instructionCount > 4096) {
        if (done) done(fail(ErrorCode::InvalidArgument, "Disassembly needs a reference and 1..4096 instructions"));
        return 0;
    }
    return m_state->inspect("disassemble", dap::disassembleArguments(memoryReference, instructionCount, instructionOffset),
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
    if (!capabilities().supportsReadMemoryRequest) {
        if (done) done(fail(ErrorCode::Unsupported, "Adapter does not support reading memory"));
        return 0;
    }
    if (memoryReference.empty() || count <= 0 || count > 65536) {
        if (done) done(fail(ErrorCode::InvalidArgument, "Memory reads need a reference and 1..65536 bytes"));
        return 0;
    }
    return m_state->inspect("readMemory", dap::readMemoryArguments(memoryReference, count, offset),
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

} // namespace frost::debug
