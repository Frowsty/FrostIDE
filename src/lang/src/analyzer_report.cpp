// SPDX-License-Identifier: Apache-2.0
// Validate actual analyzer DTOs/dependencies before profile filtering; no fallback/synthetic results.
#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/analyzer.hpp"
#include "frost/lang/cpp_index.hpp"
#include "frost/lang/uri.hpp"

#include <algorithm>
#include <limits>
#include <tuple>
namespace frost::lang::analyzer_detail {
namespace {
Result<void> bad(std::string reason) { return fail(ErrorCode::Corrupt, "Native analyzer: " + std::move(reason)); }
const AnalyzerInput* image(const AnalyzerReport& report, std::string_view path) {
    const auto found = std::ranges::find_if(
        report.inputs, [&](const auto& input) { return documentKey(input.path) == documentKey(path); });
    return found == report.inputs.end() ? nullptr : &*found;
}
DiagnosticSeverity severity(std::string_view value) {
    return value == "error"                     ? DiagnosticSeverity::Error
           : value == "note" || value == "none" ? DiagnosticSeverity::Information
                                                : DiagnosticSeverity::Warning;
}
} // namespace
Result<void> append(AnalyzerReport& report, const Json& body, const AnalyzerRequest& request,
                    const std::string& database, std::size_t ordinal, const std::string& source, io::IFileSystem& fs) {
    const auto status = body.stringAt("status");
    if (status != "ready" && status != "partial") {
        ++report.failedConfigurations;
        std::string reason = source + ": " + std::string(status);
        if (const auto* messages = body.find("diagnostics"); messages && messages->isArray())
            for (const auto& value : messages->items())
                if (auto message = value.asString())
                    reason += "\n" + *message;
        report.limitations.push_back(std::move(reason));
        return {};
    }
    if (body.unsignedAt("commandIndex") != ordinal ||
        body.stringAt("configuration") != request.compilationDatabasePath ||
        body.stringAt("compilationDatabaseSha256") != io::sha256Hex(database) ||
        documentKey(body.stringAt("source")) != documentKey(source) || body.stringAt("commandIdentity").size() != 64)
        return bad("configuration receipt differs from captured request");
    const auto *registry = body.find("rules"), *dependencies = body.find("dependencies"),
               *findings = body.find("findings"), *limitations = body.find("limitations");
    if (!registry || !registry->isArray() || registry->size() > 256 || !dependencies || !dependencies->isArray() ||
        dependencies->size() > 32768 || !findings || !findings->isArray() || findings->size() > 8192 || !limitations ||
        !limitations->isArray() || limitations->size() > 1024 || body.stringAt("columnKind") != "unicodeCodePoints")
        return bad("malformed rules, findings, limitations or source coordinate schema");
    if ((status == "ready") != limitations->items().empty())
        return bad("completion status disagrees with actual function limitations");
    std::vector<AnalyzerRuleInfo> rules;
    for (const auto& value : registry->items()) {
        AnalyzerRuleInfo rule{std::string(value.stringAt("id")),
                              std::string(value.stringAt("title")),
                              std::string(value.stringAt("category")),
                              std::string(value.stringAt("defaultLevel")),
                              std::string(value.stringAt("applicability")),
                              std::string(value.stringAt("settingKey"))};
        if (rule.id.empty() || rule.title.empty() || rule.applicability.empty() ||
            (rule.level != "warning" && rule.level != "error" && rule.level != "note"))
            return bad("invalid rule metadata");
        if (std::ranges::any_of(rules, [&](const auto& prior) { return prior.id == rule.id; }))
            return bad("duplicate rule ID");
        rules.push_back(std::move(rule));
    }
    if (report.rules.empty())
        report.rules = rules;
    else {
        if (report.rules.size() != rules.size())
            return bad("rule registry changed during scope");
        for (std::size_t i = 0; i < rules.size(); ++i)
            if (std::tie(report.rules[i].id, report.rules[i].level, report.rules[i].settingKey) !=
                std::tie(rules[i].id, rules[i].level, rules[i].settingKey))
                return bad("rule metadata changed during scope");
    }
    for (const auto& dependency : dependencies->items()) {
        const std::string path(dependency.stringAt("path")), hash(dependency.stringAt("sha256"));
        const auto size = dependency.unsignedAt("bytes");
        if (!io::isAbsolutePath(path) || hash.size() != 64 || !size || *size > 64U * 1024U * 1024U)
            return bad("invalid dependency receipt");
        if (const auto* existing = image(report, path)) {
            if (existing->bytes.size() != *size || io::sha256Hex(existing->bytes) != hash)
                return fail(ErrorCode::Busy, "Analyzer configurations observed different input images: " + path);
            continue;
        }
        const auto overlay = std::ranges::find_if(
            request.overlays, [&](const auto& value) { return documentKey(value.path) == documentKey(path); });
        const bool isOverlay = overlay != request.overlays.end();
        auto input = [&]() -> Result<CppCompilerInput> {
            if (!isOverlay)
                return readCppCompilerInput(fs, path);
            auto physical = fs.physicalPath(path);
            if (!physical)
                return std::unexpected(physical.error());
            return CppCompilerInput{std::move(*physical), overlay->contents};
        }();
        if (!input || input->bytes.size() != *size || io::sha256Hex(input->bytes) != hash)
            return fail(ErrorCode::Busy, "Analyzer dependency changed: " + path);
        std::size_t total = input->bytes.size();
        for (const auto& prior : report.inputs)
            total += prior.bytes.size();
        if (total > 512U * 1024U * 1024U || report.inputs.size() >= 32768)
            return fail(ErrorCode::OutOfRange, "Analyzer scope inputs exceed512MiB/32768 files");
        report.inputs.push_back({path, std::move(input->bytes), std::move(input->canonicalPath), isOverlay});
    }
    for (const auto key : {"missingDependencies", "directoryDependencies"}) {
        const auto* values = body.find(key);
        if (!values || !values->isArray() || values->size() > 32768)
            return bad("invalid namespace receipts");
        auto& target = std::string_view(key) == "missingDependencies" ? report.missing : report.directories;
        for (const auto& value : values->items()) {
            auto path = value.asString();
            if (!path || !io::isAbsolutePath(*path))
                return bad("invalid namespace path");
            if (std::ranges::find(target, *path) == target.end())
                target.push_back(*path);
            if (target.size() > 32768)
                return fail(ErrorCode::OutOfRange, "Analyzer namespace budget exceeded");
        }
    }
    for (const auto& value : limitations->items()) {
        const auto reason = value.stringAt("reason");
        if (reason.empty())
            return bad("empty function limitation");
        report.limitations.push_back(std::string(value.stringAt("function")) + ": " + std::string(reason));
    }
    for (const auto& value : findings->items()) {
        const std::string id(value.stringAt("ruleId")), path(value.stringAt("path")),
            message(value.stringAt("message"));
        const auto rule = std::ranges::find(rules, id, &AnalyzerRuleInfo::id);
        const auto* before = image(report, path);
        const auto offset = value.unsignedAt("offset"), length = value.unsignedAt("length");
        const auto line = value.unsignedAt("line"), column = value.unsignedAt("column"),
                   endLine = value.unsignedAt("endLine"), endColumn = value.unsignedAt("endColumn");
        if (rule == rules.end() || !before || message.empty() || !offset || !length || *offset > before->bytes.size() ||
            *length > before->bytes.size() - *offset || !line || !column || !endLine || !endColumn || !*line ||
            !*column || !*endColumn || *column > std::numeric_limits<std::uint32_t>::max() || *endLine < *line ||
            (*endLine == *line && *endColumn < *column) || *endLine > std::numeric_limits<std::uint32_t>::max() ||
            *endColumn > std::numeric_limits<std::uint32_t>::max())
            return bad("finding does not belong to its exact captured source");
        const auto toolSeverity = severity(rule->level);
        const auto level = request.profile.effectiveSeverity(id, toolSeverity);
        if (level == InspectionSeverity::Disabled)
            continue;
        InspectionFinding finding;
        finding.file = path;
        finding.line = static_cast<std::uint32_t>(*line);
        finding.column = static_cast<std::uint32_t>(*column);
        finding.endLine = static_cast<std::uint32_t>(*endLine);
        finding.endColumn = static_cast<std::uint32_t>(*endColumn);
        finding.ruleId = id;
        finding.message = message;
        finding.severity = level;
        finding.toolSeverity = toolSeverity;
        if (std::ranges::none_of(report.inspection.findings, [&](const auto& prior) {
                return prior.file == finding.file && prior.line == finding.line && prior.column == finding.column &&
                       prior.ruleId == finding.ruleId && prior.message == finding.message;
            }))
            report.inspection.findings.push_back(std::move(finding));
        if (const auto* replacement = value.find("replacement")) {
            if (!replacement->asString())
                return bad("replacement is not source text");
            report.fixes.push_back({id, path, *replacement->asString(), *offset, *length});
        }
    }
    report.inspection.toolVersion = std::string(body.stringAt("llvmVersion"));
    ++report.completedConfigurations;
    return {};
}
void reconcileFixes(AnalyzerReport& report, const std::vector<Json>& units) {
    std::vector<AnalyzerFix> accepted;
    for (const auto& fix : report.fixes) {
        bool agreed = true;
        for (const auto& unit : units) {
            const auto* inputs = unit.find("dependencies");
            if (!inputs || !inputs->isArray()) {
                agreed = false;
                break;
            }
            if (std::ranges::none_of(inputs->items(), [&](const auto& input) {
                    return documentKey(input.stringAt("path")) == documentKey(fix.path);
                }))
                continue;
            const auto* findings = unit.find("findings");
            if (!findings || std::ranges::none_of(findings->items(), [&](const auto& value) {
                    const auto* replacement = value.find("replacement");
                    return replacement && replacement->asString() && value.stringAt("ruleId") == fix.ruleId &&
                           documentKey(value.stringAt("path")) == documentKey(fix.path) &&
                           value.unsignedAt("offset") == fix.offset && value.unsignedAt("length") == fix.length &&
                           value.stringAt("replacement") == fix.replacement;
                })) {
                agreed = false;
                break;
            }
        }
        if (agreed && std::ranges::none_of(accepted, [&](const auto& prior) {
                return std::tie(prior.ruleId, prior.path, prior.replacement, prior.offset, prior.length) ==
                       std::tie(fix.ruleId, fix.path, fix.replacement, fix.offset, fix.length);
            }))
            accepted.push_back(fix);
    }
    report.fixes = std::move(accepted);
}
} // namespace frost::lang::analyzer_detail
