// SPDX-License-Identifier: Apache-2.0
//
// The MSBuild driver: the command line and environment for a request, and startMsBuild (see
// frost/build/msbuild_driver.hpp).

#include "frost/build/msbuild_driver.hpp"

#include "frost/io/path.hpp"

#include <atomic>
#include <format>
#include <optional>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#endif

namespace frost::build {

namespace {

constexpr io::PathStyle kWindows = io::PathStyle::Windows;

#ifdef _WIN32
// Builds started by this process, for unique PDB server endpoints.
constinit std::atomic<unsigned> g_buildCounter{0};
#endif

std::string_view verbosityName(MsBuildVerbosity verbosity) noexcept {
    switch (verbosity) {
    case MsBuildVerbosity::Quiet: return "quiet";
    case MsBuildVerbosity::Minimal: return "minimal";
    case MsBuildVerbosity::Normal: return "normal";
    case MsBuildVerbosity::Detailed: return "detailed";
    case MsBuildVerbosity::Diagnostic: return "diagnostic";
    }
    return "minimal";
}

// MSBuild property names: a letter or '_', then letters, digits, '_' or '-'.
bool isPropertyName(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        const bool digit = (c >= '0' && c <= '9') || c == '-';
        if (!letter && (i == 0 || !digit)) {
            return false;
        }
    }
    return true;
}

Result<std::string> absolute(std::string_view path, std::string_view what) {
    auto normalized = io::normalizePath(path, kWindows);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()).withContext(std::string(what)));
    }
    return normalized;
}

std::string property(std::string_view name, std::string_view value) {
    return std::format("-p:{}={}", name, escapeMsBuildValue(value));
}

// The user's first Windows UI language ("en-US"), or "en-US" when it cannot be read.
std::string userUiLanguage() {
#ifdef _WIN32
    ULONG count = 0;
    ULONG size = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) != FALSE && size > 1) {
        std::wstring buffer(size, L'\0');
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &size) != FALSE) {
            const std::wstring_view first(buffer.c_str()); // The list is NUL-separated.
            if (auto utf8 = io::fromWide(first); utf8 && !utf8->empty()) {
                return std::move(*utf8);
            }
        }
    }
#endif
    return "en-US";
}

} // namespace

std::string_view toString(BuildAction action) noexcept {
    switch (action) {
    case BuildAction::Build: return "Build";
    case BuildAction::Rebuild: return "Rebuild";
    case BuildAction::Clean: return "Clean";
    }
    return "Build";
}

std::string solutionTargetName(std::string_view name) {
    std::string target(name);
    for (char& c : target) {
        if (std::string_view("%$@;.()'").find(c) != std::string_view::npos) {
            c = '_';
        }
    }
    return target;
}

std::string escapeMsBuildValue(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char c : value) {
        switch (c) {
        case '%': escaped += "%25"; break;
        case ';': escaped += "%3B"; break;
        case ',': escaped += "%2C"; break;
        default: escaped.push_back(c); break;
        }
    }
    return escaped;
}

