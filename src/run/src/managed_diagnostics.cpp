// SPDX-License-Identifier: Apache-2.0
// Actual EventPipe tools with private outputs and continuous admitted process lifetime checks.
#include "frost/run/managed_diagnostics.hpp"

#include "frost/io/path.hpp"

#include <algorithm>
#include <thread>
namespace frost::run {
namespace {
std::string artifactName(ManagedDiagnosticKind kind) {
    switch (kind) {
    case ManagedDiagnosticKind::Trace: return "capture.nettrace";
    case ManagedDiagnosticKind::Counters: return "capture.csv";
    case ManagedDiagnosticKind::GcDump: return "capture.gcdump";
    }
    return {};
}
std::string duration(unsigned seconds) {
    const auto pad = [](unsigned value) { return (value < 10 ? "0" : "") + std::to_string(value); };
    return "00:00:" + pad(seconds / 60) + ":" + pad(seconds % 60);
}
std::string_view trim(std::string_view value) {
    const auto begin = value.find_first_not_of(" \r\n\t");
    if (begin == value.npos)
        return {};
    return value.substr(begin, value.find_last_not_of(" \r\n\t") - begin + 1);
}
} // namespace
Result<io::ProcessSpec> managedDiagnosticSpec(const ManagedDiagnosticRequest& r) {
    if (!io::isAbsolutePath(r.tool) || !io::isAbsolutePath(r.workingDirectory) ||
        !io::isAbsolutePath(r.outputDirectory) || !r.processId || r.processId > 2147483647U || r.durationSeconds < 1 ||
        r.durationSeconds > 300 || !r.admit || r.expectedVersion.empty() || r.expectedVersion.size() > 128 ||
        r.expectedVersion.find_first_of(" \r\n\t") != r.expectedVersion.npos ||
        r.expectedVersion.find('\0') != r.expectedVersion.npos || artifactName(r.kind).empty())
        return fail(ErrorCode::InvalidArgument,
                    "Diagnostics require absolute installed tool/private output, exact version, PID and admission");
    if (r.kind == ManagedDiagnosticKind::GcDump && !r.allowFullGc)
        return fail(ErrorCode::PermissionDenied,
                    "GC dump triggers a full generation2 GC and may suspend the target; explicit consent required");
    auto output = io::joinPath(r.outputDirectory, artifactName(r.kind));
    if (!output)
        return std::unexpected(output.error());
    io::ProcessSpec spec;
    spec.executable = r.tool;
    spec.workingDirectory = r.workingDirectory;
    spec.environment = r.environment;
    spec.arguments = {"collect", "--process-id", std::to_string(r.processId), "--output", *output};
    if (r.kind == ManagedDiagnosticKind::GcDump) {
        spec.arguments.insert(spec.arguments.end(), {"--timeout", std::to_string(r.durationSeconds)});
    } else {
        spec.arguments.insert(spec.arguments.end(), {"--duration", duration(r.durationSeconds)});
        if (r.kind == ManagedDiagnosticKind::Trace)
            spec.arguments.insert(spec.arguments.end(),
                                  {"--format", "NetTrace", "--profile", "dotnet-common,dotnet-sampled-thread-time"});
        else
            spec.arguments.insert(spec.arguments.end(), {"--format", "csv", "--refresh-interval", "1", "--counters",
                                                         "System.Runtime,Microsoft.AspNetCore.Hosting"});
    }
    return spec;
}
Result<ManagedDiagnosticReport> collectManagedDiagnostics(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                                          const ManagedDiagnosticRequest& r, std::stop_token stop) {
    auto spec = managedDiagnosticSpec(r);
    if (!spec)
        return std::unexpected(spec.error());
    auto tool = captureAnalysisIdentity(fs, r.tool);
    if (!tool)
        return std::unexpected(tool.error());
    const auto artifactPath = io::joinPath(r.outputDirectory, artifactName(r.kind));
    if (!artifactPath)
        return std::unexpected(artifactPath.error());
    std::stop_source operation;
    std::stop_callback cancel(stop, [&] { operation.request_stop(); });
    // Cheap live checks continue during tool launch/recording. Full file hash checks are worker-only.
    std::jthread watch([&](std::stop_token done) {
        while (!done.stop_requested() && !operation.stop_requested()) {
            auto size = fs.status(*artifactPath);
            if ((size && size->size > 256U * 1024U * 1024U) || !r.admit()) {
                operation.request_stop();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    });
    auto admitted = [&]() -> Result<void> {
        if (operation.stop_requested())
            return fail(ErrorCode::Cancelled, "Managed diagnostics cancelled or target/context changed");
        if (auto allowed = r.admit(); !allowed)
            return allowed;
        return validateAnalysisIdentity(fs, *tool);
    };
    auto command = [&](std::vector<std::string> arguments, IClock::Duration timeout) -> Result<AnalysisCommandResult> {
        auto child = *spec;
        child.arguments = std::move(arguments);
        auto result = runAnalysisCommand(launcher, std::move(child), operation.get_token(), admitted, timeout);
        if (!result)
            return result;
        if (result->exitCode != 0)
            return fail(ErrorCode::IoError, "Managed diagnostic tool failed: " + result->output + result->error);
        return result;
    };
    auto version = command({"--version"}, std::chrono::seconds(15));
    if (!version)
        return std::unexpected(version.error());
    // .NET tools print an informational semver (possibly +commit). Compare the entire token.
    if (trim(version->output) != r.expectedVersion)
        return fail(ErrorCode::Unsupported,
                    "Installed diagnostic tool version differs from explicitly selected version");
    auto help = command({"collect", "--help"}, std::chrono::seconds(15));
    if (!help)
        return std::unexpected(help.error());
    if (help->output.find("--process-id") == std::string::npos ||
        help->output.find(r.kind == ManagedDiagnosticKind::GcDump ? "--timeout" : "--duration") == std::string::npos)
        return fail(ErrorCode::Unsupported, "Installed tool lacks required bounded attachment options");
    if (r.kind == ManagedDiagnosticKind::Trace) {
        auto profiles = command({"list-profiles"}, std::chrono::seconds(15));
        if (!profiles)
            return std::unexpected(profiles.error());
        if (profiles->output.find("dotnet-common") == std::string::npos ||
            profiles->output.find("dotnet-sampled-thread-time") == std::string::npos)
            return fail(ErrorCode::Unsupported, "Installed trace tool lacks selected managed thread-time profiles");
    }
    if (auto allowed = admitted(); !allowed)
        return std::unexpected(allowed.error());
    auto exists = fs.exists(r.outputDirectory);
    if (!exists)
        return std::unexpected(exists.error());
    if (*exists)
        return fail(ErrorCode::AlreadyExists, "Diagnostic output directory must be new");
    if (auto made = fs.createDirectories(r.outputDirectory); !made)
        return std::unexpected(made.error());
    auto directory = fs.physicalPath(r.outputDirectory);
    if (!directory)
        return std::unexpected(directory.error());
    auto physicalOutput = io::joinPath(*directory, artifactName(r.kind));
    if (!physicalOutput)
        return std::unexpected(physicalOutput.error());
    auto artifactExists = fs.exists(*physicalOutput);
    if (!artifactExists)
        return std::unexpected(artifactExists.error());
    if (*artifactExists)
        return fail(ErrorCode::AlreadyExists, "Diagnostic artifact already exists");
    auto collected = command(spec->arguments, std::chrono::seconds(r.durationSeconds + 120));
    if (!collected)
        return std::unexpected(collected.error());
    auto output = io::joinPath(r.outputDirectory, artifactName(r.kind));
    if (!output)
        return std::unexpected(output.error());
    auto artifact = captureAnalysisIdentity(fs, *output);
    if (!artifact)
        return std::unexpected(artifact.error());
    if (artifact->physicalPath != *physicalOutput)
        return fail(ErrorCode::Busy, "Diagnostic artifact escaped private directory");
    if (!artifact->status.size)
        return fail(ErrorCode::Corrupt, "Diagnostic tool produced an empty artifact");
    ManagedDiagnosticReport result{
        r.kind, r.processId, std::string(trim(version->output)), collected->output + collected->error, {},
        *tool,  *artifact};
    if (r.kind == ManagedDiagnosticKind::Counters) {
        auto csv = fs.readFileSnapshot(*output, 32 * 1024 * 1024);
        if (!csv)
            return std::unexpected(csv.error());
        result.report = std::move(*csv); // Actual tool CSV including units/names/timestamps, never synthesized metrics.
    } else {
        std::vector<std::string> args{"report", *output};
        if (r.kind == ManagedDiagnosticKind::Trace)
            args.insert(args.end(), {"topN", "--number", "50", "--inclusive", "--verbose"});
        auto report = command(std::move(args), std::chrono::seconds(120));
        if (!report)
            return std::unexpected(report.error());
        if (trim(report->output).empty())
            return fail(ErrorCode::Corrupt, "Diagnostic tool returned no report");
        result.report = std::move(report->output);
        result.log += report->error;
    }
    auto finalDirectory = fs.physicalPath(r.outputDirectory);
    if (!finalDirectory || *finalDirectory != *directory)
        return fail(ErrorCode::Busy, "Diagnostic output directory changed");
    if (auto valid = validateAnalysisIdentity(fs, *artifact); !valid)
        return std::unexpected(valid.error());
    if (auto valid = admitted(); !valid)
        return std::unexpected(valid.error());
    return result;
}
} // namespace frost::run
