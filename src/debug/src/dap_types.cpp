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

namespace {

// Sets `target` from body[key] when the body has a boolean there (the capabilities event names only
// what changed).
void mergeFlag(bool& target, const rpc::Json& body, std::string_view key) {
    if (const std::optional<bool> value = body.boolAt(key)) {
        target = *value;
    }
}

[[nodiscard]] std::vector<ExceptionFilter> parseExceptionFilters(const rpc::Json& list) {
    std::vector<ExceptionFilter> out;
    for (const rpc::Json& item : list.items()) {
        ExceptionFilter filter{.filter = text(item, "filter"),
                               .label = text(item, "label"),
                               .description = text(item, "description"),
                               .defaultOn = flag(item, "default"),
                               .supportsCondition = flag(item, "supportsCondition")};
        if (!filter.filter.empty()) {
            if (filter.label.empty()) {
                filter.label = filter.filter;
            }
            out.push_back(std::move(filter));
        }
    }
    return out;
}

} // namespace

Capabilities parseCapabilities(const rpc::Json& body) { return mergeCapabilities(Capabilities{}, body); }

Capabilities mergeCapabilities(Capabilities caps, const rpc::Json& body) {
    // The capabilities event wraps them in "capabilities"; the initialize response is the object itself.
    const rpc::Json* inner = body.find("capabilities");
    const rpc::Json& source = inner != nullptr && inner->isObject() ? *inner : body;
    mergeFlag(caps.supportsConfigurationDoneRequest, source, "supportsConfigurationDoneRequest");
    mergeFlag(caps.supportsFunctionBreakpoints, source, "supportsFunctionBreakpoints");
    mergeFlag(caps.supportsCancelRequest, source, "supportsCancelRequest");
    mergeFlag(caps.supportsTerminateRequest, source, "supportsTerminateRequest");
    mergeFlag(caps.supportsEvaluateForHovers, source, "supportsEvaluateForHovers");
    mergeFlag(caps.supportsDelayedStackTraceLoading, source, "supportsDelayedStackTraceLoading");
    mergeFlag(caps.supportsExceptionInfoRequest, source, "supportsExceptionInfoRequest");
    mergeFlag(caps.supportsSteppingGranularity, source, "supportsSteppingGranularity");
    mergeFlag(caps.supportsConditionalBreakpoints, source, "supportsConditionalBreakpoints");
    mergeFlag(caps.supportsHitConditionalBreakpoints, source, "supportsHitConditionalBreakpoints");
    mergeFlag(caps.supportsLogPoints, source, "supportsLogPoints");
    mergeFlag(caps.supportsSetVariable, source, "supportsSetVariable");
    mergeFlag(caps.supportsRestartRequest, source, "supportsRestartRequest");
    mergeFlag(caps.supportsDisassembleRequest, source, "supportsDisassembleRequest");
    if (const rpc::Json* filters = source.find("exceptionBreakpointFilters");
        filters != nullptr && filters->isArray()) {
        caps.exceptionBreakpointFilters = parseExceptionFilters(*filters);
    }
    return caps;
}

rpc::Json setFunctionBreakpointsArguments(const std::vector<FunctionBreakpoint>& breakpoints) {
    rpc::Json list = rpc::Json::array();
    for (const FunctionBreakpoint& breakpoint : breakpoints) {
        rpc::Json item = rpc::Json::object().set("name", breakpoint.name);
        if (!breakpoint.condition.empty()) {
            item.set("condition", breakpoint.condition);
        }
        if (!breakpoint.hitCondition.empty()) {
            item.set("hitCondition", breakpoint.hitCondition);
        }
        list.push(std::move(item));
    }
    return rpc::Json::object().set("breakpoints", std::move(list));
}

