// SPDX-License-Identifier: Apache-2.0
// Construct an explicit-host CoreCLR adapter launch request.

#include "frost/debug/coreclr_dap.hpp"

#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"

namespace frost::debug {

Result<rpc::Json> coreClrLaunchArguments(const CoreClrTarget& target) {
    const auto invalidPath = [](const std::string& path) {
        return !io::isAbsolutePath(path) || path.find('\0') != std::string::npos;
    };
    if (invalidPath(target.dotnet) || invalidPath(target.assembly) || invalidPath(target.workingDirectory)) {
        return fail(ErrorCode::InvalidArgument, "CoreCLR host, assembly and working directory must be absolute paths");
    }
    rpc::Json args = rpc::Json::array();
    args.push(target.assembly);
    for (const auto& argument : target.arguments) {
        if (argument.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "CoreCLR argument contains NUL");
        args.push(argument);
    }
    rpc::Json environment = rpc::Json::object();
    for (const auto& [name, value] : target.environment) {
        if (name.empty() || name.find('=') != std::string::npos || name.find('\0') != std::string::npos ||
            value.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "invalid CoreCLR environment entry");
        environment.set(name, value);
    }
    // Supplying the DLL as program causes this adapter to search PATH for a dotnet executable.
    return rpc::Json::object()
        .set("type", "coreclr")
        .set("request", "launch")
        .set("program", target.dotnet)
        .set("args", std::move(args))
        .set("cwd", target.workingDirectory)
        .set("env", std::move(environment))
        .set("stopAtEntry", false);
}

AdapterFactory coreClrAdapterFactory(io::IProcessLauncher& launcher, std::string executable) {
    return [&launcher,
            executable = std::move(executable)](DapHandlers handlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
        if (!io::isAbsolutePath(executable) || executable.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "CoreCLR adapter path must be absolute");
        io::ProcessSpec spec;
        spec.executable = executable;
        spec.arguments = {"--interpreter=vscode"};
        spec.stdinMode = io::Stdio::Pipe;
        spec.stdoutMode = io::Stdio::Pipe;
        spec.stderrMode = io::Stdio::Pipe;
        spec.killOnClose = true;
        auto process = launcher.launch(spec);
        if (!process)
            return std::unexpected(std::move(process.error()).withContext("while starting the CoreCLR adapter"));
        return makeDapClient(processTransport(std::move(*process)), std::move(handlers));
    };
}

Result<std::unique_ptr<DebugSession>>
startCoreClrSession(const project::TrustStore& trust, const CoreClrTarget& target,
                    const std::vector<std::string>& trustRoots, AdapterFactory factory,
                    std::map<std::string, std::vector<BreakpointLine>> breakpoints, DebugSessionCallbacks callbacks) {
    auto launch = coreClrLaunchArguments(target);
    if (!launch)
        return std::unexpected(std::move(launch.error()));
    if (trustRoots.empty())
        return fail(ErrorCode::InvalidArgument, "managed debugging requires project trust roots");
    for (const auto& root : trustRoots) {
        if (!io::isAbsolutePath(root) || root.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "managed project trust root must be absolute");
        if (!trust.isTrusted(root))
            return fail(ErrorCode::Untrusted, "managed project is not trusted");
    }
    if (!trust.isTrusted(io::parentPath(target.assembly)))
        return fail(ErrorCode::Untrusted, "managed assembly directory is not trusted");
    return DebugSession::start(std::move(factory), std::move(*launch), std::move(breakpoints), std::move(callbacks),
                               false, "coreclr");
}

} // namespace frost::debug
