// SPDX-License-Identifier: Apache-2.0
// Existing inspection scope/profile plus one governed bounded pool of exact CDB configuration jobs.
#include "frost/lang/analyzer.hpp"

#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/uri.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
namespace frost::lang {
namespace analyzer_detail {
Result<Json> unit(io::IProcessLauncher&, const AnalyzerRequest&, const std::string&, std::size_t,
                  sched::ResourceGovernor&);
unsigned workers(const AnalyzerRequest&, sched::ResourceGovernor&);
Result<void> append(AnalyzerReport&, const Json&, const AnalyzerRequest&, const std::string&, std::size_t,
                    const std::string&, io::IFileSystem&);
void reconcileFixes(AnalyzerReport&, const std::vector<Json>&);
} // namespace analyzer_detail
Result<AnalyzerReport> runFrostAnalyzer(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                        const AnalyzerRequest& request) {
    if (!io::isAbsolutePath(request.executable) || !io::isAbsolutePath(request.projectRoot) ||
        !io::isAbsolutePath(request.compilationDatabasePath))
        return fail(ErrorCode::InvalidArgument, "Analyzer needs absolute tool/project/database paths");
    const auto tool = fs.status(request.executable);
    if (!tool || tool->type != io::FileType::Regular)
        return fail(ErrorCode::NotFound, "frost-analyzer is unavailable: " + request.executable);
    auto database = fs.readFileSnapshot(request.compilationDatabasePath, 32U * 1024U * 1024U);
    if (!database)
        return std::unexpected(database.error());
    auto commands = parseInspectionCompileCommands(*database);
    if (!commands)
        return std::unexpected(commands.error());
    if (commands->empty() || commands->size() > 16384)
        return fail(ErrorCode::OutOfRange, "Analyzer scope needs1..16384 captured command rows");
    const auto selected = resolveInspectionScopeFiles(*commands, request.scope);
    std::vector<std::size_t> ordinals;
    std::vector<std::string> sources;
    for (std::size_t i = 0; i < commands->size(); ++i) {
        const auto& command = (*commands)[i];
        auto directory = io::isAbsolutePath(command.directory) ? io::normalizePath(command.directory)
                                                               : io::joinPath(request.projectRoot, command.directory);
        if (!directory)
            return std::unexpected(directory.error());
        auto source =
            io::isAbsolutePath(command.file) ? io::normalizePath(command.file) : io::joinPath(*directory, command.file);
        if (!source)
            return std::unexpected(source.error());
        if (std::ranges::any_of(selected,
                                [&](const auto& file) { return documentKey(file) == documentKey(*source); })) {
            ordinals.push_back(i);
            sources.push_back(*source);
        }
    }
    if (ordinals.empty())
        return fail(ErrorCode::NotFound, "Analyzer scope has no actual translation-unit configurations");
    std::optional<sched::ResourceGovernor> fallback;
    if (!request.governor)
        fallback.emplace();
    auto& governor = request.governor ? *request.governor : *fallback;
    std::optional<sched::HeavyLegLease> lease;
    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    while (!request.stop.stop_requested()) {
        auto acquired = governor.tryAcquireHeavyLeg(sched::HeavyLegKind::Inspection);
        if (acquired) {
            lease = std::move(*acquired);
            break;
        }
        if (acquired.error().code() != ErrorCode::Busy)
            return std::unexpected(acquired.error());
        if (std::chrono::steady_clock::now() >= deadline)
            return fail(ErrorCode::Timeout, "Analyzer waiting for inspection resources");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (request.stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Analyzer request cancelled");
    std::vector<std::optional<Result<Json>>> replies(ordinals.size());
    std::atomic<std::size_t> next = 0;
    {
        std::vector<std::jthread> pool;
        const auto count = std::min<std::size_t>(ordinals.size(), analyzer_detail::workers(request, governor));
        for (std::size_t worker = 0; worker < count; ++worker)
            pool.emplace_back([&] {
                while (!request.stop.stop_requested()) {
                    const auto index = next.fetch_add(1);
                    if (index >= ordinals.size())
                        break;
                    replies[index] = analyzer_detail::unit(launcher, request, *database, ordinals[index], governor);
                }
            });
    } // Every owned child has communicated/joined before results escape the worker API.
    if (request.stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Analyzer request cancelled");
    AnalyzerReport report;
    report.inspection.toolName = "frost-analyzer";
    report.inspection.toolPath = request.executable;
    report.inspection.informationUri = "https://github.com/Frowsty/FrostIDE";
    report.inspection.columnKind = "unicodeCodePoints";
    report.inspection.profileName = request.profile.name;
    report.inspection.commandLine = request.executable + " --analyze-unit";
    report.inspection.inspectedFiles = selected;
    std::vector<Json> actual;
    for (std::size_t i = 0; i < replies.size(); ++i) {
        if (!replies[i])
            return fail(ErrorCode::Cancelled, "Analyzer scope did not complete all selected jobs");
        if (!*replies[i])
            return std::unexpected(replies[i]->error());
        const auto& body = **replies[i];
        if (auto appended = analyzer_detail::append(report, body, request, *database, ordinals[i], sources[i], fs);
            !appended)
            return std::unexpected(appended.error());
        actual.push_back(body);
    }
    auto physical = fs.physicalPath(request.compilationDatabasePath);
    if (!physical)
        return std::unexpected(physical.error());
    report.inputs.push_back({request.compilationDatabasePath, *database, *physical, false});
    report.inspection.executionSuccessful = report.failedConfigurations == 0 && report.limitations.empty();
    report.inspection.exitCode = report.inspection.executionSuccessful ? 0 : 2;
    analyzer_detail::reconcileFixes(report, actual);
    if (!report.inspection.executionSuccessful)
        report.fixes.clear();
    if (auto current = validateAnalyzerInputs(fs, report); !current)
        return std::unexpected(current.error());
    return report;
}
} // namespace frost::lang
