// SPDX-License-Identifier: Apache-2.0
#include "frost/debug/gdb_dap.hpp"
#include "gdb_transport.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include <utility>
#include <charconv>
#include <chrono>

namespace frost::debug {
namespace {
AdapterFactory trustGdbFactory(AdapterFactory factory, const project::TrustStore& trust, DebugTarget target) {
    return [factory = std::move(factory), &trust, target = std::move(target)](DapHandlers handlers) mutable {
        auto original = std::move(handlers.beforeWrite);
        handlers.beforeWrite = [&trust, target, original = std::move(original)](std::string_view command) -> Result<void> {
            if (command == "launch" || command == "attach")
                if (auto allowed = validateDebugTargetTrust(trust, target); !allowed) return allowed;
            return original ? original(command) : Result<void>{};
        };
        return factory(std::move(handlers));
    };
}
}
Result<void> validateGdbDapVersion(std::string_view banner) {
    auto line = banner.substr(0, banner.find('\n'));
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (!line.starts_with("GNU gdb ")) return fail(ErrorCode::Unsupported, "Unrecognized GNU GDB version banner");
    const auto token = line.substr(line.find_last_of(' ') + 1);
    int major = 0;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), major);
    if (error != std::errc{} || end == token.data() || (end != token.data() + token.size() && *end != '.') || major < 16)
        return fail(ErrorCode::Unsupported, "GNU GDB >=16 is required for DAP");
    return {};
}
Result<std::string> findGdbDap(io::IFileSystem& fs, std::string configured) {
    if (!io::isAbsolutePath(configured))
        return fail(ErrorCode::InvalidArgument, "Set an absolute tools.gdb.path to a user-installed GDB >=16 with Python/DAP support; PATH is not searched");
    auto exists = fs.exists(configured);
    if (!exists) return std::unexpected(std::move(exists.error()));
    if (!*exists) return fail(ErrorCode::NotFound, "Configured GDB executable is missing: " + configured);
    return configured;
}

io::ProcessSpec gdbDapProcessSpec(std::string executable) {
    io::ProcessSpec spec;
    spec.executable = std::move(executable);
    // Disable all init files and auto-loading of repository/object-file Python and .gdbinit.
    // -iex executes before the inferior is loaded. Keep DAP stdout free of banners.
    spec.arguments = {"--quiet", "--nx", "--interpreter=dap", "--init-eval-command=set auto-load off",
                      "--init-eval-command=set debuginfod enabled off"};
    spec.stdinMode = io::Stdio::Pipe;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    spec.environment.emplace_back("DEBUGINFOD_URLS", std::nullopt);
    spec.environment.emplace_back("PYTHONPATH", std::nullopt);
    spec.environment.emplace_back("PYTHONHOME", std::nullopt);
    return spec;
}

AdapterFactory gdbDapAdapterFactory(io::IProcessLauncher& launcher, std::string executable, DapProcessOutput output) {
    return [&launcher, executable = std::move(executable), output = std::move(output)](DapHandlers handlers)
        -> Result<std::unique_ptr<IDebugAdapterClient>> {
        return makeDapClient(gdbDapTransport(launcher, executable, output), std::move(handlers));
    };
}

Result<rpc::Json> gdbDapLaunchArguments(const DebugTarget& target,
                                      const std::map<std::string, std::string>& inheritedEnvironment) {
    if (target.stopOnEntry) return fail(ErrorCode::Unsupported, "GDB 16 DAP does not support stopping at the first instruction");
    if (target.sourceMap)
        return fail(ErrorCode::Unsupported, "GDB DAP source mapping is not supported; use matching local source paths");
    rpc::Json args = rpc::Json::array();
    for (const auto& argument : target.arguments) args.push(rpc::Json(argument));
    auto environment = target.inheritEnvironment ? inheritedEnvironment : std::map<std::string, std::string>{};
    for (const auto& [key, value] : target.environment) environment[key] = value;
    rpc::Json env = rpc::Json::object();
    for (const auto& [key, value] : environment) env.set(key, value);
    auto launch = rpc::Json::object().set("program", target.program).set("args", std::move(args))
        .set("env", std::move(env));
    auto cwd = target.workingDirectory.empty() ? std::string(io::parentPath(target.program)) : target.workingDirectory;
    if (!cwd.empty()) launch.set("cwd", cwd);
    return launch;
}

Result<std::unique_ptr<DebugSession>> startGdbDebugSession(
    const project::TrustStore& trust, const DebugTarget& target, AdapterFactory factory,
    const std::map<std::string, std::string>& inheritedEnvironment,
    std::map<std::string, std::vector<BreakpointLine>> breakpoints, DebugSessionCallbacks callbacks,
    std::optional<NativeDebugPrivacy> privacy,
    std::vector<FunctionBreakpointLine> functionBreakpoints, std::vector<std::string> exceptionFilters) {
    if (auto allowed = validateDebugTargetTrust(trust, target); !allowed)
        return std::unexpected(std::move(allowed.error()));
    auto arguments = gdbDapLaunchArguments(target, inheritedEnvironment);
    if (!arguments) return std::unexpected(std::move(arguments.error()));
    DebugSessionOptions options;
    options.adapterId = "gdb";
    options.functionBreakpoints = std::move(functionBreakpoints);
    options.exceptionFilters = std::move(exceptionFilters);
    if (privacy) {
        if (!privacy->launchAdmission) return fail(ErrorCode::InvalidArgument, "A private native launch requires final admission");
        options.privateSession = true;
        options.launchAdmission = std::move(privacy->launchAdmission);
    }
    return DebugSession::start(trustGdbFactory(std::move(factory), trust, target), std::move(*arguments), std::move(breakpoints),
                               std::move(callbacks), std::move(options));
}

Result<std::unique_ptr<DebugSession>> attachGdbDebugSession(
    const project::TrustStore& trust, const DebugTarget& target, std::int64_t pid, AdapterFactory factory,
    std::map<std::string, std::vector<BreakpointLine>> breakpoints, DebugSessionCallbacks callbacks,
    std::vector<FunctionBreakpointLine> functionBreakpoints, std::vector<std::string> exceptionFilters) {
    if (pid <= 0) return fail(ErrorCode::InvalidArgument, "GDB attach requires a positive process ID");
    if (target.stopOnEntry) return fail(ErrorCode::Unsupported, "GDB 16 DAP does not support stopping at the first instruction");
    if (target.sourceMap) return fail(ErrorCode::Unsupported, "GDB DAP source mapping is not supported");
    if (auto allowed = validateDebugTargetTrust(trust, target); !allowed)
        return std::unexpected(std::move(allowed.error()));
    DebugSessionOptions options;
    options.adapterId = "gdb";
    options.functionBreakpoints = std::move(functionBreakpoints);
    options.exceptionFilters = std::move(exceptionFilters);
    options.targetMode = DebugTargetMode::Attach;
    options.attachHint = ptraceAttachHint(io::realFileSystem());
    return DebugSession::start(trustGdbFactory(std::move(factory), trust, target), rpc::Json::object().set("program", target.program).set("pid", pid),
                               std::move(breakpoints), std::move(callbacks), std::move(options));
}
} // namespace frost::debug
