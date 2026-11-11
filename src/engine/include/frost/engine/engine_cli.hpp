// SPDX-License-Identifier: Apache-2.0
//
// The FrostEngine command line as a tool of the IDE (FEC-1, FER-1a): `FrostEditor <command>` runs
// one automation command headless and prints one JSON object, { command, ok, result } or
// { command, ok: false, error: { code, message, data? } }, with exit code 0 (success), 1 (command
// error), 2 (usage error) or 3 (test failures). This header finds the program a configured engine
// tree built and runs commands through io::IProcessLauncher; every result the IDE shows from it is
// the engine's own output (the truth rule). A missing program is reported as such, never replaced.
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"
#include "frost/rpc/json.hpp"

#include <chrono>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace frost::engine {

/*
 * The FrostEditor command-line program of the engine checkout at `engineRoot` (absolute), built
 * with configure preset `preset`: <root>/build/<preset>/bin/FrostEditor.com on Windows (the console
 * launcher, whose output pipes work), else FrostEditor.exe; <root>/build/<preset>/bin/FrostEditor
 * elsewhere. Errors: InvalidArgument for a relative root or an empty preset; NotFound naming the
 * path when the tree has not built the program. O(1) file system queries.
 */
[[nodiscard]] Result<std::string> findEngineEditorProgram(io::IFileSystem& fs, std::string_view engineRoot,
                                                          std::string_view preset);

// One headless command for runEngineCliCommand.
struct EngineCliRequest {
    std::string command;                    // "schema.export", "entity.get", ...
    rpc::Json params = rpc::Json::object(); // Passed as --params (dumped exactly: handles stay exact).
    std::string project{};                  // --project <dir|file.frostproj> when not empty (absolute).
    std::string workingDirectory{};         // The child's current directory when not empty (absolute).
    std::chrono::milliseconds timeout = std::chrono::minutes(5);
    std::stop_token stopToken{};
};

// The engine's refusal of a command, from its error object.
struct EngineCliError {
    int code = 0;        // The engine's CommandErrorCode (-32602 invalid params, -32001 not found, ...).
    std::string message; // The engine's message.
    rpc::Json data{};    // The engine's error data, or null.
};

// What one run printed: the result, or the engine's error. Exactly one is set.
struct EngineCliOutcome {
    std::optional<rpc::Json> result;
    std::optional<EngineCliError> error;
    int exitCode = 0;
};

/*
 * Runs `<program> <command> --params <json> [--project <dir>] --log-level error` to completion and
 * parses the JSON object it prints on stdout. `program` is absolute (findEngineEditorProgram). The
 * child is contained (its process tree ends with it) and gets no stdin. Blocks the calling thread:
 * call it from a worker, never the GUI thread.
 * Result: the engine's result or its own error object (EngineCliOutcome). Errors: Timeout when the
 * run outlives request.timeout (the child is ended); Cancelled when request.stopToken is stopped
 * (an already started contained child is terminated); Corrupt when stdout is not the one JSON
 * object of the contract (the message carries the exit code and the start of stderr); the
 * launcher's errors (NotFound, PermissionDenied, ...) as they are.
 */
[[nodiscard]] Result<EngineCliOutcome> runEngineCliCommand(io::IProcessLauncher& launcher, std::string_view program,
                                                           const EngineCliRequest& request);

// runEngineCliCommand, with the engine's own error turned into an Error ("FrostEngine <command>
// failed (<code>): <message>"; -32602 InvalidArgument, -32001 NotFound, -32003 Busy, -32005
// IoError, -32601 Unsupported, others IoError). Result: the command's result object.
[[nodiscard]] Result<rpc::Json> runEngineCliResult(io::IProcessLauncher& launcher, std::string_view program,
                                                   const EngineCliRequest& request);

} // namespace frost::engine
