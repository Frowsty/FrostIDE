// SPDX-License-Identifier: Apache-2.0
// P1689 decoding and contained clang-scan-deps invocation for CPP-15.
#include "frost/lang/cpp_modules.hpp"

#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/cmake_compile_database.hpp"

#include <algorithm>
#include <set>
namespace frost::lang {
namespace {
constexpr std::size_t maxDatabaseBytes = 32U * 1024U * 1024U;

const std::string* textMember(const Json& row, std::string_view name) {
    const auto* value = row.find(name);
    const auto* text = value ? value->asString() : nullptr;
    return text && !text->empty() && text->find('\0') == text->npos ? text : nullptr;
}

Result<std::vector<std::string>> compileArguments(const Json& command) {
    const auto* arguments = command.find("arguments");
    const auto* line = command.find("command");
    if ((arguments == nullptr) == (line == nullptr))
        return fail(ErrorCode::Corrupt, "Module scan needs exactly one compilation command form");
    std::vector<std::string> words;
    if (arguments) {
        if (!arguments->isArray())
            return fail(ErrorCode::Corrupt, "Module compilation arguments must be an array");
        for (const auto& argument : arguments->items()) {
            const auto* text = argument.asString();
            if (!text)
                return fail(ErrorCode::Corrupt, "Module compilation arguments must be strings");
            words.push_back(*text);
        }
    } else {
        const auto* text = textMember(command, "command");
        if (!text)
            return fail(ErrorCode::Corrupt, "Module compilation command is empty or invalid");
        auto parsed = project::parseCompileCommandInvocation(*text, project::nativeCMakeShell());
        if (!parsed)
            return std::unexpected(parsed.error());
        words = std::move(*parsed);
    }
    if (words.empty() || !io::isAbsolutePath(words.front()))
        return fail(ErrorCode::Unsupported, "Module compiler must be absolute");
    for (const auto& word : words)
        if (word.find('\0') != word.npos || word.starts_with('@'))
            return fail(ErrorCode::Unsupported, "Module scan refuses unexpanded response files or invalid arguments");
    return words;
}

Result<std::string> absoluteModuleOutput(std::string_view directory, std::string_view output) {
    if (output.empty() || output == "." || output == ".." || output.starts_with('-') || output.ends_with('/') ||
        output.ends_with('\\'))
        return fail(ErrorCode::Unsupported, "Module compilation has no concrete output file");
    if (io::isAbsolutePath(output))
        return io::normalizePath(output);
    auto joined = io::joinPath(directory, output);
    if (!joined)
        return std::unexpected(joined.error());
    return io::normalizePath(*joined);
}

Result<std::string> compileOutput(const Json& command, std::string_view directory,
                                  const std::vector<std::string>& arguments) {
    const auto compilerPath = documentKey(arguments.front());
    const auto compiler = io::fileName(compilerPath);
    bool clMode = compiler == "clang-cl" || compiler == "clang-cl.exe" || compiler == "cl" || compiler == "cl.exe";
    for (const auto& argument : arguments)
        if (argument.starts_with("--driver-mode="))
            clMode = argument == "--driver-mode=cl";

    std::string_view output;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string_view arg = arguments[i];
        if (arg == "--")
            break;
        if (arg == "-Xclang" || arg == "-Xassembler" || arg == "-Xpreprocessor" || arg == "-Xlinker") {
            ++i;
            continue;
        }
        const std::string_view option = clMode ? (arg.starts_with('-') ? "-Fo" : "/Fo") : "-o";
        if (arg != option && !(clMode && arg.starts_with(option)))
            continue;
        if (!output.empty() || (arg == option && i + 1 == arguments.size()))
            return fail(ErrorCode::Corrupt, "Ambiguous module compiler output arguments");
        output = arg == option ? std::string_view(arguments[++i]) : arg.substr(option.size());
        if (output.empty())
            return fail(ErrorCode::Corrupt, "Module compiler output argument is empty");
    }
    if (command.find("output")) {
        const auto* explicitOutput = textMember(command, "output");
        if (!explicitOutput)
            return fail(ErrorCode::Corrupt, "Module compilation database output is invalid");
        auto explicitPath = absoluteModuleOutput(directory, *explicitOutput);
        if (!explicitPath || output.empty())
            return explicitPath;
        auto commandPath = absoluteModuleOutput(directory, output);
        if (!commandPath || documentKey(*explicitPath) != documentKey(*commandPath))
            return fail(ErrorCode::Corrupt, "Module compilation database and compiler outputs disagree");
        return explicitPath;
    }
    return absoluteModuleOutput(directory, output);
}

struct ModuleScannerDatabase {
    std::string bytes;
    std::set<std::string> outputs;
};

Result<ModuleScannerDatabase> scannerDatabase(std::string_view original) {
    if (original.size() > maxDatabaseBytes)
        return fail(ErrorCode::OutOfRange, "Module compilation database exceeds 32 MiB");
    auto database = parseJson(original);
    if (!database || !database->isArray() || database->size() == 0 || database->size() > 100000)
        return fail(ErrorCode::Corrupt, "Module scan requires a bounded nonempty captured compilation database");
    auto normalized = Json::array();
    ModuleScannerDatabase result;
    for (const auto& command : database->items()) {
        const auto* directory = textMember(command, "directory");
        const auto* file = textMember(command, "file");
        if (!directory || !file || !io::isAbsolutePath(*directory) || !io::isAbsolutePath(*file))
            return fail(ErrorCode::Unsupported, "Module scan needs absolute file and compilation directory paths");
        auto args = compileArguments(command);
        if (!args)
            return std::unexpected(args.error());
        auto output = compileOutput(command, *directory, *args);
        if (!output)
            return std::unexpected(output.error());
        if (!result.outputs.insert(documentKey(*output)).second)
            return fail(ErrorCode::Corrupt, "Duplicate output in module compilation database");
        auto argv = Json::array();
        for (auto& word : *args)
            argv.push(std::move(word));
        normalized.push(Json::object()
                            .set("directory", *directory)
                            .set("file", *file)
                            .set("arguments", std::move(argv))
                            .set("output", std::move(*output)));
    }
    result.bytes = normalized.dump();
    if (result.bytes.size() > maxDatabaseBytes)
        return fail(ErrorCode::OutOfRange, "Normalized module compilation database exceeds 32 MiB");
    return result;
}

struct RemoveScannerDatabase {
    io::IFileSystem& fs;
    const std::string& path;
    ~RemoveScannerDatabase() { (void)fs.removeFile(path); }
};
} // namespace
Result<CppModuleGraph> parseCppModuleGraph(std::string_view report) {
    if (report.size() > 32U * 1024U * 1024U)
        return fail(ErrorCode::Unsupported, "Module dependency report exceeds 32 MiB");
    auto parsed = parseJson(report);
    if (!parsed)
        return std::unexpected(parsed.error());
    const auto* rules = parsed->find("rules");
    if (!parsed->isObject() || parsed->integerAt("version") != 1 || parsed->integerAt("revision") != 0 || !rules ||
        !rules->isArray() || rules->size() > 100000)
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
            if (!entries)
                continue;
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
                    rule.requiresModules.push_back(
                        {*logical, source ? *source : "", method ? *method : "by-name", onPath});
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
    if (targets.size() != 1)
        targets.clear();
    return targets;
}
Result<void> validateCppModuleScannerVersion(std::string_view banner) {
    const std::string expected = "LLVM version " FROST_PINNED_LLVM_VERSION;
    while (!banner.empty()) {
        const auto end = banner.find('\n');
        auto line = banner.substr(0, end);
        const auto start = line.find_first_not_of(" \t");
        if (start != line.npos)
            line.remove_prefix(start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line == expected ||
            (line.starts_with(expected) && line.size() > expected.size() && line[expected.size()] == ' '))
            return {};
        if (end == banner.npos)
            break;
        banner.remove_prefix(end + 1);
    }
    return fail(ErrorCode::Unsupported, "Module scanner is not the pinned LLVM clang-scan-deps");
}
Result<CppModuleGraph> scanCppModules(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                      const CppModuleScanRequest& request) {
    if (!request.trusted)
        return fail(ErrorCode::PermissionDenied, "Trust the project before scanning C++ module commands");
    if (request.scanner.empty())
        return fail(ErrorCode::NotFound, "Pinned clang-scan-deps tool unavailable");
    if (!io::isAbsolutePath(request.scanner) || !io::isAbsolutePath(request.privateDatabasePath) ||
        !io::isAbsolutePath(request.workingDirectory) || request.databaseBytes.empty() || !request.validateCurrent)
        return fail(ErrorCode::InvalidArgument,
                    "Module scan requires a pinned scanner and captured compilation context");
    const auto scannerName = io::fileName(request.scanner);
    if (scannerName != "clang-scan-deps" && scannerName != "clang-scan-deps.exe")
        return fail(ErrorCode::Unsupported, "Module scanner executable must be clang-scan-deps");
    if (auto current = request.validateCurrent(); !current)
        return std::unexpected(current.error());
    auto validateDatabase = [&]() -> Result<void> {
        auto bytes = fs.readFileSnapshot(request.privateDatabasePath, maxDatabaseBytes);
        if (!bytes)
            return std::unexpected(bytes.error());
        if (*bytes != request.databaseBytes)
            return fail(ErrorCode::Cancelled, "Captured module compilation database changed");
        return request.validateCurrent();
    };
    auto database = scannerDatabase(request.databaseBytes);
    if (!database)
        return std::unexpected(database.error());
    if (auto current = validateDatabase(); !current)
        return std::unexpected(current.error());
    io::ProcessSpec spec;
    spec.executable = request.scanner;
    spec.workingDirectory = request.workingDirectory;
    spec.arguments = {"--version"};
    auto version = io::runToCompletion(launcher, spec, "", request.timeout, request.stop);
    if (!version)
        return std::unexpected(version.error());
    const auto versionText = version->stdoutBytes + version->stderrBytes;
    if (version->exitCode != 0)
        return fail(ErrorCode::Unsupported, "Module scanner version probe failed");
    if (auto pinned = validateCppModuleScannerVersion(versionText); !pinned)
        return std::unexpected(pinned.error());
    // The P1689 driver takes primary-output from the JSON field, including when -o or /Fo is present.
    // Keep the original bytes for admission and freshness, and stage the scanner's explicit output separately.
    auto scannerPath = io::joinPath(io::parentPath(request.privateDatabasePath), "compile_commands.p1689.json");
    if (!scannerPath)
        return std::unexpected(scannerPath.error());
    auto exists = fs.exists(*scannerPath);
    if (!exists)
        return std::unexpected(exists.error());
    if (*exists)
        return fail(ErrorCode::AlreadyExists, "Module scanner database path is already in use");
    if (auto written = fs.writeFileAtomic(*scannerPath, database->bytes); !written)
        return std::unexpected(written.error());
    const RemoveScannerDatabase cleanup{fs, *scannerPath};
    const auto validateScannerDatabase = [&]() -> Result<void> {
        auto bytes = fs.readFileSnapshot(*scannerPath, maxDatabaseBytes);
        if (!bytes)
            return std::unexpected(bytes.error());
        if (*bytes != database->bytes)
            return fail(ErrorCode::Cancelled, "Normalized module compilation database changed");
        return {};
    };
    if (auto current = validateDatabase(); !current)
        return std::unexpected(current.error());
    if (auto current = validateScannerDatabase(); !current)
        return std::unexpected(current.error());
    spec.arguments = {"-format=p1689", "-compilation-database=" + *scannerPath};
    auto result = io::runToCompletion(launcher, spec, "", request.timeout, request.stop);
    if (!result)
        return std::unexpected(result.error());
    if (auto current = request.validateCurrent(); !current)
        return std::unexpected(current.error());
    if (auto current = validateDatabase(); !current)
        return std::unexpected(current.error());
    if (auto current = validateScannerDatabase(); !current)
        return std::unexpected(current.error());
    if (result->exitCode != 0)
        return fail(ErrorCode::IoError, "clang-scan-deps failed: " + result->stderrBytes);
    auto graph = parseCppModuleGraph(result->stdoutBytes);
    if (!graph)
        return std::unexpected(graph.error());
    // P1689 paths can be command-relative. Multiple CDB working directories make a
    // single invocation-directory fallback unsound; require absolute scanner paths.
    for (const auto& rule : graph->rules) {
        if (!io::isAbsolutePath(rule.primaryOutput) || database->outputs.erase(documentKey(rule.primaryOutput)) != 1)
            return fail(ErrorCode::Corrupt, "Scanner returned an unknown or duplicate module output");
        for (const auto& provider : rule.provides)
            if (!provider.sourcePath.empty() && !io::isAbsolutePath(provider.sourcePath))
                return fail(ErrorCode::Unsupported, "Scanner returned a relative interface path");
        for (const auto& dependency : rule.requiresModules)
            if (!dependency.sourcePath.empty() && !io::isAbsolutePath(dependency.sourcePath))
                return fail(ErrorCode::Unsupported, "Scanner returned a relative dependency path");
    }
    if (!database->outputs.empty())
        return fail(ErrorCode::Corrupt, "Scanner report omitted a captured module output");
    return graph;
}
} // namespace frost::lang
