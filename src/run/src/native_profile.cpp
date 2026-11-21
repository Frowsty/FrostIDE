// SPDX-License-Identifier: Apache-2.0
// RUN-13 genuine perf reports and uniquely owned WPR ETW CPU recordings plus xperf symbol reports.
#include "frost/build/run_target.hpp"
#include "frost/io/path.hpp"
#include "frost/run/native_analysis.hpp"

#include <algorithm>
#include <cctype>
namespace frost::run {
Result<NativeProfileReport> collectNativeProfile(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                                 const NativeProfileRequest& request, std::stop_token stop,
                                                 AnalysisAdmission admission) {
    if (!io::isAbsolutePath(request.plan.program) || !io::isAbsolutePath(request.plan.workingDirectory) ||
        !io::isAbsolutePath(request.outputDirectory))
        return fail(ErrorCode::InvalidArgument, "Profiling needs absolute program/working/output paths");
    if (request.duration <= IClock::Duration::zero() || request.duration > std::chrono::minutes(5))
        return fail(ErrorCode::InvalidArgument, "Profiling interval must be positive and at most five minutes");
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Profiling cancelled");
    if (admission) {
        auto allowed = admission();
        if (!allowed)
            return std::unexpected(allowed.error());
    }
    const bool etw = request.backend == NativeProfileBackend::WindowsEtw;
    if (etw) {
#ifndef _WIN32
        return fail(ErrorCode::Unsupported, "ETW CPU profiling requires Windows");
#endif
        if (!request.systemWideEtwConsent)
            return fail(ErrorCode::Untrusted, "ETW CPU sampling needs explicit system-wide recording consent");
        if (!io::isAbsolutePath(request.wpr) || !io::isAbsolutePath(request.xperf))
            return fail(ErrorCode::NotFound, "Installed Windows WPR/xperf tools unavailable");
        if (!request.sessionName.starts_with("FrostIDE-") || request.sessionName.size() != 45 ||
            !std::ranges::all_of(std::string_view(request.sessionName).substr(9),
                                 [](unsigned char c) { return std::isxdigit(c) || c == '-'; }))
            return fail(ErrorCode::InvalidArgument, "ETW needs a unique FrostIDE UUID instance");
    } else {
#ifdef _WIN32
        return fail(ErrorCode::Unsupported, "Linux perf profiling requires Linux");
#endif
        if (!io::isAbsolutePath(request.perf))
            return fail(ErrorCode::NotFound, "Installed Linux perf tool unavailable");
    }
    auto existing = fs.exists(request.outputDirectory);
    if (!existing)
        return std::unexpected(existing.error());
    if (*existing)
        return fail(ErrorCode::AlreadyExists, "Profiling output directory must be new");
    if (auto made = fs.createDirectories(request.outputDirectory); !made)
        return std::unexpected(made.error());
    auto output = fs.physicalPath(request.outputDirectory);
    if (!output)
        return std::unexpected(output.error());
    auto binary = captureAnalysisIdentity(fs, request.plan.program);
    if (!binary)
        return std::unexpected(binary.error());
    std::optional<AnalysisFileIdentity> admittedTrace;
    const auto guarded = [&]() -> Result<void> {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Profiling cancelled");
        if (admission) {
            auto allowed = admission();
            if (!allowed)
                return allowed;
        }
        auto now = fs.physicalPath(request.outputDirectory);
        if (!now || *now != *output)
            return fail(ErrorCode::Busy, "Profiling output directory changed");
        if (admittedTrace) {
            auto valid = validateAnalysisIdentity(fs, *admittedTrace);
            if (!valid)
                return valid;
        }
        return validateAnalysisIdentity(fs, *binary);
    };
    const auto tool = [&](std::string executable, std::vector<std::string> arguments,
                          std::vector<std::pair<std::string, std::optional<std::string>>> environment = {})
        -> Result<AnalysisCommandResult> {
        io::ProcessSpec spec;
        spec.executable = std::move(executable);
        spec.arguments = std::move(arguments);
        spec.environment = std::move(environment);
        spec.workingDirectory = request.outputDirectory;
        return runAnalysisCommand(launcher, std::move(spec), stop, guarded, std::chrono::minutes(5),
                                  32U * 1024U * 1024U, {}, admission);
    };
    NativeProfileReport report;
    report.backend = request.backend;
    report.binary = *binary;
    const auto trace = io::joinPath(request.outputDirectory, etw ? "cpu.etl" : "perf.data");
    if (!trace)
        return std::unexpected(trace.error());
    if (etw) {
        const auto captureRecorder = [&]() -> Result<AnalysisFileIdentity> {
#ifdef _WIN32
            if (&fs == &io::realFileSystem()) {
                const auto commandProcessor = build::systemCommandProcessor();
                if (commandProcessor) {
                    const auto systemRecorder = io::joinPath(io::parentPath(*commandProcessor), "wpr.exe");
                    const auto requested = io::normalizePath(request.wpr);
                    if (systemRecorder && requested && *requested == *systemRecorder)
                        return captureWindowsSystemRecorderIdentity(*systemRecorder);
                }
            }
#endif
            return captureAnalysisIdentity(fs, request.wpr);
        };
        auto recorder = captureRecorder();
        if (!recorder)
            return std::unexpected(recorder.error());
        const auto verifyRecorder = [&]() -> Result<void> {
            auto current = captureRecorder();
            if (!current)
                return std::unexpected(current.error());
            if (*current != *recorder)
                return fail(ErrorCode::Busy, "ETW recorder image changed");
            return {};
        };
        const auto retire = [&](bool save) -> Result<void> {
            // OS recording retirement is independent of revoked project execution authority.
            // The recorder image is re-admitted; no arbitrary project tool is run on cleanup.
            if (auto same = verifyRecorder(); !same)
                return fail(same.error().code(),
                            "Cannot retire owned ETW instance " + request.sessionName + ": " + same.error().message());
            io::ProcessSpec spec;
            spec.executable = recorder->path;
            spec.workingDirectory = request.outputDirectory;
            spec.arguments = save ? std::vector<std::string>{"-stop", *trace, "-instancename", request.sessionName}
                                  : std::vector<std::string>{"-cancel", "-instancename", request.sessionName};
            auto ended = runAnalysisCommand(launcher, std::move(spec), {}, {}, std::chrono::minutes(2));
            if (!ended)
                return std::unexpected(ended.error());
            if (ended->exitCode != 0)
                return fail(ErrorCode::IoError, "Owned ETW instance " + request.sessionName +
                                                    " cleanup failed: " + ended->error + ended->output);
            return {};
        };
        // Microsoft requires -instancename last. The short OS start command is joined even
        // if project cancellation arrives, so a successful recorder start cannot be forgotten.
        // https://learn.microsoft.com/windows-hardware/test/wpt/wpr-command-line-options
        io::ProcessSpec start;
        start.executable = recorder->path;
        start.workingDirectory = request.outputDirectory;
        start.arguments = {"-start", "CPU", "-filemode", "-instancename", request.sessionName};
        auto started = runAnalysisCommand(
            launcher, std::move(start), {},
            [&]() -> Result<void> {
                auto admitted = guarded();
                if (!admitted)
                    return admitted;
                return verifyRecorder();
            },
            std::chrono::minutes(1));
        if (!started || started->exitCode != 0) {
            // A failed/timed-out start may have already created its uniquely owned OS instance.
            auto cancelled = retire(false);
            const auto cleanup =
                cancelled ? std::string{} : " Recorder retirement also reported: " + cancelled.error().message();
            if (!started)
                return fail(started.error().code(), started.error().message() + cleanup);
            return fail(ErrorCode::IoError, "ETW recording unavailable (permissions/session): " + started->error +
                                                started->output + cleanup);
        }
        report.diagnostics = started->output + started->error;
        io::ProcessSpec program;
        program.executable = request.plan.program;
        program.arguments = request.plan.arguments;
        program.workingDirectory = request.plan.workingDirectory;
        for (const auto& entry : request.plan.environment)
            program.environment.emplace_back(entry.name, entry.value);
        io::ProcessId pid = 0;
        auto ran = runAnalysisCommand(
            launcher, std::move(program), stop, guarded, request.duration, 32U * 1024U * 1024U,
            [&](auto value) { pid = value; }, admission);
        const bool intervalEnded = !ran && ran.error().code() == ErrorCode::Timeout;
        auto ended = retire(ran.has_value() || intervalEnded);
        if (!ended) {
            auto cancelled = retire(false);
            return fail(ended.error().code(),
                        ended.error().message() +
                            (cancelled ? " Owned instance cancelled."
                                       : " Recorder retirement also reported: " + cancelled.error().message()));
        }
        if (!ran && !intervalEnded)
            return std::unexpected(ran.error());
        if (ran)
            report.programExitCode = ran->exitCode;
        report.programPid = pid;
        auto recorded = captureAnalysisIdentity(fs, *trace);
        if (!recorded)
            return std::unexpected(recorded.error());
        admittedTrace = *recorded;
        const auto symbols = fs.physicalPath(io::parentPath(request.plan.program));
        if (!symbols || symbols->starts_with("//") || symbols->find_first_of(";*") != symbols->npos)
            return fail(ErrorCode::Unsupported, "Profiling requires a local binary/symbol directory");
        auto cache = io::joinPath(request.outputDirectory, "symcache");
        if (!cache)
            return std::unexpected(cache.error());
        if (auto made = fs.createDirectories(*cache); !made)
            return std::unexpected(made.error());
        // Actual profile -detail is bucketed by process/function when local symbols resolve.
        // https://learn.microsoft.com/windows-hardware/test/wpt/symbols
        auto actual = tool(request.xperf, {"-i", *trace, "-symbols", "-a", "profile", "-detail"},
                           {{"_NT_SYMBOL_PATH", *symbols}, {"_NT_ALT_SYMBOL_PATH", ""}, {"_NT_SYMCACHE_PATH", *cache}});
        if (!actual)
            return std::unexpected(actual.error());
        if (actual->exitCode != 0)
            return fail(ErrorCode::IoError, "ETW profile analysis failed: " + actual->error);
        report.toolReport = std::move(actual->output);
        report.diagnostics += actual->error;
    } else {
        std::vector<std::string> arguments{"record", "-o", *trace, "--call-graph", "dwarf", "--", request.plan.program};
        arguments.insert(arguments.end(), request.plan.arguments.begin(), request.plan.arguments.end());
        io::ProcessSpec record;
        record.executable = request.perf;
        record.arguments = std::move(arguments);
        record.workingDirectory = request.plan.workingDirectory;
        for (const auto& entry : request.plan.environment)
            record.environment.emplace_back(entry.name, entry.value);
        record.environment.emplace_back("DEBUGINFOD_URLS", "");
        auto ran = runAnalysisCommand(launcher, std::move(record), stop, guarded, request.duration, 32U * 1024U * 1024U,
                                      {}, admission);
        if (!ran && ran.error().code() != ErrorCode::Timeout)
            return std::unexpected(ran.error());
        if (ran) {
            if (ran->exitCode != 0)
                return fail(ErrorCode::IoError, "perf recorder failed: " + ran->error);
            report.diagnostics = std::move(ran->error);
        }
        auto recorded = captureAnalysisIdentity(fs, *trace);
        if (!recorded)
            return std::unexpected(recorded.error());
        admittedTrace = *recorded;
        // perf owns the recorded command; stdout stays genuine. No version-dependent columns are guessed.
        auto actual =
            tool(request.perf,
                 {"report", "--stdio", "--no-children", "--percent-limit=0", "--show-nr-samples", "-i", *trace},
                 {{"DEBUGINFOD_URLS", ""}});
        if (!actual)
            return std::unexpected(actual.error());
        if (actual->exitCode != 0)
            return fail(ErrorCode::IoError, "perf report failed: " + actual->error);
        report.toolReport = std::move(actual->output);
        report.diagnostics += actual->error;
    }
    if (report.toolReport.empty())
        return fail(ErrorCode::Unsupported,
                    "Profiler produced no report; sampling/permissions/symbols may be unavailable");
    if (!admittedTrace)
        return fail(ErrorCode::IoError, "Profiler trace identity was not retained");
    report.trace = *admittedTrace;
    if (auto allowed = guarded(); !allowed)
        return std::unexpected(allowed.error());
    auto saved = io::joinPath(request.outputDirectory, "cpu-report.txt");
    if (!saved)
        return std::unexpected(saved.error());
    if (auto written = fs.writeFileAtomic(*saved, report.toolReport); !written)
        return std::unexpected(written.error());
    return report;
}
} // namespace frost::run
