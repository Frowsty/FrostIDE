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

Result<void> DebugSession::continueRun() { return m_state->resume("continue"); }
Result<void> DebugSession::next() { return m_state->resume("next"); }
Result<void> DebugSession::stepIn() { return m_state->resume("stepIn"); }
Result<void> DebugSession::stepOut() { return m_state->resume("stepOut"); }

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
    rpc::Json arguments = rpc::Json::object().set("expression", expression).set("context", std::move(context));
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

} // namespace frost::debug
