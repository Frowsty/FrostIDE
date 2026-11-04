// SPDX-License-Identifier: Apache-2.0
//
// lldb-dap discovery, its process spec and launch arguments, and the trust-gated session start (see
// frost/debug/lldb_dap.hpp).

#include "frost/debug/lldb_dap.hpp"

#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/project/vcxproj.hpp"

#include <algorithm>
#include <format>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef FROST_PINNED_LLDB_VERSION
#error "FROST_PINNED_LLDB_VERSION comes from src/debug/CMakeLists.txt (sdk/sdk.lock.json)"
#endif

namespace frost::debug {
namespace {

#ifdef _WIN32
constexpr std::string_view kLldbDapName = "lldb-dap.exe";
#else
constexpr std::string_view kLldbDapName = "lldb-dap";
#endif

[[nodiscard]] std::string executableDirectory() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    auto utf8 = io::fromWide(buffer);
    return utf8 ? std::string(io::parentPath(*utf8)) : std::string();
#elif defined(__linux__)
    std::string buffer(4096, '\0');
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) {
        return {};
    }
    buffer.resize(static_cast<std::size_t>(length));
    return std::string(io::parentPath(buffer));
#else
    return {};
#endif
}

} // namespace

Result<SourceMap> makeSourceMap(io::IFileSystem& fs, const project::TrustStore& trust, std::string_view from,
                                std::string_view to, const std::vector<std::string>& debugRoots) {
    auto source = io::normalizePath(from);
    auto destination = fs.canonicalPath(to);
    if (!source || !destination)
        return fail(ErrorCode::InvalidArgument, "source mapping requires absolute paths and an existing destination");
    auto destinationStatus = fs.status(*destination);
    if (!destinationStatus || destinationStatus->type != io::FileType::Directory || *source == *destination)
        return fail(ErrorCode::InvalidArgument, "source mapping destination must be a distinct existing directory");
    bool matchesRoot = false;
    for (const auto& root : debugRoots) {
        auto canonical = fs.canonicalPath(root);
        if (canonical && *canonical == *destination && trust.isTrusted(root))
            matchesRoot = true;
    }
    if (!matchesRoot || !trust.isTrusted(*destination))
        return fail(ErrorCode::Untrusted, "source mapping destination must be a trusted debug project root");
    return SourceMap(std::move(*source), std::move(*destination));
}

std::string_view pinnedLldbVersion() noexcept { return FROST_PINNED_LLDB_VERSION; }

Result<std::string> pinnedLldbDapPath(std::string_view sdkRoot) {
    auto root = io::normalizePath(sdkRoot);
    if (!root) {
        return root;
    }
    return io::joinPath(*root, std::format("lldb-{}", pinnedLldbVersion()))
        .and_then([](const std::string& folder) { return io::joinPath(folder, "bin"); })
        .and_then([](const std::string& bin) { return io::joinPath(bin, kLldbDapName); });
}

LldbDapSearch defaultLldbDapSearch(std::string configured) {
    LldbDapSearch search;
    search.configured = std::move(configured);
    if (const auto sdk = project::processEnvironmentVariable("FROST_SDK"); sdk && !sdk->empty()) {
        search.sdkRoots.push_back(*sdk);
    }
    if (const std::string exeDir = executableDirectory(); !exeDir.empty()) {
        search.sdkRoots.push_back(exeDir);
    }
#ifdef _WIN32
    search.sdkRoots.emplace_back("E:\\sdk"); // tools/bootstrap.ps1's default -SdkRoot.
#endif
    if (const auto home = project::processEnvironmentVariable("HOME"); home && !home->empty()) {
        if (auto sdk = io::joinPath(*home, "sdk")) { // tools/bootstrap-wsl.sh's root.
            search.sdkRoots.push_back(std::move(*sdk));
        }
    }
    return search;
}

Result<std::string> findLldbDap(io::IFileSystem& fs, const LldbDapSearch& search) {
    if (!search.configured.empty()) {
        auto path = io::normalizePath(search.configured);
        if (!path) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("tools.lldbDap.path must be an absolute path to lldb-dap: {}", search.configured));
        }
        auto exists = fs.exists(*path);
        if (!exists || !*exists) {
            return fail(ErrorCode::NotFound, std::format("lldb-dap was not found at {} (tools.lldbDap.path)", *path));
        }
        return path;
    }
    std::string tried;
    for (const std::string& root : search.sdkRoots) {
        auto path = pinnedLldbDapPath(root);
        if (!path) {
            continue;
        }
        if (auto exists = fs.exists(*path); exists && *exists) {
            return path;
        }
        tried += tried.empty() ? *path : ", " + *path;
    }
    return fail(
        ErrorCode::NotFound,
        std::format("lldb-dap {} is not installed (looked for {}); run tools/bootstrap to install the SDK, or set "
                    "tools.lldbDap.path",
                    pinnedLldbVersion(), tried.empty() ? std::string("no SDK folder") : tried));
}