ProcessEvent parseProcessEvent(const rpc::Json& body) {
    return ProcessEvent{.name = text(body, "name"),
                        .systemProcessId = integer(body, "systemProcessId"),
                        .startMethod = text(body, "startMethod")};
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

rpc::Json setBreakpointsArguments(std::string_view path, const std::vector<SourceBreakpoint>& breakpoints, bool muted) {
    rpc::Json list = rpc::Json::array();
    if (!muted) {
        for (const SourceBreakpoint& bp : breakpoints) {
            if (!bp.enabled) {
                continue;
            }
            rpc::Json entry = rpc::Json::object().set("line", bp.line);
            if (!bp.condition.empty()) {
                entry.set("condition", bp.condition);
            }
            if (!bp.hitCondition.empty()) {
                entry.set("hitCondition", bp.hitCondition);
            }
            if (!bp.logMessage.empty()) {
                entry.set("logMessage", bp.logMessage);
            }
            list.push(std::move(entry));
        }
    }
    return rpc::Json::object()
        .set("source", rpc::Json::object().set("path", path))
        .set("breakpoints", std::move(list))
        .set("sourceModified", false);
}

namespace {

std::string decodeBase64(std::string_view encoded) {
    std::string out;
    out.reserve((encoded.size() * 3) / 4);
    int val = 0;
    int valb = -8;
    for (const char ch : encoded) {
        const auto c = static_cast<unsigned char>(ch);
        int digit = -1;
        if (c >= 'A' && c <= 'Z') {
            digit = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            digit = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            digit = c - '0' + 52;
        } else if (c == '+') {
            digit = 62;
        } else if (c == '/') {
            digit = 63;
        } else if (c == '=') {
            break;
        } else {
            continue;
        }
        val = (val << 6) + digit;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

} // namespace

std::vector<DisassembledInstruction> parseDisassembleResponse(const rpc::Json& body) {
    std::vector<DisassembledInstruction> list;
    const rpc::Json* instructions = body.find("instructions");
    if (instructions == nullptr || !instructions->isArray()) {
        return list;
    }
    for (const rpc::Json& item : instructions->items()) {
        DisassembledInstruction inst;
        inst.address = text(item, "address");
        inst.instructionBytes = text(item, "instructionBytes");
        inst.instruction = text(item, "instruction");
        inst.symbol = text(item, "symbol");
        if (const rpc::Json* loc = item.find("location"); loc != nullptr && loc->isObject()) {
            inst.location = parseSource(*loc);
        }
        inst.line = integer(item, "line");
        list.push_back(std::move(inst));
    }
    return list;
}

ReadMemoryResult parseReadMemoryResponse(const rpc::Json& body) {
    ReadMemoryResult result;
    result.address = text(body, "address");
    result.unreadableBytes = integer(body, "unreadableBytes");
    result.data = text(body, "data");
    result.decodedBytes = decodeBase64(result.data);
    return result;
}

std::vector<GotoTarget> parseGotoTargetsResponse(const rpc::Json& body) {
    std::vector<GotoTarget> list;
    const rpc::Json* targets = body.find("targets");
    if (targets == nullptr || !targets->isArray()) {
        return list;
    }
    for (const rpc::Json& item : targets->items()) {
        GotoTarget target;
        target.id = integer(item, "id");
        target.label = text(item, "label");
        target.line = integer(item, "line");
        target.column = integer(item, "column");
        list.push_back(std::move(target));
    }
    return list;
}

rpc::Json setVariableArguments(std::int64_t variablesReference, std::string_view name, std::string_view value) {
    return rpc::Json::object().set("variablesReference", variablesReference).set("name", name).set("value", value);
}

Variable parseSetVariableResponse(const rpc::Json& body, std::string_view name) {
    return Variable{
        .name = std::string(name),
        .value = text(body, "value"),
        .type = text(body, "type"),
        .evaluateName = std::string(name),
        .variablesReference = integer(body, "variablesReference"),
        .namedVariables = integer(body, "namedVariables"),
        .indexedVariables = integer(body, "indexedVariables"),
    };
}

rpc::Json setExceptionBreakpointsArguments(const std::vector<std::string>& filters) {
    rpc::Json arr = rpc::Json::array();
    for (const std::string& filter : filters) {
        arr.push(rpc::Json(filter));
    }
    return rpc::Json::object().set("filters", std::move(arr));
}

rpc::Json disassembleArguments(std::string_view memoryReference, std::int64_t instructionCount,
                               std::int64_t instructionOffset) {
    return rpc::Json::object()
        .set("memoryReference", memoryReference)
        .set("instructionOffset", instructionOffset)
        .set("instructionCount", instructionCount)
        .set("resolveSymbols", true);
}

rpc::Json readMemoryArguments(std::string_view memoryReference, std::int64_t count, std::int64_t offset) {
    return rpc::Json::object().set("memoryReference", memoryReference).set("offset", offset).set("count", count);
}

rpc::Json gotoTargetsArguments(std::string_view path, std::int64_t line) {
    return rpc::Json::object().set("source", rpc::Json::object().set("path", path)).set("line", line);
}

rpc::Json gotoArguments(std::int64_t threadId, std::int64_t targetId) {
    return rpc::Json::object().set("threadId", threadId).set("targetId", targetId);
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
