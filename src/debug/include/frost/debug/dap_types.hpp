// SPDX-License-Identifier: Apache-2.0
//
// The Debug Adapter Protocol 1.71 types the U1 debugger uses (PLAN.md M4, ADR 0050), hand-written
// from the specification (https://microsoft.github.io/debug-adapter-protocol/specification): the
// message envelope, the capabilities FrostIDE reads, sources, breakpoints, threads, stack frames,
// scopes, variables, evaluate results and the bodies of the events the session handles, each with
// a tolerant reader from frost::rpc::Json (missing or mistyped members take their defaults, as the
// specification's optional members do) and, for request arguments, a writer.
// TODO(M4): generate these from the DAP 1.71 JSON schema with frost-codegen (PLAN.md section 2,
// "DAP/LSP/MCP clients"), with each schema description as a comment, and move them to src/protocols.
#pragma once

#include "frost/core/error.hpp"
#include "frost/rpc/json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::debug::dap {

// One exception breakpoint filter an adapter offers (Capabilities.exceptionBreakpointFilters):
// lldb-dap 23 offers cpp_throw, cpp_catch, objc_throw and objc_catch.
struct ExceptionFilter {
    std::string filter;        // The id setExceptionBreakpoints takes ("cpp_throw").
    std::string label;         // "C++ Throw".
    std::string description{}; // The adapter's longer text; may be empty.
    bool defaultOn = false;    // DAP "default".
    bool supportsCondition = false;

    friend bool operator==(const ExceptionFilter&, const ExceptionFilter&) = default;
};

// The capabilities of an adapter (the initialize response's body) that FrostIDE reads.
struct Capabilities {
    bool supportsConfigurationDoneRequest = false;
    bool supportsFunctionBreakpoints = false;
    bool supportsCancelRequest = false;
    bool supportsTerminateRequest = false;
    bool supportsEvaluateForHovers = false;
    bool supportsDelayedStackTraceLoading = false;
    bool supportsExceptionInfoRequest = false;
    bool supportsSteppingGranularity = false;
    bool supportsConditionalBreakpoints = false;
    bool supportsHitConditionalBreakpoints = false;
    bool supportsLogPoints = false;
    bool supportsSetVariable = false;
    bool supportsRestartRequest = false; // lldb-dap announces it in a later capabilities event.
    bool supportsDisassembleRequest = false;
    std::vector<ExceptionFilter> exceptionBreakpointFilters{};
};

[[nodiscard]] Capabilities parseCapabilities(const rpc::Json& body);
/*
 * The capabilities event (DAP 1.71: "the adapter's capabilities changed"): `base` with every
 * member the event's body.capabilities names replaced; members it leaves out keep their value.
 * Pure.
 */
[[nodiscard]] Capabilities mergeCapabilities(Capabilities base, const rpc::Json& eventBody);

// A function breakpoint as sent with setFunctionBreakpoints (DAP FunctionBreakpoint).
struct FunctionBreakpoint {
    std::string name;
    std::string condition{};
    std::string hitCondition{};

    friend bool operator==(const FunctionBreakpoint&, const FunctionBreakpoint&) = default;
};

// {"breakpoints": [{"name": ..., "condition"?: ..., "hitCondition"?: ...}, ...]}; empty strings are
// left out.
[[nodiscard]] rpc::Json setFunctionBreakpointsArguments(const std::vector<FunctionBreakpoint>& breakpoints);

// The process event's body: the debuggee's operating-system process id (0 when the adapter does not
// know it, as lldb-dap says for a launch that failed).
struct ProcessEvent {
    std::string name;
    std::int64_t systemProcessId = 0;
    std::string startMethod; // "launch", "attach", "attachForSuspendedLaunch".
};

[[nodiscard]] ProcessEvent parseProcessEvent(const rpc::Json& body);

// A source file: `path` for files on disk; `sourceReference` > 0 for sources the adapter serves.
struct Source {
    std::string name;
    std::string path;
    std::int64_t sourceReference = 0;
    std::string presentationHint; // "normal", "emphasize", "deemphasize".

    friend bool operator==(const Source&, const Source&) = default;
};

[[nodiscard]] Source parseSource(const rpc::Json& json);

struct SourceBreakpoint {
    std::int64_t line = 0;
    bool enabled = true;
    std::string condition{};
    std::string hitCondition{};
    std::string logMessage{};

    friend bool operator==(const SourceBreakpoint&, const SourceBreakpoint&) = default;
};

// A breakpoint as the adapter reports it (setBreakpoints' answer, the breakpoint event).
struct Breakpoint {
    std::optional<std::int64_t> id;   // The adapter's id, for later breakpoint events.
    bool verified = false;            // The adapter could bind it.
    std::string message;              // Why it is not verified, or extra information.
    std::optional<std::int64_t> line; // The line it actually bound to (1-based).
    std::string reason;               // DAP 1.62: "pending" or "failed" for an unverified breakpoint.
};

[[nodiscard]] Breakpoint parseBreakpoint(const rpc::Json& json);

struct Thread {
    std::int64_t id = 0;
    std::string name;
};

[[nodiscard]] Thread parseThread(const rpc::Json& json);

struct StackFrame {
    std::int64_t id = 0;
    std::string name;
    std::optional<Source> source;
    std::int64_t line = 0;   // 1-based; 0 when the frame has no source position.
    std::int64_t column = 0; // 1-based; 0 when unknown.
    std::string moduleName;  // From moduleId when it is a string (lldb-dap's module names).
    std::string instructionPointerReference;
    std::string presentationHint; // "normal", "label", "subtle".
};

