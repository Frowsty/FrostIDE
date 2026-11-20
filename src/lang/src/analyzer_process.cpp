// SPDX-License-Identifier: Apache-2.0
// Bounded native analyzer children; below-normal process trees, timeout, cancellation and RSS cap.
#include "frost/io/path.hpp"
#include "frost/lang/analyzer.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
namespace frost::lang::analyzer_detail {
Result<Json> unit(io::IProcessLauncher& launcher, const AnalyzerRequest& request, const std::string& database,
                  std::size_t ordinal, sched::ResourceGovernor& governor) {
    auto overlays = Json::array();
    for (const auto& overlay : request.overlays)
        overlays.push(Json::object().set("path", overlay.path).set("contents", overlay.contents));
    auto payload = Json::object()
                       .set("projectRoot", request.projectRoot)
                       .set("compilationDatabase", database)
                       .set("configuration", request.compilationDatabasePath)
                       .set("commandIndex", ordinal)
                       .set("overlays", std::move(overlays));
    const auto bytes = payload.dump();
    if (bytes.size() > 32U * 1024U * 1024U)
        return fail(ErrorCode::OutOfRange, "Analyzer captured request exceeds32MiB");
    if (request.stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Analyzer request cancelled");
    io::ProcessSpec spec;
    spec.executable = request.executable;
    spec.arguments = {"--analyze-unit"};
    spec.workingDirectory = request.projectRoot;
    spec.stdinMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    spec.belowNormalPriority = true;
#ifdef _WIN32
    spec.residentMemoryLimitBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
#endif
    if (!request.llvmRoot.empty())
        spec.environment.emplace_back("FROST_LLVM_ROOT", request.llvmRoot);
    auto child = launcher.launch(spec);
    if (!child)
        return std::unexpected(child.error());
    const auto pid = (*child)->pid();
    std::atomic<bool> complete = false, overBudget = false;
    // Native RSS observations also enforce the cap on Linux, where pre-exec working-set policy is unsupported.
    std::jthread monitor([&](std::stop_token stop) {
        while (!stop.stop_requested() && !complete.load()) {
            const auto rss = governor.sampleProcessRss(pid, sched::HeavyLegKind::Inspection);
            if ((rss && *rss > 2ULL * 1024ULL * 1024ULL * 1024ULL) || governor.isCombinedOverRssCap()) {
                overBudget = true;
                static_cast<void>((*child)->terminate());
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    std::stop_callback cancel(request.stop, [&] { static_cast<void>((*child)->terminate()); });
    auto output = (*child)->communicate(bytes, request.timeout);
    complete = true;
    monitor.request_stop();
    monitor.join();
    governor.unregisterProcess(pid);
    if (request.stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Analyzer request cancelled");
    if (overBudget.load())
        return fail(ErrorCode::OutOfRange, "Analyzer process exceeded actual RSS budget");
    if (!output)
        return std::unexpected(output.error());
    if (output->signal || output->stdoutBytes.size() > 64U * 1024U * 1024U)
        return fail(ErrorCode::IoError, "Analyzer terminated or exceeded report budget");
    auto body = parseJson(output->stdoutBytes);
    if (!body || body->stringAt("kind") != "frost-analyzer-unit" || body->integerAt("schema") != 1)
        return fail(ErrorCode::Corrupt, "Invalid native analyzer report");
    const auto status = body->stringAt("status");
    if ((status == "ready" && output->exitCode != 0) || (status == "partial" && output->exitCode != 2) ||
        (status != "ready" && status != "partial" && output->exitCode != 1))
        return fail(ErrorCode::Corrupt, "Analyzer native exit disagrees with report status");
    return body;
}
unsigned workers(const AnalyzerRequest& request, sched::ResourceGovernor& governor) {
    const auto host = governor.hostResources();
    auto count = std::min({4U, std::max(1U, host.logicalCores / 4), std::max(1U, request.maxWorkers),
                           std::max(1U, governor.clangdJobs())});
    constexpr auto perProcess = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    if (host.availablePhysicalBytes)
        count = std::min(
            count,
            std::max(1U, static_cast<unsigned>(std::min<std::uint64_t>(4, host.availablePhysicalBytes / perProcess))));
    if (host.availableCommitBytes)
        count = std::min(count, std::max(1U, static_cast<unsigned>(
                                                 std::min<std::uint64_t>(4, host.availableCommitBytes / perProcess))));
    return count;
}
} // namespace frost::lang::analyzer_detail
