// SPDX-License-Identifier: Apache-2.0
//
// The DAP 1.71 readers and writers of dap_types.hpp.

#include "frost/debug/dap_types.hpp"

#include <format>

namespace frost::debug::dap {
namespace {

[[nodiscard]] bool flag(const rpc::Json& json, std::string_view key) { return json.boolAt(key).value_or(false); }

[[nodiscard]] std::int64_t integer(const rpc::Json& json, std::string_view key) {
    return json.integerAt(key).value_or(0);
}

[[nodiscard]] std::string text(const rpc::Json& json, std::string_view key) { return std::string(json.stringAt(key)); }

// `format` with each {name} replaced by variables[name]; unknown names stay as written.
[[nodiscard]] std::string fillFormat(std::string_view format, const rpc::Json* variables) {
    std::string out;
    while (!format.empty()) {
        const std::size_t open = format.find('{');
        if (open == std::string_view::npos) {
            out += format;
            break;
        }
        const std::size_t close = format.find('}', open);
        if (close == std::string_view::npos) {
            out += format;
            break;
        }
        out += format.substr(0, open);
        const std::string_view name = format.substr(open + 1, close - open - 1);
        const rpc::Json* value = variables != nullptr ? variables->find(name) : nullptr;
        if (value != nullptr && value->isString()) {
            out += *value->asString();
        } else {
            out += format.substr(open, close - open + 1);
        }
        format.remove_prefix(close + 1);
    }
    return out;
}

} // namespace

Capabilities parseCapabilities(const rpc::Json& body) {
    return Capabilities{
        .supportsConfigurationDoneRequest = flag(body, "supportsConfigurationDoneRequest"),
        .supportsFunctionBreakpoints = flag(body, "supportsFunctionBreakpoints"),
        .supportsCancelRequest = flag(body, "supportsCancelRequest"),
        .supportsTerminateRequest = flag(body, "supportsTerminateRequest"),
        .supportsEvaluateForHovers = flag(body, "supportsEvaluateForHovers"),
        .supportsDelayedStackTraceLoading = flag(body, "supportsDelayedStackTraceLoading"),
        .supportsExceptionInfoRequest = flag(body, "supportsExceptionInfoRequest"),
        .supportsSteppingGranularity = flag(body, "supportsSteppingGranularity"),
    };
}

Source parseSource(const rpc::Json& json) {
    return Source{.name = text(json, "name"),
                  .path = text(json, "path"),
                  .sourceReference = integer(json, "sourceReference"),
                  .presentationHint = text(json, "presentationHint")};
}

Breakpoint parseBreakpoint(const rpc::Json& json) {
    Breakpoint breakpoint;
    breakpoint.id = json.integerAt("id");
    breakpoint.verified = flag(json, "verified");
    breakpoint.message = text(json, "message");
    breakpoint.line = json.integerAt("line");
    breakpoint.reason = text(json, "reason");
    return breakpoint;
}

Thread parseThread(const rpc::Json& json) { return Thread{.id = integer(json, "id"), .name = text(json, "name")}; }

StackFrame parseStackFrame(const rpc::Json& json) {
    StackFrame frame;
    frame.id = integer(json, "id");
    frame.name = text(json, "name");
    if (const rpc::Json* source = json.find("source"); source != nullptr && source->isObject()) {
        frame.source = parseSource(*source);
    }
    frame.line = integer(json, "line");
    frame.column = integer(json, "column");
    if (const rpc::Json* module = json.find("moduleId"); module != nullptr && module->isString()) {
        frame.moduleName = *module->asString();
    }
    frame.instructionPointerReference = text(json, "instructionPointerReference");
    frame.presentationHint = text(json, "presentationHint");
    return frame;
}

Scope parseScope(const rpc::Json& json) {
    return Scope{.name = text(json, "name"),
                 .presentationHint = text(json, "presentationHint"),
                 .variablesReference = integer(json, "variablesReference"),
                 .namedVariables = integer(json, "namedVariables"),
                 .indexedVariables = integer(json, "indexedVariables"),
                 .expensive = flag(json, "expensive")};
}

Variable parseVariable(const rpc::Json& json) {
    return Variable{.name = text(json, "name"),
                    .value = text(json, "value"),
                    .type = text(json, "type"),
                    .evaluateName = text(json, "evaluateName"),
                    .variablesReference = integer(json, "variablesReference"),
                    .namedVariables = integer(json, "namedVariables"),
                    .indexedVariables = integer(json, "indexedVariables")};
}

Variable parseEvaluateResult(const rpc::Json& body, std::string_view expression) {
    return Variable{.name = std::string(expression),
                    .value = text(body, "result"),
                    .type = text(body, "type"),
                    .evaluateName = std::string(expression),
                    .variablesReference = integer(body, "variablesReference"),
                    .namedVariables = integer(body, "namedVariables"),
                    .indexedVariables = integer(body, "indexedVariables")};
}

StoppedEvent parseStoppedEvent(const rpc::Json& body) {
    StoppedEvent event;
    event.reason = text(body, "reason");
    event.description = text(body, "description");
    event.threadId = body.integerAt("threadId");
    event.text = text(body, "text");
    event.allThreadsStopped = flag(body, "allThreadsStopped");
    if (const rpc::Json* ids = body.find("hitBreakpointIds"); ids != nullptr && ids->isArray()) {
        for (const rpc::Json& id : ids->items()) {
            if (const auto value = id.asInteger()) {
                event.hitBreakpointIds.push_back(*value);
            }
        }
    }
    return event;
}

OutputEvent parseOutputEvent(const rpc::Json& body) {
    OutputEvent event;
    if (const std::string category = text(body, "category"); !category.empty()) {
        event.category = category;
    }
    event.output = text(body, "output");
    return event;
}

BreakpointEvent parseBreakpointEvent(const rpc::Json& body) {
    BreakpointEvent event;
    event.reason = text(body, "reason");
    if (const rpc::Json* breakpoint = body.find("breakpoint"); breakpoint != nullptr) {
        event.breakpoint = parseBreakpoint(*breakpoint);
    }
    return event;
}

rpc::Json initializeArguments(std::string_view adapterId) {
    return rpc::Json::object()
        .set("clientID", "frostide")
        .set("clientName", "FrostIDE")
        .set("adapterID", adapterId)
        .set("locale", "en-US")
        .set("linesStartAt1", true)
        .set("columnsStartAt1", true)
        .set("pathFormat", "path")
        .set("supportsVariableType", true)
        .set("supportsVariablePaging", true)
        .set("supportsRunInTerminalRequest", false)
        .set("supportsMemoryReferences", false)
        .set("supportsProgressReporting", false)
        .set("supportsInvalidatedEvent", false)
        .set("supportsStartDebuggingRequest", false);
}

rpc::Json setBreakpointsArguments(std::string_view path, const std::vector<std::int64_t>& lines) {
    rpc::Json breakpoints = rpc::Json::array();
    for (const std::int64_t line : lines) {
        breakpoints.push(rpc::Json::object().set("line", line));
    }
    return rpc::Json::object()
        .set("source", rpc::Json::object().set("path", path))
        .set("breakpoints", std::move(breakpoints))
        .set("sourceModified", false);
}

Error responseError(std::string_view command, std::string_view message, const rpc::Json& body) {
    ErrorCode code = ErrorCode::IoError;
    if (message == "cancelled") {
        code = ErrorCode::Cancelled;
    } else if (message == "notStopped") {
        code = ErrorCode::Busy;
    }
    std::string detail;
    if (const rpc::Json* error = body.find("error"); error != nullptr && error->isObject()) {
        detail = fillFormat(error->stringAt("format"), error->find("variables"));
    }
    if (detail.empty()) {
        detail = message.empty() ? std::string("the request failed") : std::string(message);
    }
    return Error(code, std::format("{}: {}", command, detail));
}

} // namespace frost::debug::dap
