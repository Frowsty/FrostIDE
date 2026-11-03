// SPDX-License-Identifier: Apache-2.0
// Build SDK-project commands and read MSBuild's structured property response.

#include "frost/build/managed_project.hpp"

#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"

#include "json.hpp"

namespace frost::build {
namespace {

bool propertyValue(std::string_view value) {
    if (value.empty())
        return false;
    for (const char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
              c == '_' || c == ' '))
            return false;
    }
    return true;
}

Result<io::ProcessSpec> baseSpec(const ManagedProjectRequest& request, bool requireFramework) {
    if (!io::isAbsolutePath(request.dotnet) || !io::isAbsolutePath(request.project) ||
        request.dotnet.find('\0') != std::string::npos || request.project.find('\0') != std::string::npos ||
        !propertyValue(request.configuration) ||
        ((requireFramework || !request.targetFramework.empty()) && !propertyValue(request.targetFramework)) ||
        (!request.runtimeIdentifier.empty() && !propertyValue(request.runtimeIdentifier)))
        return fail(ErrorCode::InvalidArgument, "invalid managed project paths or configuration");
    io::ProcessSpec spec;
    spec.executable = request.dotnet;
    spec.workingDirectory = std::string(io::parentPath(request.project));
    spec.environment = {
        {"DOTNET_CLI_UI_LANGUAGE", "en-US"}, {"MSBUILDTERMINALLOGGER", "off"}, {"MSBUILDDISABLENODEREUSE", "1"}};
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    return spec;
}

Result<void> checkTrust(const project::TrustStore& trust, const ManagedProjectRequest& request,
                        const std::vector<std::string>& roots) {
    if (roots.empty())
        return fail(ErrorCode::InvalidArgument, "managed project requires trust roots");
    for (const auto& root : roots) {
        if (!io::isAbsolutePath(root) || root.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "managed project trust root must be absolute");
        if (!trust.isTrusted(root))
            return fail(ErrorCode::Untrusted, "managed project is not trusted");
    }
    if (!trust.isTrusted(io::parentPath(request.project)))
        return fail(ErrorCode::Untrusted, "managed project directory is not trusted");
    return {};
}

} // namespace

Result<io::ProcessSpec> managedProjectQuerySpec(const ManagedProjectRequest& request) {
    auto spec = baseSpec(request, false);
    if (!spec)
        return std::unexpected(std::move(spec.error()));
    spec->arguments = {"msbuild",
                       request.project,
                       "-nologo",
                       "-getProperty:TargetPath,TargetFramework,TargetFrameworks,OutputType,RuntimeIdentifier",
                       "-property:Configuration=" + request.configuration,
                       "-property:SelfContained=false"};
    if (!request.targetFramework.empty())
        spec->arguments.push_back("-property:TargetFramework=" + request.targetFramework);
    if (!request.runtimeIdentifier.empty())
        spec->arguments.push_back("-property:RuntimeIdentifier=" + request.runtimeIdentifier);
    return spec;
}

Result<io::ProcessSpec> managedProjectBuildSpec(const ManagedProjectRequest& request) {
    auto spec = baseSpec(request, true);
    if (!spec)
        return std::unexpected(std::move(spec.error()));
    spec->arguments = {"build",       request.project,         "--configuration",     request.configuration,
                       "--framework", request.targetFramework, "--no-self-contained", "--nologo"};
    if (!request.runtimeIdentifier.empty()) {
        spec->arguments.push_back("--runtime");
        spec->arguments.push_back(request.runtimeIdentifier);
    }
    return spec;
}

Result<ManagedProjectProperties> parseManagedProjectProperties(std::string_view json) {
    auto document = detail::parseJson(json);
    if (!document)
        return std::unexpected(std::move(document.error()).withContext("managed MSBuild property output"));
    const auto* properties = document->find("Properties");
    if (properties == nullptr || properties->kind != detail::JsonValue::Kind::Object)
        return fail(ErrorCode::Corrupt, "managed MSBuild output has no Properties object");
    for (const std::string_view name :
         {"TargetPath", "TargetFramework", "TargetFrameworks", "OutputType", "RuntimeIdentifier"}) {
        const auto* value = properties->find(name);
        if (value == nullptr || value->kind != detail::JsonValue::Kind::String)
            return fail(ErrorCode::Corrupt, "managed MSBuild output is missing a requested property");
    }
    return ManagedProjectProperties{.targetPath = properties->stringMember("TargetPath"),
                                    .targetFramework = properties->stringMember("TargetFramework"),
                                    .targetFrameworks = properties->stringMember("TargetFrameworks"),
                                    .outputType = properties->stringMember("OutputType"),
                                    .runtimeIdentifier = properties->stringMember("RuntimeIdentifier")};
}

Result<std::string> managedConsoleTarget(const ManagedProjectRequest& request,
                                         const ManagedProjectProperties& properties) {
    if (request.targetFramework.empty() || !propertyValue(request.targetFramework) ||
        properties.targetFramework != request.targetFramework ||
        (!request.runtimeIdentifier.empty() && properties.runtimeIdentifier != request.runtimeIdentifier))
        return fail(ErrorCode::InvalidArgument,
                    "evaluated managed target does not match the selected framework or RID");
    if (properties.outputType != "Exe")
        return fail(ErrorCode::Unsupported, "only managed console executable projects are supported");
    if (!io::isAbsolutePath(properties.targetPath) || properties.targetPath.find('\0') != std::string::npos ||
        !std::string_view(properties.targetPath).ends_with(".dll"))
        return fail(ErrorCode::InvalidArgument, "managed executable target must be an absolute DLL path");
    return properties.targetPath;
}

Result<std::unique_ptr<BuildSession>> startManagedProjectBuild(io::IProcessLauncher& launcher,
                                                               const project::TrustStore& trust,
                                                               const ManagedProjectRequest& request,
                                                               const std::vector<std::string>& trustRoots,
                                                               BuildCallbacks callbacks, IClock& clock) {
    auto spec = managedProjectBuildSpec(request);
    if (!spec)
        return std::unexpected(std::move(spec.error()));
    if (auto trusted = checkTrust(trust, request, trustRoots); !trusted)
        return std::unexpected(std::move(trusted.error()));
    return BuildSession::start(launcher, *spec, true, std::move(callbacks), clock);
}

Result<std::unique_ptr<BuildSession>> startManagedProjectQuery(io::IProcessLauncher& launcher,
                                                               const project::TrustStore& trust,
                                                               const ManagedProjectRequest& request,
                                                               const std::vector<std::string>& trustRoots,
                                                               BuildCallbacks callbacks, IClock& clock) {
    auto spec = managedProjectQuerySpec(request);
    if (!spec)
        return std::unexpected(std::move(spec.error()));
    if (auto trusted = checkTrust(trust, request, trustRoots); !trusted)
        return std::unexpected(std::move(trusted.error()));
    return BuildSession::start(launcher, *spec, true, std::move(callbacks), clock);
}

} // namespace frost::build