Result<io::ProcessSpec> msbuildProcessSpec(const MsBuildRequest& request) {
    auto msbuild = absolute(request.msbuildPath, "MSBuild.exe path");
    if (!msbuild) {
        return std::unexpected(std::move(msbuild.error()));
    }
    if (!request.projectName.empty() && !request.projectPath.empty()) {
        return fail(ErrorCode::InvalidArgument, "a build names a project of the solution or a project file, not both");
    }
    if (request.solutionPath.empty() && request.projectPath.empty()) {
        return fail(ErrorCode::InvalidArgument, "a build needs a solution or a project file");
    }
    if (request.configuration.empty() || request.platform.empty()) {
        return fail(ErrorCode::InvalidArgument, "a build needs a configuration and a platform");
    }
    std::optional<std::string> solution;
    if (!request.solutionPath.empty()) {
        auto normalized = absolute(request.solutionPath, "solution path");
        if (!normalized) {
            return std::unexpected(std::move(normalized.error()));
        }
        solution = std::move(*normalized);
    }
    std::optional<std::string> project;
    if (!request.projectPath.empty()) {
        auto normalized = absolute(request.projectPath, "project path");
        if (!normalized) {
            return std::unexpected(std::move(normalized.error()));
        }
        project = std::move(*normalized);
    }
    const std::string& file = project ? *project : *solution;

    std::string target(toString(request.action));
    if (!request.projectName.empty()) {
        const std::string name = solutionTargetName(request.projectName);
        target = request.action == BuildAction::Build ? name : std::format("{}:{}", name, target);
    }

    io::ProcessSpec spec;
    spec.executable = std::move(*msbuild);
    spec.arguments = {file, "-t:" + target, property("Configuration", request.configuration),
                      property("Platform", request.platform)};
    if (project && solution) {
        // What Visual Studio passes when it builds a project of a solution.
        const std::string_view directory = io::parentPath(*solution, kWindows);
        const std::string_view fileName = io::fileName(*solution, kWindows);
        const std::size_t dot = fileName.rfind('.');
        const std::string_view stem = dot == std::string_view::npos ? fileName : fileName.substr(0, dot);
        const std::string_view extension = dot == std::string_view::npos ? std::string_view() : fileName.substr(dot);
        std::string solutionDir(directory);
        if (!solutionDir.ends_with('\\')) {
            solutionDir.push_back('\\');
        }
        spec.arguments.push_back(property("SolutionDir", solutionDir));
        spec.arguments.push_back(property("SolutionPath", *solution));
        spec.arguments.push_back(property("SolutionName", stem));
        spec.arguments.push_back(property("SolutionFileName", fileName));
        spec.arguments.push_back(property("SolutionExt", extension));
    }
    for (const auto& [name, value] : request.properties) {
        if (!isPropertyName(name)) {
            return fail(ErrorCode::InvalidArgument, std::format("'{}' is not an MSBuild property name", name));
        }
        spec.arguments.push_back(property(name, value));
    }
    spec.arguments.push_back(request.maxCpuCount == 0 ? std::string("-m") : std::format("-m:{}", request.maxCpuCount));
    spec.arguments.emplace_back("-nologo");
    spec.arguments.emplace_back("-nodeReuse:false");
    spec.arguments.push_back(std::format("-v:{}", verbosityName(request.verbosity)));
    spec.arguments.emplace_back("-clp:NoSummary;ForceNoAlign;DisableConsoleColor");

    spec.workingDirectory = std::string(io::parentPath(file, kWindows));
    spec.environment = {
        {"DOTNET_CLI_UI_LANGUAGE", request.uiLanguage.empty() ? userUiLanguage() : request.uiLanguage},
        {"MSBUILDTERMINALLOGGER", "off"},
        {"MSBUILDDISABLENODEREUSE", "1"},
        {"NoDefaultCurrentDirectoryInExePath", std::nullopt},
    };
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    return spec;
}

Result<std::unique_ptr<BuildSession>> startMsBuild(io::IProcessLauncher& launcher, const MsBuildRequest& request,
                                                   BuildCallbacks callbacks, IClock& clock) {
    if (!request.trusted) {
        return fail(ErrorCode::Untrusted, "the build was not started: the solution's folder is not trusted");
    }
#ifdef _WIN32
    auto spec = msbuildProcessSpec(request);
    if (!spec) {
        return std::unexpected(std::move(spec.error()));
    }
    spec->environment.emplace_back(
        "_MSPDBSRV_ENDPOINT_",
        std::format("FrostIDE-{}-{}", GetCurrentProcessId(), g_buildCounter.fetch_add(1, std::memory_order_relaxed)));
    return BuildSession::start(launcher, *spec, request.trusted, std::move(callbacks), clock);
#else
    static_cast<void>(launcher);
    static_cast<void>(callbacks);
    static_cast<void>(clock);
    return fail(ErrorCode::Unsupported, "MSBuild builds need Windows");
#endif
}

} // namespace frost::build
