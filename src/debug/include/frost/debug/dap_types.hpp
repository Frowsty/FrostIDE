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
};

[[nodiscard]] Capabilities parseCapabilities(const rpc::Json& body);

// A source file: `path` for files on disk; `sourceReference` > 0 for sources the adapter serves.
struct Source {
    std::string name;
    std::string path;
    std::int64_t sourceReference = 0;
    std::string presentationHint; // "normal", "emphasize", "deemphasize".
};

[[nodiscard]] Source parseSource(const rpc::Json& json);

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

// The stopped event's body.
struct StoppedEvent {
    std::string reason; // "step", "breakpoint", "exception", "pause", "entry", "goto", "signal", ...
    std::string description;
    std::optional<std::int64_t> threadId;
    std::string text;
    bool allThreadsStopped = false;
    std::vector<std::int64_t> hitBreakpointIds;
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

/*
 * The error a failed response stands for: "cancelled" -> Cancelled, "notStopped" -> Busy, anything
 * else IoError; the text is "<command>: " plus the body's error message (its `format` with the
 * {name} variables filled in) or, without one, the response's `message`. Pure.
 */
[[nodiscard]] Error responseError(std::string_view command, std::string_view message, const rpc::Json& body);

} // namespace frost::debug::dap
