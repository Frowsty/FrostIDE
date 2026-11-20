// SPDX-License-Identifier: Apache-2.0
// Construct an explicit-host CoreCLR adapter launch request.

#include "frost/debug/coreclr_dap.hpp"

#include "frost/io/path.hpp"
#include "frost/debug/lldb_dap.hpp"
#include "frost/project/trust_store.hpp"
#include <limits>

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
    auto admission = [&trust, target, trustRoots]() -> Result<void> {
        if (trustRoots.empty())
            return fail(ErrorCode::InvalidArgument, "managed debugging requires project trust roots");
        for (const auto& root : trustRoots) {
            if (!io::isAbsolutePath(root) || root.find('\0') != std::string::npos)
                return fail(ErrorCode::InvalidArgument, "managed project trust root must be absolute");
            if (!trust.isTrusted(root))
                return fail(ErrorCode::Untrusted, "managed project is not trusted");
        }
        if (!trust.isTrusted(io::parentPath(target.assembly)) || !trust.isTrusted(target.workingDirectory))
            return fail(ErrorCode::Untrusted, "managed assembly or working directory is not trusted");
        return {};
    };
    if (auto allowed = admission(); !allowed)
        return std::unexpected(std::move(allowed.error()));
    DebugSessionOptions options;
    options.adapterId = "coreclr";
    options.launchAdmission = std::move(admission);
    return DebugSession::start(std::move(factory), std::move(*launch), std::move(breakpoints), std::move(callbacks),
                               std::move(options));
}

Result<std::unique_ptr<DebugSession>>
attachCoreClrSession(const project::TrustStore& trust, const DebugTarget& target, std::int64_t pid,
                     AdapterFactory factory, std::function<Result<void>()> processAdmission,
                     std::map<std::string, std::vector<BreakpointLine>> breakpoints, DebugSessionCallbacks callbacks,
                     std::function<void()> releaseAuthorization) {
    if (pid <= 0 || pid > std::numeric_limits<std::uint32_t>::max() || !processAdmission || !factory)
        return fail(ErrorCode::InvalidArgument, "CoreCLR attach requires a local PID, adapter and process admission");
    auto admission = [&trust, target, processAdmission = std::move(processAdmission)]() -> Result<void> {
        if (auto allowed = validateDebugTargetTrust(trust, target); !allowed)
            return allowed;
        if (!target.workingDirectory.empty() &&
            (!io::isAbsolutePath(target.workingDirectory) || !trust.isTrusted(target.workingDirectory)))
            return fail(ErrorCode::Untrusted, "CoreCLR attach working directory is not trusted");
        return processAdmission();
    };
    if (auto allowed = admission(); !allowed)
        return std::unexpected(std::move(allowed.error()));
    DebugSessionOptions options;
    options.adapterId = "coreclr";
    options.targetMode = DebugTargetMode::Attach;
    options.launchAdmission = [admission = std::move(admission), release = std::move(releaseAuthorization)] {
        auto allowed = admission();
        if (release)
            release();
        return allowed;
    };
    return DebugSession::start(std::move(factory),
                               rpc::Json::object().set("processId", pid).set("justMyCode", false),
                               std::move(breakpoints), std::move(callbacks), std::move(options));
}

} // namespace frost::debug
