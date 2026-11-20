// SPDX-License-Identifier: Apache-2.0
// SARIF 2.1 ingestion of actual primary physical locations, including explicit URI bases/artifacts.
#include "frost/lang/analyzer.hpp"
#include "frost/lang/uri.hpp"

#include <limits>
namespace frost::lang {
namespace {
Result<std::string> locationUri(const Json& location, const Json& run, unsigned depth = 0) {
    if (depth > 16)
        return fail(ErrorCode::Corrupt, "SARIF URI-base cycle/depth exceeds16");
    std::string uri(location.stringAt("uri"));
    if (uri.empty()) {
        const auto index = location.unsignedAt("index");
        const auto* artifacts = run.find("artifacts");
        if (!index || !artifacts || !artifacts->isArray() || *index >= artifacts->size())
            return fail(ErrorCode::Unsupported, "SARIF location has no actual URI or valid artifact index");
        const auto* nested = artifacts->items()[static_cast<std::size_t>(*index)].find("location");
        if (!nested)
            return fail(ErrorCode::Corrupt, "SARIF artifact has no location");
        return locationUri(*nested, run, depth + 1);
    }
    if (uri.starts_with("file:"))
        return uri;
    if (uri.find(':') != std::string::npos || uri.find_first_of("?#") != std::string::npos)
        return fail(ErrorCode::Unsupported, "SARIF source URI must resolve to an actual file URI");
    const auto baseId = location.stringAt("uriBaseId");
    const auto* bases = run.find("originalUriBaseIds");
    const auto* base = bases ? bases->find(baseId) : nullptr;
    if (baseId.empty() || !base)
        return fail(ErrorCode::Unsupported, "Relative SARIF source URI has no explicit originalUriBaseIds base");
    auto parent = locationUri(*base, run, depth + 1);
    if (!parent)
        return std::unexpected(parent.error());
    const auto slash = parent->rfind('/');
    if (slash == std::string::npos)
        return fail(ErrorCode::Corrupt, "Invalid SARIF file base URI");
    return parent->substr(0, slash + 1) + uri;
}
Result<InspectionReport> importRun(const Json& run) {
    const auto *tool = run.find("tool"), *driver = tool ? tool->find("driver") : nullptr;
    if (!driver || driver->stringAt("name").empty())
        return fail(ErrorCode::Corrupt, "SARIF run has no actual tool driver name");
    const auto* results = run.find("results");
    if (!results || !results->isArray() || results->size() > 100000)
        return fail(ErrorCode::Corrupt, "SARIF results must be a bounded array");
    InspectionReport report;
    report.profileName = "Imported SARIF";
    report.toolName = std::string(driver->stringAt("name"));
    report.toolVersion = std::string(driver->stringAt("version"));
    report.informationUri = std::string(driver->stringAt("informationUri"));
    report.columnKind = std::string(run.stringAt("columnKind"));
    if (report.columnKind.empty())
        report.columnKind = "utf16CodeUnits";
    if (report.columnKind != "utf16CodeUnits" && report.columnKind != "unicodeCodePoints")
        return fail(ErrorCode::Unsupported, "Unsupported SARIF columnKind");
    report.executionSuccessful = false;
    if (const auto* invocations = run.find("invocations");
        invocations && invocations->isArray() && !invocations->items().empty()) {
        report.executionSuccessful = true;
        for (const auto& invocation : invocations->items()) {
            const auto* success = invocation.find("executionSuccessful");
            if (!success || !success->asBool().value_or(false))
                report.executionSuccessful = false;
        }
    } else
        report.rawStderr = "External SARIF did not state executionSuccessful";
    for (const auto& result : results->items()) {
        const auto kind = result.stringAt("kind");
        if (kind == "pass" || kind == "notApplicable")
            continue;
        const Json* rule = nullptr;
        const auto* rules = driver->find("rules");
        const auto index = result.unsignedAt("ruleIndex");
        if (index && rules && rules->isArray() && *index < rules->size())
            rule = &rules->items()[static_cast<std::size_t>(*index)];
        std::string id(result.stringAt("ruleId"));
        if (id.empty() && rule)
            id = std::string(rule->stringAt("id"));
        if (id.empty())
            return fail(ErrorCode::Unsupported, "SARIF result has no reported rule identity");
        if (!rule && rules && rules->isArray())
            for (const auto& value : rules->items())
                if (value.stringAt("id") == id) {
                    rule = &value;
                    break;
                }
        auto level = result.stringAt("level");
        if (level.empty() && rule)
            if (const auto* defaults = rule->find("defaultConfiguration"))
                level = defaults->stringAt("level");
        if (level.empty())
            level = "warning"; // SARIF's normative default, not an inferred tool result.
        InspectionSeverity severity;
        if (level == "error")
            severity = InspectionSeverity::Error;
        else if (level == "warning")
            severity = InspectionSeverity::Warning;
        else if (level == "note" || level == "none")
            severity = InspectionSeverity::Info;
        else
            return fail(ErrorCode::Corrupt, "Unknown SARIF severity level");
        if (const auto* suppressions = result.find("suppressions"); suppressions && suppressions->isArray())
            for (const auto& suppression : suppressions->items())
                if (suppression.stringAt("status") == "accepted")
                    severity = InspectionSeverity::Disabled;
        const auto* message = result.find("message");
        std::string text(message ? message->stringAt("text") : std::string_view{});
        if (text.empty() && message)
            text = std::string(message->stringAt("markdown"));
        if (text.empty())
            return fail(ErrorCode::Unsupported,
                        "SARIF message requires actual text/markdown; resource lookup is unsupported");
        const auto* locations = result.find("locations");
        if (!locations || !locations->isArray() || locations->items().empty())
            return fail(ErrorCode::Unsupported, "SARIF result has no physical source location");
        for (const auto& location : locations->items()) {
            const auto *physical = location.find("physicalLocation"),
                       *artifact = physical ? physical->find("artifactLocation") : nullptr,
                       *region = physical ? physical->find("region") : nullptr;
            if (!artifact || !region)
                return fail(
                    ErrorCode::Unsupported,
                    "Logical/file-only SARIF locations need an explicit source region for this Problems profile");
            auto uri = locationUri(*artifact, run);
            if (!uri)
                return std::unexpected(uri.error());
            auto path = uriToPath(*uri);
            if (!path)
                return std::unexpected(path.error());
            const auto line = region->unsignedAt("startLine");
            const auto column = region->unsignedAt("startColumn").value_or(1);
            if (!line || !*line || *line > std::numeric_limits<std::uint32_t>::max() || !column ||
                column > std::numeric_limits<std::uint32_t>::max())
                return fail(ErrorCode::Corrupt, "Invalid SARIF start region");
            const auto endLine = region->unsignedAt("endLine").value_or(*line),
                       endColumn = region->unsignedAt("endColumn").value_or(column);
            if (endLine < *line || endLine > std::numeric_limits<std::uint32_t>::max() || !endColumn ||
                endColumn > std::numeric_limits<std::uint32_t>::max() || (endLine == *line && endColumn < column))
                return fail(ErrorCode::Corrupt, "Invalid SARIF end region");
            InspectionFinding finding;
            finding.file = *path;
            finding.line = static_cast<std::uint32_t>(*line);
            finding.column = static_cast<std::uint32_t>(column);
            finding.endLine = static_cast<std::uint32_t>(endLine);
            finding.endColumn = static_cast<std::uint32_t>(endColumn);
            finding.ruleId = id;
            finding.message = text;
            finding.severity = severity;
            finding.toolSeverity = severity == InspectionSeverity::Error  ? DiagnosticSeverity::Error
                                   : severity == InspectionSeverity::Info ? DiagnosticSeverity::Information
                                                                          : DiagnosticSeverity::Warning;
            report.findings.push_back(std::move(finding));
        }
    }
    return report;
}
} // namespace
Result<std::vector<InspectionReport>> importInspectionSarif(const Json& document) {
    if (document.stringAt("version") != "2.1.0")
        return fail(ErrorCode::Unsupported, "Only SARIF2.1.0 reports are supported");
    const auto* runs = document.find("runs");
    if (!runs || !runs->isArray() || runs->size() > 1024)
        return fail(ErrorCode::Corrupt, "SARIF runs must be a bounded array");
    std::vector<InspectionReport> reports;
    for (const auto& run : runs->items()) {
        auto imported = importRun(run);
        if (!imported)
            return std::unexpected(imported.error());
        reports.push_back(std::move(*imported));
    }
    return reports;
}
} // namespace frost::lang
