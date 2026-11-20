// SPDX-License-Identifier: Apache-2.0
// Builds UnrealBuildTool commands and starts them for trusted projects.
#include "frost/build/unreal_build.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include <algorithm>
#include <cctype>
namespace frost::build {
namespace {
bool path(std::string_view value) { return io::isAbsolutePath(value) && value.find('\0') == std::string_view::npos; }
bool target(std::string_view value) {
    return !value.empty() && value.size() <= 256 &&
        std::ranges::all_of(value, [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; });
}
}
Result<io::ProcessSpec> unrealBuildSpec(const UnrealBuildRequest& request) {
    if (!path(request.tool) || !path(request.projectFile) || !request.projectFile.ends_with(".uproject"))
        return fail(ErrorCode::InvalidArgument, "Unreal tool and project must be absolute paths");
    std::string tool = request.tool;
    std::ranges::transform(tool, tool.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (tool.ends_with(".bat") || tool.ends_with(".cmd") || tool.ends_with(".sh") || tool.ends_with(".dll"))
        return fail(ErrorCode::Unsupported, "select UBT executable or dotnet host, not a shell wrapper");
    if (!request.buildToolAssembly.empty() && (!path(request.buildToolAssembly) || !request.buildToolAssembly.ends_with(".dll")))
        return fail(ErrorCode::InvalidArgument, "UBT assembly must be an absolute DLL path");
    const char* platform = nullptr;
    switch (request.platform) { case UnrealPlatform::Win64: platform = "Win64"; break; case UnrealPlatform::Linux: platform = "Linux"; break; }
    const char* configuration = nullptr;
    switch (request.configuration) {
    case UnrealConfiguration::Debug: configuration = "Debug"; break;
    case UnrealConfiguration::DebugGame: configuration = "DebugGame"; break;
    case UnrealConfiguration::Development: configuration = "Development"; break;
    case UnrealConfiguration::Shipping: configuration = "Shipping"; break;
    case UnrealConfiguration::Test: configuration = "Test"; break;
    }
    if (!platform || !configuration) return fail(ErrorCode::InvalidArgument, "invalid Unreal target platform/configuration");
    io::ProcessSpec spec;
    spec.executable = request.tool;
    spec.workingDirectory = std::string(io::parentPath(request.projectFile));
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    if (!request.buildToolAssembly.empty()) spec.arguments.push_back(request.buildToolAssembly);
    switch (request.operation) {
    case UnrealOperation::HeaderTool:
        if (!path(request.headerManifest)) return fail(ErrorCode::InvalidArgument, "UHT requires an explicit absolute manifest");
        spec.arguments.insert(spec.arguments.end(), {"-Mode=UnrealHeaderTool", request.projectFile, request.headerManifest});
        break;
    case UnrealOperation::GenerateProjectFiles:
        spec.arguments.insert(spec.arguments.end(), {"-ProjectFiles", "-Project=" + request.projectFile, "-Game", "-Engine"});
        break;
    case UnrealOperation::Build:
    case UnrealOperation::Clean:
    case UnrealOperation::GenerateCompileDatabase:
        if (!target(request.target)) return fail(ErrorCode::InvalidArgument, "select an explicit Unreal target");
        spec.arguments.insert(spec.arguments.end(), {request.target, platform, configuration, "-Project=" + request.projectFile});
        if (request.operation == UnrealOperation::Clean) spec.arguments.emplace_back("-Clean");
        if (request.operation == UnrealOperation::GenerateCompileDatabase) spec.arguments.emplace_back("-Mode=GenerateClangDatabase");
        break;
    default: return fail(ErrorCode::InvalidArgument, "unknown Unreal operation");
    }
    return spec;
}
Result<std::unique_ptr<BuildSession>> startUnrealBuild(io::IProcessLauncher& launcher, const project::TrustStore& trust,
    const UnrealBuildRequest& request, BuildCallbacks callbacks, IClock& clock) {
    auto spec = unrealBuildSpec(request);
    if (!spec) return std::unexpected(spec.error());
    if (!trust.isTrusted(io::parentPath(request.projectFile))) return fail(ErrorCode::Untrusted, "Unreal project directory is not trusted");
    BuildSessionOptions options;
    options.parser = makeCompilerOutputParser;
    return BuildSession::start(launcher, *spec, true, std::move(callbacks), clock, options);
}
} // namespace frost::build
