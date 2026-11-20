// SPDX-License-Identifier: Apache-2.0
#include "frost/run/dotnet_tests.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include <pugixml.hpp>
#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <utility>

namespace frost::run {
namespace {
std::string_view local(std::string_view name) { const auto colon = name.find(':'); return colon == name.npos ? name : name.substr(colon + 1); }
pugi::xml_node child(pugi::xml_node parent, std::string_view name) { for (const auto node : parent.children()) if (local(node.name()) == name) return node; return {}; }
std::optional<std::chrono::microseconds> duration(std::string_view text) {
    if (text.empty()) return {};
    const auto first = text.find(':'), second = text.find(':', first == text.npos ? 0 : first + 1);
    if (first == text.npos || second == text.npos) return {};
    const auto integer = [](std::string_view value) -> std::optional<std::int64_t> { std::int64_t n = 0; const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n); if (ec != std::errc{} || end != value.data()+value.size() || n < 0) return {}; return n; };
    const auto hours = integer(text.substr(0, first)), minutes = integer(text.substr(first+1, second-first-1));
    const auto dot = text.find('.', second+1);
    const auto seconds = integer(text.substr(second+1, dot == text.npos ? text.size()-second-1 : dot-second-1));
    if (!hours || !minutes || !seconds || *hours > 100000 || *minutes > 59 || *seconds > 59) return {};
    std::int64_t micros = 0;
    if (dot != text.npos) { auto fraction = text.substr(dot+1); if (fraction.empty() || fraction.size() > 7) return {}; for (const char digit : fraction) if (digit < '0' || digit > '9') return {}; fraction = fraction.substr(0, 6); const auto n = integer(fraction); if (!n) return {}; micros = *n; for (auto count = fraction.size(); count < 6; ++count) micros *= 10; }
    return std::chrono::microseconds(((*hours*60+*minutes)*60+*seconds)*1000000+micros);
}
}
Result<io::ProcessSpec> dotnetTestSpec(const DotnetTestRequest& request) {
    auto spec = build::managedProjectQuerySpec(request.project);
    if (!spec) return std::unexpected(std::move(spec.error()));
    if (request.project.targetFramework.empty() || !io::isAbsolutePath(request.reportDirectory)) return fail(ErrorCode::InvalidArgument, "select an evaluated framework and private absolute test report directory");
    if (request.platform == DotnetTestPlatform::MicrosoftTestingPlatform && (request.discover || !request.selected.empty()))
        return fail(ErrorCode::Unsupported, "MTP discovery and exact selection require an adapter-specific protocol; Run All supports its real TRX extension");
    spec->arguments = {"test"};
    if (request.platform == DotnetTestPlatform::MicrosoftTestingPlatform) spec->arguments.insert(spec->arguments.end(), {"--project", request.project.project});
    else spec->arguments.push_back(request.project.project);
    spec->arguments.insert(spec->arguments.end(), {"--configuration", request.project.configuration, "--framework", request.project.targetFramework, "--no-restore"});
    if (!request.build) spec->arguments.push_back("--no-build");
    if (!request.project.runtimeIdentifier.empty()) spec->arguments.insert(spec->arguments.end(), {"--runtime", request.project.runtimeIdentifier});
    if (request.discover) spec->arguments.push_back("--list-tests");
    else {
        spec->arguments.insert(spec->arguments.end(), {"--results-directory", request.reportDirectory});
        if (request.platform == DotnetTestPlatform::MicrosoftTestingPlatform) spec->arguments.insert(spec->arguments.end(), {"--", "--report-trx", "--report-trx-filename", "results.trx"});
        else spec->arguments.insert(spec->arguments.end(), {"--logger", "trx;LogFileName=results.trx"});
    }
    if (!request.selected.empty()) {
        std::string filter;
        for (const auto& name : request.selected) {
            if (name.empty() || name.find_first_of("&|!=~()\\\r\n") != name.npos || name.find('\0') != name.npos)
                return fail(ErrorCode::Unsupported, "adapter test identity cannot be represented by an exact VSTest filter");
            if (!filter.empty()) filter += "|";
            filter += "FullyQualifiedName=" + name;
        }
        spec->arguments.insert(spec->arguments.end(), {"--filter", filter});
    }
    return spec;
}
Result<std::vector<std::string>> parseDotnetTestDiscovery(std::string_view output) {
    constexpr std::string_view marker = "The following Tests are available:";
    const auto start = output.find(marker);
    if (start == output.npos) return fail(ErrorCode::Corrupt, "dotnet test returned no VSTest discovery marker");
    output.remove_prefix(start+marker.size());
    std::vector<std::string> names;
    std::set<std::string> seen;
    while (!output.empty()) {
        const auto end = output.find('\n'); auto line = output.substr(0, end);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("    ")) { line.remove_prefix(4); if (line.empty()) return fail(ErrorCode::Corrupt, "empty adapter test identity"); std::string name(line); if (!seen.insert(name).second) return fail(ErrorCode::Unsupported, "duplicate adapter display identities cannot be selected unambiguously"); names.push_back(std::move(name)); }
        else if (!line.empty() && !names.empty()) break;
        if (end == output.npos) break; output.remove_prefix(end+1);
    }
    return names;
}
Result<std::vector<DotnetTestResult>> parseDotnetTrx(std::string_view xml) {
    if (xml.size() > 16*1024*1024 || xml.find("<!DOCTYPE") != xml.npos) return fail(ErrorCode::Corrupt, "invalid or oversized TRX report");
    pugi::xml_document document;
    const auto parsed = document.load_buffer(xml.data(), xml.size());
    if (!parsed || local(document.document_element().name()) != "TestRun") return fail(ErrorCode::Corrupt, "invalid TRX TestRun document");
    const auto root = document.document_element(), results = child(root, "Results"), summary = child(root, "ResultSummary");
    if (!results || !summary) return fail(ErrorCode::Corrupt, "TRX report is incomplete");
    std::map<std::string, std::string> identities;
    for (const auto definition : child(root, "TestDefinitions").children()) {
        const auto method = child(definition, "TestMethod");
        if (method) identities[definition.attribute("id").value()] = std::string(method.attribute("className").value()) + "." + method.attribute("name").value();
    }
    std::vector<DotnetTestResult> report; std::set<std::string> executions;
    for (const auto row : results.children()) {
        if (local(row.name()) != "UnitTestResult") continue;
        DotnetTestResult test; test.id = row.attribute("testId").value(); test.result.name = row.attribute("testName").value();
        const std::string execution = row.attribute("executionId").value(), outcome = row.attribute("outcome").value();
        if (test.id.empty() || test.result.name.empty() || execution.empty() || !executions.insert(execution).second) return fail(ErrorCode::Corrupt, "TRX result has ambiguous identity");
        test.fullyQualifiedName = identities[test.id];
        if (outcome == "Passed") test.result.outcome = TestOutcome::Passed;
        else if (outcome == "Failed" || outcome == "Error" || outcome == "Timeout" || outcome == "Aborted") test.result.outcome = TestOutcome::Failed;
        else if (outcome == "NotExecuted" || outcome == "Inconclusive") test.result.outcome = TestOutcome::Skipped;
        else return fail(ErrorCode::Corrupt, "unknown TRX outcome");
        const std::string durationText = row.attribute("duration").value();
        test.result.duration = duration(durationText); if (!durationText.empty() && !test.result.duration) return fail(ErrorCode::Corrupt, "invalid TRX duration");
        test.result.reason = outcome;
        const auto out = child(row, "Output"), error = child(out, "ErrorInfo");
        test.result.output = child(out, "StdOut").text().get();
        if (error) test.result.failures.push_back({std::string(child(error, "Message").text().get()) + "\n" + child(error, "StackTrace").text().get(), {}});
        report.push_back(std::move(test));
    }
    return report;
}
Result<std::unique_ptr<build::BuildSession>> startDotnetTest(io::IProcessLauncher& launcher, const project::TrustStore& trust,
    const DotnetTestRequest& request, const std::vector<std::string>& roots, build::BuildCallbacks callbacks, IClock& clock) {
    auto spec = dotnetTestSpec(request); if (!spec) return std::unexpected(std::move(spec.error()));
    if (roots.empty()) return fail(ErrorCode::InvalidArgument, "test execution requires explicit trust roots");
    for (const auto& root : roots) if (!io::isAbsolutePath(root) || !trust.isTrusted(root)) return fail(ErrorCode::Untrusted, "trust the workspace before discovering or running .NET tests");
    if (!trust.isTrusted(io::parentPath(request.project.project))) return fail(ErrorCode::Untrusted, "test project is not trusted");
    return build::BuildSession::start(launcher, *spec, true, std::move(callbacks), clock);
}
}