io::ProcessSpec lldbDapProcessSpec(std::string executable) {
    io::ProcessSpec spec;
    spec.executable = std::move(executable);
    spec.arguments = {"--no-lldbinit"};
    for (const std::string_view name : kStrippedAdapterEnvironment) {
        spec.environment.emplace_back(std::string(name), std::nullopt);
    }
#ifdef _WIN32
    spec.environment.emplace_back("NoDefaultCurrentDirectoryInExePath", std::nullopt);
#endif
    spec.stdinMode = io::Stdio::Pipe;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    return spec;
}

rpc::Json lldbDapLaunchArguments(const DebugTarget& target) {
    rpc::Json args = rpc::Json::array();
    for (const std::string& argument : target.arguments) {
        args.push(argument);
    }
    rpc::Json env = rpc::Json::array();
    for (const auto& [name, value] : target.environment) {
        env.push(std::format("{}={}", name, value));
    }
    rpc::Json launch = rpc::Json::object()
                           .set("name", target.name.empty() ? std::string(io::fileName(target.program)) : target.name)
                           .set("type", "lldb-dap")
                           .set("request", "launch")
                           .set("program", target.program)
                           .set("args", std::move(args))
                           .set("env", std::move(env))
                           .set("stopOnEntry", false);
    const std::string cwd =
        target.workingDirectory.empty() ? std::string(io::parentPath(target.program)) : target.workingDirectory;
    if (!cwd.empty()) {
        launch.set("cwd", cwd);
    }
    if (target.sourceMap) {
        launch.set("sourceMap", rpc::Json::array({rpc::Json::array(
                                    {rpc::Json(target.sourceMap->from()), rpc::Json(target.sourceMap->to())})}));
    }
    if (!target.inheritEnvironment) {
        launch.set("initCommands", rpc::Json::array({rpc::Json("settings set target.inherit-env false")}));
    }
    return launch;
}

AdapterFactory lldbDapAdapterFactory(io::IProcessLauncher& launcher, std::string executable) {
    return [&launcher,
            executable = std::move(executable)](DapHandlers handlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
        auto process = launcher.launch(lldbDapProcessSpec(executable));
        if (!process) {
            return std::unexpected(std::move(process.error()).withContext("while starting lldb-dap"));
        }
        return makeDapClient(processTransport(std::move(*process)), std::move(handlers));
    };
}

namespace {
Result<void> checkDebugTrust(const project::TrustStore& trust, const DebugTarget& target) {
    if (!io::isAbsolutePath(target.program)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the program to debug is not an absolute path: {}", target.program));
    }
    std::vector<std::string> roots = target.trustRoots;
    if (roots.empty()) {
        roots.emplace_back(io::parentPath(target.program));
    }
    for (const std::string& root : roots) {
        if (!trust.isTrusted(root)) {
            return fail(ErrorCode::Untrusted,
                        std::format("{} is not trusted: debugging runs its code. Trust the folder (build the "
                                    "solution once) and debug again.",
                                    root));
        }
    }
    if (target.sourceMap &&
        (!trust.isTrusted(target.sourceMap->to()) || std::ranges::find(roots, target.sourceMap->to()) == roots.end()))
        return fail(ErrorCode::Untrusted, "source mapping destination is no longer a trusted debug root");
    return {};
}
} // namespace

Result<std::unique_ptr<DebugSession>> startDebugSession(const project::TrustStore& trust, const DebugTarget& target,
                                                        AdapterFactory factory,
                                                        std::map<std::string, std::vector<BreakpointLine>> breakpoints,
                                                        DebugSessionCallbacks callbacks) {
    if (auto allowed = checkDebugTrust(trust, target); !allowed)
        return std::unexpected(std::move(allowed.error()));
    return DebugSession::start(std::move(factory), lldbDapLaunchArguments(target), std::move(breakpoints),
                               std::move(callbacks), io::kWindowsProcessRules);
}

Result<std::unique_ptr<DebugSession>> attachDebugSession(const project::TrustStore& trust, const DebugTarget& target,
                                                         AdapterFactory factory, rpc::Json attachArguments,
                                                         std::map<std::string, std::vector<BreakpointLine>> breakpoints,
                                                         DebugSessionCallbacks callbacks, std::string adapterId) {
    if (auto allowed = checkDebugTrust(trust, target); !allowed)
        return std::unexpected(std::move(allowed.error()));
    return DebugSession::start(std::move(factory), std::move(attachArguments), std::move(breakpoints),
                               std::move(callbacks), false, std::move(adapterId), DebugTargetMode::Attach);
}

} // namespace frost::debug
