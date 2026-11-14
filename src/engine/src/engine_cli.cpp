// SPDX-License-Identifier: Apache-2.0
//
// frost::engine's engine CLI runner (engine_cli.hpp): finds FrostEditor in a configured engine tree
// and runs one headless command through io::IProcessLauncher, reading the one JSON object it prints.

#include "frost/engine/engine_cli.hpp"

#include "frost/io/path.hpp"

#include <format>
#include <utility>
#include <vector>

namespace frost::engine {
namespace {

// The longest piece of stderr an error message quotes.
constexpr std::size_t kStderrExcerpt = 2000;

std::string excerpt(std::string_view text) {
    std::string shown(text.substr(0, kStderrExcerpt));
    while (!shown.empty() && (shown.back() == '\n' || shown.back() == '\r' || shown.back() == ' ')) {
        shown.pop_back();
    }
    return shown;
}

ErrorCode errorCodeFor(int engineCode) {
    switch (engineCode) {
    case -32602: return ErrorCode::InvalidArgument; // InvalidParams
    case -32601: return ErrorCode::Unsupported;     // MethodNotFound: this engine has no such command.
    case -32001: return ErrorCode::NotFound;
    case -32002: return ErrorCode::AlreadyExists;
    case -32003: return ErrorCode::Busy; // PreconditionFailed: not now (no project, unsaved changes).
    case -32004: return ErrorCode::PermissionDenied;
    default: return ErrorCode::IoError; // CommandFailed, IoError and the rest.
    }
}

} // namespace

Result<std::string> findEngineEditorProgram(io::IFileSystem& fs, std::string_view engineRoot, std::string_view preset) {
    if (!io::isAbsolutePath(engineRoot) || preset.empty()) {
        return fail(ErrorCode::InvalidArgument, "the engine root must be absolute and the preset named");
    }
    auto root = io::normalizePath(engineRoot);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    auto bin = io::joinPath(*root, std::format("build/{}/bin", preset));
    if (!bin) {
        return std::unexpected(std::move(bin.error()));
    }
#ifdef _WIN32
    const std::vector<std::string_view> names{"FrostEditor.com", "FrostEditor.exe"};
#else
    const std::vector<std::string_view> names{"FrostEditor"};
#endif
    for (const std::string_view name : names) {
        auto candidate = io::joinPath(*bin, name);
        if (!candidate) {
            return std::unexpected(std::move(candidate.error()));
        }
        auto exists = fs.exists(*candidate);
        if (!exists) {
            return std::unexpected(std::move(exists.error()));
        }
        if (*exists) {
            return std::move(*candidate);
        }
    }
    return fail(ErrorCode::NotFound,
                std::format("the engine tree has not built FrostEditor: no {} in {}", names.front(), *bin));
}

Result<EngineCliOutcome> runEngineCliCommand(io::IProcessLauncher& launcher, std::string_view program,
                                             const EngineCliRequest& request) {
    if (request.command.empty() || !request.params.isObject()) {
        return fail(ErrorCode::InvalidArgument, "an engine command needs a name and an object of parameters");
    }
    io::ProcessSpec spec;
    spec.executable = std::string(program);
    spec.arguments = {request.command, "--params", request.params.dump(), "--log-level", "error"};
    if (!request.project.empty()) {
        spec.arguments.emplace_back("--project");
        spec.arguments.push_back(request.project);
    }
    spec.workingDirectory = request.workingDirectory;
    auto run = io::runToCompletion(launcher, spec, {}, request.timeout, request.stopToken);
    if (!run) {
        return std::unexpected(std::move(run.error()));
    }
    // The contract: one JSON object on stdout, whatever the exit code.
    auto printed = rpc::parseJson(run->stdoutBytes);
    if (request.stopToken.stop_requested()) {
        return fail(ErrorCode::Cancelled, "engine CLI command was cancelled");
    }
    if (!printed || !printed->isObject() || !printed->boolAt("ok").has_value()) {
        return fail(ErrorCode::Corrupt,
                    std::format("FrostEditor {} printed no result object (exit code {}): {}", request.command,
                                run->exitCode,
                                excerpt(run->stderrBytes.empty() ? run->stdoutBytes : run->stderrBytes)));
    }
    EngineCliOutcome outcome;
    outcome.exitCode = run->exitCode;
    if (*printed->boolAt("ok")) {
        const rpc::Json* result = printed->find("result");
        if (result == nullptr) {
            return fail(ErrorCode::Corrupt, std::format("FrostEditor {} succeeded without a result", request.command));
        }
        outcome.result = *result;
        return outcome;
    }
    const rpc::Json* error = printed->find("error");
    if (error == nullptr || !error->isObject() || !error->integerAt("code")) {
        return fail(ErrorCode::Corrupt, std::format("FrostEditor {} failed without an error object", request.command));
    }
    EngineCliError refused;
    refused.code = static_cast<int>(*error->integerAt("code"));
    refused.message = std::string(error->stringAt("message"));
    if (const rpc::Json* data = error->find("data")) {
        refused.data = *data;
    }
    outcome.error = std::move(refused);
    return outcome;
}

Result<rpc::Json> runEngineCliResult(io::IProcessLauncher& launcher, std::string_view program,
                                     const EngineCliRequest& request) {
    auto outcome = runEngineCliCommand(launcher, program, request);
    if (!outcome) {
        return std::unexpected(std::move(outcome.error()));
    }
    if (outcome->error) {
        return fail(errorCodeFor(outcome->error->code), std::format("FrostEngine {} failed ({}): {}", request.command,
                                                                    outcome->error->code, outcome->error->message));
    }
    if (request.stopToken.stop_requested()) {
        return fail(ErrorCode::Cancelled, "engine CLI command was cancelled");
    }
    return std::move(*outcome->result);
}

} // namespace frost::engine