[[nodiscard]] StackFrame parseStackFrame(const rpc::Json& json);

struct Scope {
    std::string name;
    std::string presentationHint; // "arguments", "locals", "registers", ...
    std::int64_t variablesReference = 0;
    std::int64_t namedVariables = 0;
    std::int64_t indexedVariables = 0;
    bool expensive = false;
};

[[nodiscard]] Scope parseScope(const rpc::Json& json);

struct Variable {
    std::string name;
    std::string value; // As the adapter formats it (lldb-dap's default formatters).
    std::string type;
    std::string evaluateName;
    std::int64_t variablesReference = 0; // > 0: the variable has children.
    std::int64_t namedVariables = 0;
    std::int64_t indexedVariables = 0;
};

[[nodiscard]] Variable parseVariable(const rpc::Json& json);

// The evaluate response's body, as a Variable without a name (the expression is its name).
[[nodiscard]] Variable parseEvaluateResult(const rpc::Json& body, std::string_view expression);

// FrostEngine's actual Lua-hook pause identity. OS thread IDs are distinct from DAP coroutine IDs;
// only an explicitly verified native adapter mapping may correlate them.
struct ScriptPauseIdentity {
    std::uint64_t vmGeneration = 0, osThreadId = 0, stopGeneration = 0;
    friend bool operator==(const ScriptPauseIdentity&, const ScriptPauseIdentity&) = default;
};

// The stopped event's body.
struct StoppedEvent {
    std::string reason; // "step", "breakpoint", "exception", "pause", "entry", "goto", "signal", ...
    std::string description;
    std::optional<std::int64_t> threadId;
    std::string text;
    bool allThreadsStopped = false;
    std::vector<std::int64_t> hitBreakpointIds;
    std::optional<ScriptPauseIdentity> scriptPause;
};

[[nodiscard]] StoppedEvent parseStoppedEvent(const rpc::Json& body);

// The output event's body (category "console", "important", "stdout", "stderr", "telemetry", ...).
struct OutputEvent {
    std::string category = "console";
    std::string output;
};

[[nodiscard]] OutputEvent parseOutputEvent(const rpc::Json& body);

// The breakpoint event's body: reason "changed", "new" or "removed".
struct BreakpointEvent {
    std::string reason;
    Breakpoint breakpoint;
};

[[nodiscard]] BreakpointEvent parseBreakpointEvent(const rpc::Json& body);

// The arguments FrostIDE sends with initialize (clientID "frostide", lines and columns 1-based, paths
// as paths, variable types and paging on, runInTerminal off).
[[nodiscard]] rpc::Json initializeArguments(std::string_view adapterId = "lldb-dap");

// {"source": {"path": path}, "breakpoints": [{"line": l}, ...]} for setBreakpoints.
[[nodiscard]] rpc::Json setBreakpointsArguments(std::string_view path, const std::vector<std::int64_t>& lines);
[[nodiscard]] rpc::Json setBreakpointsArguments(std::string_view path, const std::vector<SourceBreakpoint>& breakpoints,
                                                bool muted = false);

struct DisassembledInstruction {
    std::string address;
    std::string instructionBytes;
    std::string instruction;
    std::string symbol;
    std::optional<Source> location;
    std::int64_t line = 0;

    friend bool operator==(const DisassembledInstruction&, const DisassembledInstruction&) = default;
};

[[nodiscard]] std::vector<DisassembledInstruction> parseDisassembleResponse(const rpc::Json& body);

struct ReadMemoryResult {
    std::string address;
    std::int64_t unreadableBytes = 0;
    std::string data;         // Raw base64 payload from DAP.
    std::string decodedBytes; // Decoded binary bytes.
};

[[nodiscard]] ReadMemoryResult parseReadMemoryResponse(const rpc::Json& body);

struct GotoTarget {
    std::int64_t id = 0;
    std::string label;
    std::int64_t line = 0;
    std::int64_t column = 0;
};

[[nodiscard]] std::vector<GotoTarget> parseGotoTargetsResponse(const rpc::Json& body);

[[nodiscard]] rpc::Json setVariableArguments(std::int64_t variablesReference, std::string_view name,
                                             std::string_view value);
[[nodiscard]] Variable parseSetVariableResponse(const rpc::Json& body, std::string_view name);
[[nodiscard]] rpc::Json setExceptionBreakpointsArguments(const std::vector<std::string>& filters);
[[nodiscard]] rpc::Json disassembleArguments(std::string_view memoryReference, std::int64_t instructionCount,
                                             std::int64_t instructionOffset = 0);
[[nodiscard]] rpc::Json readMemoryArguments(std::string_view memoryReference, std::int64_t count,
                                            std::int64_t offset = 0);
[[nodiscard]] rpc::Json gotoTargetsArguments(std::string_view path, std::int64_t line);
[[nodiscard]] rpc::Json gotoArguments(std::int64_t threadId, std::int64_t targetId);

/*
 * The error a failed response stands for: "cancelled" -> Cancelled, "notStopped" -> Busy, anything
 * else IoError; the text is "<command>: " plus the body's error message (its `format` with the
 * {name} variables filled in) or, without one, the response's `message`. Pure.
 */
[[nodiscard]] Error responseError(std::string_view command, std::string_view message, const rpc::Json& body);

} // namespace frost::debug::dap
