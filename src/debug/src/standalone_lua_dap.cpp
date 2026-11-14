// SPDX-License-Identifier: Apache-2.0
#include "frost/debug/standalone_lua_dap.hpp"

#include "frost/io/path.hpp"
namespace frost::debug {
Result<rpc::Json> standaloneLuaLaunchArguments(const StandaloneLuaTarget& target) {
    const auto invalidPath = [](const std::string& path) {
        return !io::isAbsolutePath(path) || path.find('\0') != std::string::npos;
    };
    if (invalidPath(target.interpreter) || invalidPath(target.script) || invalidPath(target.workingDirectory))
        return fail(ErrorCode::InvalidArgument, "Lua interpreter, script and working directory must be absolute");
    rpc::Json arg0 = rpc::Json::array(), arg = rpc::Json::array(), env = rpc::Json::object();
    for (const auto& value : target.interpreterArguments) {
        if (value.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "NUL in interpreter argument");
        arg0.push(value);
    }
    for (const auto& value : target.arguments) {
        if (value.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "NUL in script argument");
        arg.push(value);
    }
    for (const auto& [name, value] : target.environment) {
        if (name.empty() || name.find_first_of("=\0", 0, 2) != std::string::npos ||
            value.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "invalid Lua environment entry");
        env.set(name, value);
    }
    rpc::Json output = rpc::Json::array();
    output.push("print");
    output.push("io.write");
    output.push("stdout");
    output.push("stderr");
    return rpc::Json::object()
        .set("type", "lua")
        .set("request", "launch")
        .set("luaexe", target.interpreter)
        .set("luaVersion", "lua54")
        .set("program", target.script)
        .set("arg0", std::move(arg0))
        .set("arg", std::move(arg))
        .set("cwd", target.workingDirectory)
        .set("env", std::move(env))
        .set("console", "internalConsole")
        .set("sourceCoding", "utf8")
        .set("useWSL", false)
        .set("stopOnEntry", target.stopOnEntry)
        .set("outputCapture", std::move(output));
}
AdapterFactory standaloneLuaAdapterFactory(io::IProcessLauncher& launcher, std::string executable) {
    return [&launcher,
            executable = std::move(executable)](DapHandlers handlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
        if (!io::isAbsolutePath(executable) || executable.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "configure an absolute installed lua-debug executable");
        io::ProcessSpec spec;
        spec.executable = executable;
        spec.workingDirectory = std::string(io::parentPath(executable));
        spec.stdinMode = io::Stdio::Pipe;
        spec.stdoutMode = io::Stdio::Pipe;
        spec.stderrMode = io::Stdio::Pipe;
        spec.killOnClose = true;
        auto child = launcher.launch(spec);
        if (!child)
            return std::unexpected(child.error().withContext("starting standalone lua-debug"));
        DapProcessOutput output;
        output.privateOutput = handlers.privateSession;
        return makeDapClient(processTransport(std::move(*child), std::move(output)), std::move(handlers));
    };
}
} // namespace frost::debug
