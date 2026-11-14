// SPDX-License-Identifier: Apache-2.0
//
// DebugSession (see frost/debug/debug_session.hpp): source breakpoints, function breakpoints, and
// exception filter / MSVC throw-type synchronization.

#include "frost/core/log.hpp"

#include "debug_session_internal.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace frost::debug {

// --- Breakpoints ---

void DebugSession::State::sendBreakpoints(const std::string& path) {
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

void DebugSession::State::breakpointsAnswered(const std::vector<BreakpointLine>& lines, Result<rpc::Json> body) {
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

void DebugSession::State::breakpointEvent(const rpc::Json& body) {
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

// One setFunctionBreakpoints request with the caller's breakpoints, then abort and
// _CxxThrowException when the session needs them; `initial` skips an empty first request.
void DebugSession::State::sendFunctionBreakpoints(bool initial) {
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
        list.push_back(
            dap::FunctionBreakpoint{.name = line.name, .condition = line.condition, .hitCondition = line.hitCondition});
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

void DebugSession::State::functionBreakpointsAnswered(const std::vector<FunctionBreakpointLine>& user, bool withAbort,
                                                      bool withThrow, Result<rpc::Json> body) {
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
void DebugSession::State::noteIgnoredThrowTypes() {
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
void DebugSession::State::sendExceptionFilters(bool initial) {
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

void DebugSession::setBreakpoints(const std::string& path, std::vector<BreakpointLine> lines) {
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

} // namespace frost::debug
