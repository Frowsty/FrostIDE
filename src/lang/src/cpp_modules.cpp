// SPDX-License-Identifier: Apache-2.0
// P1689 decoding and contained clang-scan-deps invocation for CPP-15.
#include "frost/lang/cpp_modules.hpp"
#include "frost/io/path.hpp"
#include <algorithm>
#include <set>
namespace frost::lang {
namespace {
const std::string* textMember(const Json& row, std::string_view name) {
    const auto* value = row.find(name);
    const auto* text = value ? value->asString() : nullptr;
    return text && !text->empty() && text->find('\0') == text->npos ? text : nullptr;
}
}
Result<CppModuleGraph> parseCppModuleGraph(std::string_view report) {
    if (report.size() > 32U * 1024U * 1024U)
        return fail(ErrorCode::Unsupported, "Module dependency report exceeds 32 MiB");
    auto parsed = parseJson(report);
    if (!parsed) return std::unexpected(parsed.error());
    const auto* rules = parsed->find("rules");
    if (!parsed->isObject() || parsed->integerAt("version") != 1 ||
        parsed->integerAt("revision") != 0 || !rules || !rules->isArray() || rules->size() > 100000)
        return fail(ErrorCode::Corrupt, "Unsupported or malformed P1689 envelope");
    CppModuleGraph graph;
    std::set<std::string> outputs;
    for (const auto& row : rules->items()) {
        const auto* output = textMember(row, "primary-output");
        if (!row.isObject() || !output || !outputs.insert(*output).second)
            return fail(ErrorCode::Corrupt, "Missing or duplicate module rule output");
        CppModuleRule rule;
        rule.primaryOutput = *output;
        for (const auto name : {"provides", "requires"}) {
            const auto* entries = row.find(name);
            if (!entries) continue;
            if (!entries->isArray() || entries->size() > 100000)
                return fail(ErrorCode::Corrupt, "Malformed module dependency list");
            std::set<std::string> identities;
            for (const auto& entry : entries->items()) {
                const auto* logical = textMember(entry, "logical-name");
                if (!entry.isObject() || !logical || !identities.insert(*logical).second)
                    return fail(ErrorCode::Corrupt, "Missing or duplicate module identity");
                const auto* sourceValue = entry.find("source-path");
                const auto* source = textMember(entry, "source-path");
                if (sourceValue && !source)
                    return fail(ErrorCode::Corrupt, "Invalid module source path");
                if (std::string_view(name) == "provides") {
                    const auto* interface = entry.find("is-interface");
                    if (!interface || !interface->asBool())
                        return fail(ErrorCode::Corrupt, "Missing module interface classification");
                    if (*interface->asBool() && !source)
                        return fail(ErrorCode::Corrupt, "Module interface has no source path");
                    rule.provides.push_back({*logical, source ? *source : "", *interface->asBool()});
                } else {
                    const auto* unique = entry.find("unique-on-source-path");
                    if (unique && !unique->asBool())
                        return fail(ErrorCode::Corrupt, "Invalid module source identity flag");
                    const bool onPath = unique && *unique->asBool();
                    const auto* method = textMember(entry, "lookup-method");
                    if ((entry.find("lookup-method") && !method) || (onPath && !source))
                        return fail(ErrorCode::Corrupt, "Invalid module dependency lookup");
                    if (method && *method != "by-name" && *method != "include-angle" && *method != "include-quote")
                        return fail(ErrorCode::Unsupported, "Unknown module dependency lookup method");
                    rule.requiresModules.push_back({*logical, source ? *source : "", method ? *method : "by-name", onPath});
                }
            }
        }
        graph.rules.push_back(std::move(rule));
    }
    graph.rawP1689 = report;
    return graph;
}
std::vector<std::string> CppModuleGraph::interfaceTargets(const CppModuleRequire& dependency) const {
    std::vector<std::string> targets;
    for (const auto& rule : rules)
        for (const auto& provider : rule.provides)
            if (provider.interface && provider.name == dependency.name &&
                (!dependency.uniqueOnSourcePath || provider.sourcePath == dependency.sourcePath))
                targets.push_back(provider.sourcePath);
    std::ranges::sort(targets);
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    if (targets.size() != 1) targets.clear();
    return targets;
}
Result<void> validateCppModuleScannerVersion(std::string_view banner) {
    const std::string expected = "LLVM version " FROST_PINNED_LLVM_VERSION;
    while (!banner.empty()) {
        const auto end = banner.find('\n');
        auto line = banner.substr(0, end);
        const auto start = line.find_first_not_of(" \t");
        if (start != line.npos) line.remove_prefix(start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line == expected || (line.starts_with(expected) && line.size() > expected.size() &&
                                line[expected.size()] == ' '))
            return {};
        if (end == banner.npos) break;
        banner.remove_prefix(end + 1);
    }
    return fail(ErrorCode::Unsupported, "Module scanner is not the pinned LLVM clang-scan-deps");
}
Result<CppModuleGraph> scanCppModules(io::IProcessLauncher& launcher, io::IFileSystem& fs, const CppModuleScanRequest& request) {
    if (!request.trusted)
        return fail(ErrorCode::PermissionDenied, "Trust the project before scanning C++ module commands");
    if (request.scanner.empty())
        return fail(ErrorCode::NotFound, "Pinned clang-scan-deps tool unavailable");
    if (!io::isAbsolutePath(request.scanner) || !io::isAbsolutePath(request.privateDatabasePath) ||
        !io::isAbsolutePath(request.workingDirectory) || request.databaseBytes.empty() || !request.validateCurrent)
        return fail(ErrorCode::InvalidArgument, "Module scan requires a pinned scanner and captured compilation context");
    const auto scannerName = io::fileName(request.scanner);
    if (scannerName != "clang-scan-deps" && scannerName != "clang-scan-deps.exe")
        return fail(ErrorCode::Unsupported, "Module scanner executable must be clang-scan-deps");
    if (auto current = request.validateCurrent(); !current) return std::unexpected(current.error());
    auto validateDatabase = [&]() -> Result<void> {
        auto bytes = fs.readFileSnapshot(request.privateDatabasePath, 32U * 1024U * 1024U);
        if (!bytes) return std::unexpected(bytes.error());
        if (*bytes != request.databaseBytes)
            return fail(ErrorCode::Cancelled, "Captured module compilation database changed");
        return request.validateCurrent();
    };
    auto database = parseJson(request.databaseBytes);
    if (!database || !database->isArray() || database->size() == 0)
        return fail(ErrorCode::Corrupt, "Module scan requires a nonempty captured compilation database");
    for (const auto& command : database->items()) {
        const auto* directory = textMember(command, "directory");
        const auto* file = textMember(command, "file");
        const auto* arguments = command.find("arguments");
        if (!directory || !file || !io::isAbsolutePath(*directory) || !io::isAbsolutePath(*file) ||
            !arguments || !arguments->isArray() || arguments->size() == 0 || command.find("command"))
            return fail(ErrorCode::Unsupported, "Module scan needs absolute arguments-form compilation commands");
        for (const auto& argument : arguments->items()) {
            const auto* text = argument.asString();
            if (!text || text->find('\0') != text->npos || text->starts_with('@'))
                return fail(ErrorCode::Unsupported, "Module scan refuses unexpanded response files or invalid arguments");
        }
        if (!io::isAbsolutePath(*arguments->items().front().asString()))
            return fail(ErrorCode::Unsupported, "Module compiler must be absolute");
    }
    if (auto current = validateDatabase(); !current) return std::unexpected(current.error());
    io::ProcessSpec spec;
    spec.executable = request.scanner;
    spec.workingDirectory = request.workingDirectory;
    spec.arguments = {"--version"};
    auto version = io::runToCompletion(launcher, spec, "", request.timeout, request.stop);
    if (!version) return std::unexpected(version.error());
    const auto versionText = version->stdoutBytes + version->stderrBytes;
    if (version->exitCode != 0)
        return fail(ErrorCode::Unsupported, "Module scanner version probe failed");
    if (auto pinned = validateCppModuleScannerVersion(versionText); !pinned)
        return std::unexpected(pinned.error());
    if (auto current = validateDatabase(); !current) return std::unexpected(current.error());
    spec.arguments = {"-format=p1689", "-compilation-database=" + request.privateDatabasePath};
    auto result = io::runToCompletion(launcher, spec, "", request.timeout, request.stop);
    if (!result) return std::unexpected(result.error());
    if (auto current = request.validateCurrent(); !current) return std::unexpected(current.error());
    if (auto current = validateDatabase(); !current) return std::unexpected(current.error());
    if (result->exitCode != 0)
        return fail(ErrorCode::IoError, "clang-scan-deps failed: " + result->stderrBytes);
    auto graph = parseCppModuleGraph(result->stdoutBytes);
    if (!graph) return std::unexpected(graph.error());
    // P1689 paths can be command-relative. Multiple CDB working directories make a
    // single invocation-directory fallback unsound; require absolute scanner paths.
    for (const auto& rule : graph->rules) {
        for (const auto& provider : rule.provides)
            if (!provider.sourcePath.empty() && !io::isAbsolutePath(provider.sourcePath))
                return fail(ErrorCode::Unsupported, "Scanner returned a relative interface path");
        for (const auto& dependency : rule.requiresModules)
            if (!dependency.sourcePath.empty() && !io::isAbsolutePath(dependency.sourcePath))
                return fail(ErrorCode::Unsupported, "Scanner returned a relative dependency path");
    }
    return graph;
}
} // namespace frost::lang
