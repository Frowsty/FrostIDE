// SPDX-License-Identifier: Apache-2.0
// Explicit DbgEng process factory and launch serialization, separate from LLDB launch policy.
#include "frost/debug/dbgeng_dap.hpp"

#include "frost/io/path.hpp"

#include <utility>

namespace frost::debug {
namespace {
bool safePath(std::string_view path, bool commandPath = false) {
    const bool localVerbatim = path.size() >= 7 && path.starts_with("\\\\?\\") && path[5] == ':' && path[6] == '\\';
    const auto separator = [](char value) { return value == '/' || value == '\\'; };
    const bool unc = path.size() >= 2 && separator(path[0]) && separator(path[1]);
    return io::isAbsolutePath(path) && (!unc || localVerbatim) && path.find('\0') == std::string_view::npos &&
           (!commandPath || path.find_first_of("\r\n\";") == std::string_view::npos);
}
} // namespace

Result<rpc::Json> dbgEngDapLaunchArguments(const DbgEngTarget& target) {
    if (target.native.sourceMap)
        return fail(ErrorCode::Unsupported, "DbgEng source mapping is not implemented in the FER-16 backend slice");
    const bool invalidProgram = target.dumpFile.empty()
                                    ? !safePath(target.native.program, true)
                                    : (!safePath(target.dumpFile) ||
                                       (!target.native.program.empty() && !safePath(target.native.program, true)));
    if (invalidProgram ||
        (!target.native.workingDirectory.empty() && !safePath(target.native.workingDirectory, true)) ||
        target.natvisFiles.size() > 64 || target.localSymbolPaths.size() > 64) {
        return fail(ErrorCode::InvalidArgument, "DbgEng target paths must be absolute and lists bounded");
    }
    auto arguments = rpc::Json::array();
    for (const auto& argument : target.native.arguments) {
        if (argument.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "DbgEng argument contains NUL");
        arguments.push(argument);
    }
    auto natvis = rpc::Json::array();
    for (const auto& path : target.natvisFiles) {
        if (!safePath(path, true))
            return fail(ErrorCode::InvalidArgument, "Natvis requires an absolute file path without debugger commands");
        natvis.push(path);
    }
    auto symbols = rpc::Json::array();
    for (const auto& path : target.localSymbolPaths) {
        if (!safePath(path, true))
            return fail(ErrorCode::InvalidArgument, "symbol path must be an absolute local directory");
        symbols.push(path);
    }
    auto environment = rpc::Json::object();
    for (const auto& [key, value] : target.native.environment) {
        if (key.empty() || key.find('=') != std::string::npos || key.find('\0') != std::string::npos ||
            value.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "invalid debuggee environment");
        environment.set(key, value);
    }
    auto result = rpc::Json::object()
                      .set("type", kDbgEngAdapterId)
                      .set("program", target.native.program)
                      .set("args", std::move(arguments))
                      .set("cwd", target.native.workingDirectory)
                      .set("env", std::move(environment))
                      .set("inheritEnvironment", target.native.inheritEnvironment)
                      .set("stopOnEntry", target.native.stopOnEntry)
                      .set("natvis", std::move(natvis))
                      .set("symbolPaths", std::move(symbols));
    if (!target.dumpFile.empty())
        result.set("dumpFile", target.dumpFile);
    return result;
}

io::ProcessSpec dbgEngDapProcessSpec(std::string executable) {
    io::ProcessSpec result;
    result.executable = std::move(executable);
    result.stdinMode = io::Stdio::Pipe;
    result.stdoutMode = io::Stdio::Pipe;
    result.stderrMode = io::Stdio::Pipe;
    for (const auto name : kStrippedAdapterEnvironment)
        result.environment.emplace_back(std::string(name), std::nullopt);
    return result;
}

AdapterFactory dbgEngDapAdapterFactory(io::IProcessLauncher& launcher, std::string executable) {
    return [&launcher,
            executable = std::move(executable)](DapHandlers handlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
        auto process = launcher.launch(dbgEngDapProcessSpec(executable));
        if (!process)
            return std::unexpected(std::move(process.error()).withContext("while starting frost-dbgeng-dap"));
        return makeDapClient(processTransport(std::move(*process)), std::move(handlers));
    };
}

} // namespace frost::debug
