// SPDX-License-Identifier: Apache-2.0
// Native analysis evidence and resource-bounded owned child process execution.
#include "frost/run/native_analysis.hpp"

#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"

#include <atomic>
#include <mutex>
namespace frost::run {
Result<AnalysisFileIdentity> captureAnalysisIdentity(io::IFileSystem& fs, std::string_view path, std::size_t cap) {
    auto physical = fs.physicalPath(path);
    if (!physical)
        return std::unexpected(physical.error());
    auto before = fs.status(path);
    if (!before)
        return std::unexpected(before.error());
    if (before->type != io::FileType::Regular)
        return fail(ErrorCode::InvalidArgument, "Analysis input is not a regular file");
    auto bytes = fs.readFileSnapshot(path, cap);
    if (!bytes)
        return std::unexpected(bytes.error());
    auto after = fs.status(path);
    auto finalPath = fs.physicalPath(path);
    if (!after || !finalPath || *before != *after || *physical != *finalPath)
        return fail(ErrorCode::Busy, "Analysis input changed while capturing identity");
    return AnalysisFileIdentity{std::string(path), *physical, io::sha256Hex(*bytes), *after};
}
Result<void> validateAnalysisIdentity(io::IFileSystem& fs, const AnalysisFileIdentity& identity) {
    auto now = captureAnalysisIdentity(fs, identity.path);
    if (!now)
        return std::unexpected(now.error());
    if (*now != identity)
        return fail(ErrorCode::Busy, "Analysis image/source changed: " + identity.path);
    return {};
}
Result<AnalysisCommandResult> runAnalysisCommand(io::IProcessLauncher& launcher, io::ProcessSpec spec,
                                                 std::stop_token stop, AnalysisAdmission admission,
                                                 std::optional<IClock::Duration> timeout, std::size_t outputCap,
                                                 std::function<void(io::ProcessId)> onStarted) {
    if (!io::isAbsolutePath(spec.executable) || !io::isAbsolutePath(spec.workingDirectory) || !outputCap ||
        outputCap > 64U * 1024U * 1024U || (timeout && *timeout <= IClock::Duration::zero()))
        return fail(ErrorCode::InvalidArgument,
                    "Analysis needs absolute executable/directory and bounded positive output/deadline");
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Analysis cancelled before launch");
    if (admission) {
        auto allowed = admission();
        if (!allowed)
            return std::unexpected(allowed.error());
    }
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Analysis cancelled before launch");
    struct Capture {
        std::mutex mutex;
        std::string output, error;
        bool overflow = false;
    };
    auto capture = std::make_shared<Capture>();
    build::RunSessionCallbacks callbacks;
    callbacks.onOutput = [capture, outputCap](build::RunStream stream, std::string_view bytes) {
        std::lock_guard lock(capture->mutex);
        if (capture->overflow)
            return;
        if (bytes.size() > outputCap - capture->output.size() - capture->error.size()) {
            capture->overflow = true;
            return;
        }
        (stream == build::RunStream::Stdout ? capture->output : capture->error).append(bytes);
    };
    spec.killOnClose = true;
    spec.belowNormalPriority = true;
    auto session = build::RunSession::start(launcher, spec, std::move(callbacks), systemClock(),
                                            {.terminateDescendantsOnExit = true, .rejectInvalidUtf8 = true});
    if (!session)
        return std::unexpected(session.error());
    if (onStarted)
        onStarted((*session)->processId());
    const auto started = systemClock().steadyNow();
    std::stop_callback cancel(stop, [&] { (*session)->stop(); });
    bool expired = false, overflow = false;
    std::optional<build::RunExit> exit;
    while (!(exit = (*session)->waitFor(std::chrono::milliseconds(20)))) {
        {
            std::lock_guard lock(capture->mutex);
            overflow = capture->overflow;
        }
        expired = timeout && systemClock().steadyNow() - started >= *timeout;
        if (stop.stop_requested() || expired || overflow)
            (*session)->stop();
    }
    {
        std::lock_guard lock(capture->mutex);
        overflow = capture->overflow;
    }
    if (exit->cleanupFailed || exit->waitFailed)
        return fail(ErrorCode::IoError, "Analysis process cleanup/wait failed");
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Analysis cancelled; owned process tree stopped");
    if (expired)
        return fail(ErrorCode::Timeout, "Analysis deadline exceeded");
    if (overflow)
        return fail(ErrorCode::OutOfRange, "Analysis output exceeded bounded capture");
    if (exit->decodingFailed)
        return fail(ErrorCode::Corrupt, "Analysis output was not valid UTF-8");
    std::lock_guard lock(capture->mutex);
    return AnalysisCommandResult{exit->exitCode, std::move(capture->output), std::move(capture->error),
                                 (*session)->processId()};
}
} // namespace frost::run
