// SPDX-License-Identifier: Apache-2.0
//
// The MSBuild-evaluated compile-commands provider (msbuild_compile_commands.hpp): the evaluation
// command line, its JSON, the -pp import list, the clang-cl commands, the per-project cache and
// the provider chain of ADR 0041.

#include "frost/lang/msbuild_compile_commands.hpp"

#include "frost/core/log.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/solution_model.hpp"

#include "msvc_flags.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <set>
#include <unordered_map>

namespace frost::lang {
namespace {

using namespace detail; // msvc_flags.hpp

constexpr std::string_view kLogCategory = "lang";
constexpr int kCacheVersion = 1;

[[nodiscard]] std::string escapeMsBuildValue(std::string_view value) {
    std::string out;
    for (const char c : value) {
        switch (c) {
        case '%': out += "%25"; break;
        case ';': out += "%3B"; break;
        case ',': out += "%2C"; break;
        default: out += c;
        }
    }
    return out;
}

// ';'-separated entries, trimmed, without empty ones (evaluated values hold no references).
[[nodiscard]] std::vector<std::string> listEntries(std::string_view list) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t end = std::min(list.find(';', start), list.size());
        const std::string_view entry = trim(list.substr(start, end - start));
        if (!entry.empty()) {
            out.emplace_back(entry);
        }
        start = end + 1;
    }
    return out;
}

// Command-line words of AdditionalOptions: whitespace-separated, double quotes group.
[[nodiscard]] std::vector<std::string> splitOptions(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    bool any = false;
    for (const char c : text) {
        if (c == '"') {
            quoted = !quoted;
            any = true;
        } else if (!quoted && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
            if (any) {
                out.push_back(std::move(current));
                current.clear();
                any = false;
            }
        } else {
            current += c;
            any = true;
        }
    }
    if (any) {
        out.push_back(std::move(current));
    }
    return out;
}

// Switches clangd must not act on: PCH creation/use, output files, parallel builds.
[[nodiscard]] bool droppedOption(std::string_view option) noexcept {
    if (option.size() < 2 || (option[0] != '/' && option[0] != '-')) {
        return false;
    }
    const std::string_view name = option.substr(1);
    return name.starts_with('Y') || name.starts_with("Fp") || name.starts_with("Fo") || name.starts_with("Fd") ||
           name.starts_with("MP");
}

[[nodiscard]] std::string valueOf(const std::map<std::string, std::string, std::less<>>& map, std::string_view key) {
    auto it = map.find(key);
    return it != map.end() ? std::string(trim(it->second)) : std::string();
}

[[nodiscard]] std::optional<std::string> exceptionFlag(std::string_view value) {
    const std::string v = lower(value);
    if (v == "false") {
        return std::nullopt;
    }
    if (v == "async") {
        return "/EHa";
    }
    if (v == "syncctrow" || v == "synccthrow") {
        return "/EHs";
    }
    return "/EHsc"; // Sync and MSVC's default.
}

[[nodiscard]] std::uint64_t fnv1a(std::string_view text) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

[[nodiscard]] std::string join(std::string_view base, std::string_view name) {
    auto joined = io::joinPath(base, name);
    return joined ? std::move(*joined) : std::string(base);
}

// File status for cache keys: size and modification time, or size -1 for a missing file.
struct FileStamp {
    std::int64_t size = -1;
    std::int64_t mtime = 0;
    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

class StampCache {
public:
    explicit StampCache(io::IFileSystem& fs) : m_fs(fs) {}
    FileStamp stamp(const std::string& path) {
        auto [it, inserted] = m_stamps.try_emplace(documentKey(path));
        if (inserted) {
            if (auto status = m_fs.status(path)) {
                it->second.size = static_cast<std::int64_t>(status->size);
                it->second.mtime =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(status->mtime.time_since_epoch()).count();
            }
        }
        return it->second;
    }

private:
    io::IFileSystem& m_fs;
    std::unordered_map<std::string, FileStamp> m_stamps;
};

[[nodiscard]] Json commandToJson(const CompileCommand& command) {
    Json arguments = Json::array();
    for (const std::string& argument : command.arguments) {
        arguments.push(argument);
    }
    return Json::object()
        .set("directory", command.directory)
        .set("file", command.file)
        .set("arguments", std::move(arguments));
}

[[nodiscard]] std::optional<CompileCommand> commandFromJson(const Json& json) {
    CompileCommand command;
    command.directory = std::string(json.stringAt("directory"));
    command.file = std::string(json.stringAt("file"));
    const Json* arguments = json.find("arguments");
    if (command.file.empty() || arguments == nullptr) {
        return std::nullopt;
    }
    for (const Json& argument : arguments->items()) {
        if (const std::string* text = argument.asString()) {
            command.arguments.push_back(*text);
        }
    }
    return command;
}

[[nodiscard]] std::string firstLines(std::string_view text, std::size_t count) {
    std::string out;
    std::size_t start = 0;
    while (count > 0 && start < text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = trim(text.substr(start, end - start));
        if (!line.empty()) {
            if (!out.empty()) {
                out += " | ";
            }
            out += line;
            --count;
        }
        start = end + 1;
    }
    return out;
}

} // namespace

std::vector<std::string> evaluatedPropertyNames() {
    return {"IncludePath", "ExternalIncludePath", "MSBuildAllProjects", "Configuration", "Platform", "ProjectDir"};
}

Result<ProjectEvaluation> parseMsBuildEvaluation(std::string_view output) {
    const std::size_t brace = output.find('{');
    if (brace == std::string_view::npos) {
        return fail(ErrorCode::Corrupt, std::format("MSBuild printed no JSON: {}", firstLines(output, 3)));
    }
    auto json = parseJson(output.substr(brace));
    if (!json || !json->isObject()) {
        return fail(ErrorCode::Corrupt, "MSBuild's evaluation output is not a JSON object");
    }
    ProjectEvaluation out;
    if (const Json* properties = json->find("Properties"); properties != nullptr) {
        for (std::size_t i = 0; i < properties->size(); ++i) {
            if (const std::string* value = properties->items()[i].asString()) {
                out.properties.emplace(lower(properties->keys()[i]), *value);
            }
        }
    }
    const Json* items = json->find("Items");
    const Json* clCompile = items != nullptr ? items->find("ClCompile") : nullptr;
    if (clCompile != nullptr) {
        for (const Json& item : clCompile->items()) {
            EvaluatedItem evaluated;
            for (std::size_t i = 0; i < item.size(); ++i) {
                if (const std::string* value = item.items()[i].asString()) {
                    evaluated.metadata.emplace(lower(item.keys()[i]), *value);
                }
            }
            evaluated.fullPath = valueOf(evaluated.metadata, "fullpath");
            if (!evaluated.fullPath.empty()) {
                out.clCompile.push_back(std::move(evaluated));
            }
        }
    }
    return out;
}

Result<io::ProcessSpec> msbuildEvaluationSpec(const MsBuildEvaluationRequest& request) {
    if (!io::isAbsolutePath(request.msbuildPath) || !io::isAbsolutePath(request.projectPath)) {
        return fail(ErrorCode::InvalidArgument, "MSBuild and the project need absolute paths");
    }
    if (!request.solutionPath.empty() && !io::isAbsolutePath(request.solutionPath)) {
        return fail(ErrorCode::InvalidArgument, std::format("{} is not absolute", request.solutionPath));
    }
    if (request.configuration.empty() || request.platform.empty()) {
        return fail(ErrorCode::InvalidArgument, "an evaluation needs a configuration and a platform");
    }
    io::ProcessSpec spec;
    spec.executable = request.msbuildPath;
    spec.arguments = {request.projectPath, "-nologo", "-nodeReuse:false"};
    if (request.preprocessTo.empty()) {
        spec.arguments.emplace_back("-getItem:ClCompile");
        std::string properties = "-getProperty:";
        const std::vector<std::string> names = evaluatedPropertyNames();
        for (std::size_t i = 0; i < names.size(); ++i) {
            properties += (i > 0 ? ";" : "") + names[i];
        }
        spec.arguments.push_back(std::move(properties));
    } else {
        spec.arguments.push_back("-pp:" + request.preprocessTo);
    }
    spec.arguments.push_back("-p:Configuration=" + escapeMsBuildValue(request.configuration));
    spec.arguments.push_back("-p:Platform=" + escapeMsBuildValue(request.platform));
    spec.arguments.emplace_back("-p:DesignTimeBuild=true");
    if (!request.solutionPath.empty()) {
        // The Solution* properties Visual Studio passes, so $(SolutionDir) paths resolve.
        std::string directory(io::parentPath(request.solutionPath));
        if (!directory.empty() && directory.back() != '\\' && directory.back() != '/') {
            directory += '\\';
        }
        const std::string_view file = io::fileName(request.solutionPath);
        const std::size_t dot = file.find_last_of('.');
        spec.arguments.push_back("-p:SolutionDir=" + escapeMsBuildValue(directory));
        spec.arguments.push_back("-p:SolutionPath=" + escapeMsBuildValue(request.solutionPath));
        spec.arguments.push_back("-p:SolutionName=" + escapeMsBuildValue(file.substr(0, dot)));
        spec.arguments.push_back("-p:SolutionFileName=" + escapeMsBuildValue(file));
        spec.arguments.push_back("-p:SolutionExt=" +
                                 escapeMsBuildValue(dot == std::string_view::npos ? "" : file.substr(dot)));
    }
    spec.workingDirectory = std::string(io::parentPath(request.projectPath));
    spec.environment = {
        {"DOTNET_CLI_UI_LANGUAGE", "en-US"}, {"MSBUILDTERMINALLOGGER", "off"}, {"MSBUILDDISABLENODEREUSE", "1"}};
    spec.environment.insert(spec.environment.end(), request.environment.begin(), request.environment.end());
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    return spec;
}

std::vector<std::string> parsePreprocessedImports(std::string_view preprocessed) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    bool inBanner = false;
    std::size_t start = 0;
    while (start < preprocessed.size()) {
        const std::size_t end = std::min(preprocessed.find('\n', start), preprocessed.size());
        const std::string_view line = trim(preprocessed.substr(start, end - start));
        start = end + 1;
        if (line.starts_with("=====")) {
            inBanner = !inBanner; // A banner's rule lines open and close it.
            continue;
        }
        if (line == "-->") {
            inBanner = false;
            continue;
        }
        if (!inBanner || line.empty() || line.starts_with('<')) {
            continue;
        }
        if (io::isAbsolutePath(line, io::PathStyle::Windows) && seen.insert(lower(line)).second) {
            out.emplace_back(line);
        }
    }
    return out;
}

std::vector<CompileCommand> commandsFromEvaluation(const ProjectEvaluation& evaluation,
                                                   std::string_view projectDirectory, std::string_view configuration,
                                                   std::string_view platform, std::string_view compiler) {
    std::vector<std::string> system;
    std::set<std::string> seenSystem;
    for (const char* property : {"includepath", "externalincludepath"}) {
        for (const std::string& entry : listEntries(valueOf(evaluation.properties, property))) {
            if (auto dir = resolvePath(projectDirectory, entry); dir && seenSystem.insert(lower(*dir)).second) {
                system.push_back(std::move(*dir));
            }
        }
    }
    std::vector<CompileCommand> out;
    for (const EvaluatedItem& item : evaluation.clCompile) {
        const auto& m = item.metadata;
        if (iequals(valueOf(m, "excludedfrombuild"), "true")) {
            continue;
        }
        CompileCommand command{.directory = std::string(projectDirectory), .file = {}, .arguments = {}};
        command.file = resolvePath(projectDirectory, item.fullPath).value_or(item.fullPath);
        std::vector<std::string>& args = command.arguments;
        args = {std::string(compiler), "--driver-mode=cl", std::string(targetFor(platform)),
                runtimeFlag(valueOf(m, "runtimelibrary"), configuration)};
        if (auto flag = exceptionFlag(valueOf(m, "exceptionhandling"))) {
            args.push_back(*flag);
        }
        if (iequals(valueOf(m, "conformancemode"), "true")) {
            args.emplace_back("/permissive-");
        }
        if (iequals(valueOf(m, "treatwchar_tasbuiltintype"), "false")) {
            args.emplace_back("/Zc:wchar_t-");
        }
        const std::string compileAs = lower(valueOf(m, "compileas"));
        bool cSource = isCSource(command.file);
        if (compileAs == "compileasc") {
            args.emplace_back("/TC");
            cSource = true;
        } else if (compileAs == "compileascpp") {
            args.emplace_back("/TP");
            cSource = false;
        }
        for (const std::string& define : listEntries(valueOf(m, "preprocessordefinitions"))) {
            args.push_back("/D" + define);
        }
        for (const std::string& undefine : listEntries(valueOf(m, "undefinepreprocessordefinitions"))) {
            args.push_back("/U" + undefine);
        }
        for (const std::string& entry : listEntries(valueOf(m, "additionalincludedirectories"))) {
            if (auto dir = resolvePath(projectDirectory, entry)) {
                args.push_back("/I" + *dir);
            }
        }
        for (const std::string& dir : system) {
            args.push_back("/imsvc" + dir);
        }
        std::set<std::string> forced;
        for (const std::string& entry : listEntries(valueOf(m, "forcedincludefiles"))) {
            if (forced.insert(lower(entry)).second) {
                args.push_back("/FI" + entry);
            }
        }
        if (iequals(valueOf(m, "precompiledheader"), "use")) {
            const std::string header = valueOf(m, "precompiledheaderfile");
            if (!header.empty() && forced.insert(lower(header)).second) {
                args.push_back("/FI" + header);
            }
        }
        if (cSource) {
            if (auto standard = cStandard(valueOf(m, "languagestandard_c"))) {
                args.push_back(*standard);
            }
        } else {
            args.push_back(cxxStandard(valueOf(m, "languagestandard")));
        }
        for (std::string& option : splitOptions(valueOf(m, "additionaloptions"))) {
            if (!droppedOption(option)) {
                args.push_back(std::move(option));
            }
        }
        args.emplace_back("/c");
        args.push_back(command.file);
        out.push_back(std::move(command));
    }
    return out;
}

MsBuildCompileCommandsProvider::MsBuildCompileCommandsProvider(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                                               std::shared_ptr<const project::SolutionModel> model,
                                                               std::string cacheDirectory,
                                                               MsBuildProviderOptions options)
    : m_launcher(launcher), m_fs(fs), m_model(std::move(model)), m_cacheDirectory(std::move(cacheDirectory)),
      m_options(std::move(options)) {}

Result<std::optional<CompileCommandsLocation>> MsBuildCompileCommandsProvider::locate() {
    m_stats = {};
    m_warnings.clear();
    if constexpr (io::kNativePathStyle != io::PathStyle::Windows) {
        return std::optional<CompileCommandsLocation>(); // No MSBuild off Windows.
    } else {
        return evaluateAll();
    }
}

Result<std::optional<CompileCommandsLocation>> MsBuildCompileCommandsProvider::evaluateAll() {
    if (!m_options.trusted || !m_options.trusted()) {
        log::info(kLogCategory, "{} is not trusted: compile commands come from the static project model",
                  m_model->path());
        return std::optional<CompileCommandsLocation>();
    }
    auto configurationIndex = resolveConfigurationIndex(*m_model, m_options.configuration);
    if (!configurationIndex) {
        return std::unexpected(std::move(configurationIndex.error()));
    }
    std::string msbuild = m_options.msbuildPath;
    if (msbuild.empty()) {
        if (!m_options.locateMsBuild) {
            return fail(ErrorCode::NotFound, "no MSBuild to evaluate the projects with");
        }
        auto found = m_options.locateMsBuild();
        if (!found) {
            return std::unexpected(std::move(found.error().withContext("finding MSBuild for project evaluation")));
        }
        msbuild = std::move(*found);
    }
    const std::string evalDirectory = join(m_cacheDirectory, "msbuild-eval");
    if (auto created = m_fs.createDirectories(evalDirectory); !created) {
        return std::unexpected(std::move(created.error()));
    }

    StampCache stamps(m_fs);
    std::vector<CompileCommand> all;
    std::optional<Error> firstError;
    for (std::size_t p = 0; p < m_model->projects().size(); ++p) {
        const project::ProjectId id{static_cast<std::uint32_t>(p)};
        const project::ProjectInfo& info = m_model->project(id);
        const std::optional<project::ProjectConfigurationMapping>& mapping =
            m_model->projectConfiguration(id, *configurationIndex);
        if (!info.vcxproj || !mapping || info.path.empty()) {
            continue;
        }
        const std::string identity = documentKey(info.path) + "|" + mapping->configuration + "|" + mapping->platform;
        const std::string cacheFile = join(evalDirectory, std::format("{:016x}.json", fnv1a(identity)));

        // 1. The cache, when every file it depends on is unchanged.
        if (auto text = m_fs.readFile(cacheFile)) {
            if (auto cached = parseJson(*text); cached && cached->integerAt("version") == kCacheVersion &&
                                                cached->stringAt("msbuild") == msbuild &&
                                                cached->stringAt("identity") == identity) {
                bool fresh = true;
                if (const Json* files = cached->find("files"); files != nullptr) {
                    for (const Json& file : files->items()) {
                        const FileStamp now = stamps.stamp(std::string(file.stringAt("path")));
                        if (now != FileStamp{.size = file.integerAt("size").value_or(-2),
                                             .mtime = file.integerAt("mtime").value_or(0)}) {
                            fresh = false;
                            break;
                        }
                    }
                } else {
                    fresh = false;
                }
                const Json* commands = cached->find("commands");
                if (fresh && commands != nullptr) {
                    for (const Json& command : commands->items()) {
                        if (auto parsed = commandFromJson(command)) {
                            all.push_back(std::move(*parsed));
                        }
                    }
                    ++m_stats.cached;
                    continue;
                }
            }
        }

        // 2. Evaluate: the query, then the preprocess run for the import list.
        MsBuildEvaluationRequest request{.msbuildPath = msbuild,
                                         .projectPath = info.path,
                                         .solutionPath = m_model->path(),
                                         .configuration = mapping->configuration,
                                         .platform = mapping->platform,
                                         .preprocessTo = {},
                                         .environment = m_options.environment};
        const auto evaluate = [&]() -> Result<std::pair<std::vector<CompileCommand>, std::vector<std::string>>> {
            auto spec = msbuildEvaluationSpec(request);
            if (!spec) {
                return std::unexpected(std::move(spec.error()));
            }
            auto run = io::runToCompletion(m_launcher, *spec, {}, m_options.timeout);
            if (!run) {
                return std::unexpected(std::move(run.error()));
            }
            if (run->exitCode != 0) {
                return fail(ErrorCode::IoError, std::format("MSBuild exited with {}: {}", run->exitCode,
                                                            firstLines(run->stdoutBytes + run->stderrBytes, 3)));
            }
            auto evaluation = parseMsBuildEvaluation(run->stdoutBytes);
            if (!evaluation) {
                return std::unexpected(std::move(evaluation.error()));
            }
            std::vector<CompileCommand> commands = commandsFromEvaluation(
                *evaluation, info.vcxproj->directory, mapping->configuration, mapping->platform, m_options.compiler);

            std::vector<std::string> files{info.path};
            request.preprocessTo = join(evalDirectory, std::format("{:016x}.pp.xml", fnv1a(identity)));
            auto ppSpec = msbuildEvaluationSpec(request);
            if (!ppSpec) {
                return std::unexpected(std::move(ppSpec.error()));
            }
            auto pp = io::runToCompletion(m_launcher, *ppSpec, {}, m_options.timeout);
            if (!pp) {
                return std::unexpected(std::move(pp.error().withContext("preprocessing for the import list")));
            }
            auto preprocessed = m_fs.readFile(request.preprocessTo);
            static_cast<void>(m_fs.removeFile(request.preprocessTo));
            if (!preprocessed) {
                return std::unexpected(std::move(preprocessed.error().withContext("the preprocessed project")));
            }
            for (std::string& import : parsePreprocessedImports(*preprocessed)) {
                if (stamps.stamp(import).size < 0 && import.find("__") != std::string::npos) {
                    // MSBuild wrote "--" as "__" in the banner comment.
                    std::string repaired = import;
                    for (std::size_t at = repaired.find("__"); at != std::string::npos;
                         at = repaired.find("__", at + 2)) {
                        repaired.replace(at, 2, "--");
                    }
                    if (stamps.stamp(repaired).size >= 0) {
                        import = std::move(repaired);
                    }
                }
                files.push_back(std::move(import));
            }
            for (std::string& file : listEntries(valueOf(evaluation->properties, "msbuildallprojects"))) {
                files.push_back(std::move(file));
            }
            return std::pair{std::move(commands), std::move(files)};
        };
        auto evaluated = evaluate();
        if (!evaluated) {
            const std::string warning =
                std::format("MSBuild could not evaluate {} ({}|{}); using the static project model: {}", info.path,
                            mapping->configuration, mapping->platform, evaluated.error().describe());
            log::warn(kLogCategory, "{}", warning);
            m_warnings.push_back(warning);
            if (!firstError) {
                firstError = evaluated.error();
            }
            if (m_options.fallback.systemIncludeDirectories.empty() && m_options.msvc && !m_msvcLooked) {
                m_msvcLooked = true;
                if (auto environment = m_options.msvc()) {
                    m_options.fallback.systemIncludeDirectories = std::move(environment->includeDirectories);
                }
            }
            std::vector<CompileCommand> fallback =
                synthesizeProjectCompileCommands(*m_model, id, *configurationIndex, m_options.fallback);
            all.insert(all.end(), std::make_move_iterator(fallback.begin()), std::make_move_iterator(fallback.end()));
            ++m_stats.fellBack;
            continue;
        }
        ++m_stats.evaluated;
        Json files = Json::array();
        std::set<std::string> seen;
        for (const std::string& file : evaluated->second) {
            if (!seen.insert(documentKey(file)).second) {
                continue;
            }
            const FileStamp stamp = stamps.stamp(file);
            files.push(Json::object().set("path", file).set("size", stamp.size).set("mtime", stamp.mtime));
        }
        Json commands = Json::array();
        for (const CompileCommand& command : evaluated->first) {
            commands.push(commandToJson(command));
        }
        const Json entry = Json::object()
                               .set("version", kCacheVersion)
                               .set("identity", identity)
                               .set("msbuild", msbuild)
                               .set("files", std::move(files))
                               .set("commands", std::move(commands));
        if (auto written = m_fs.writeFileAtomic(cacheFile, entry.dump()); !written) {
            log::warn(kLogCategory, "could not cache the evaluation of {}: {}", info.path, written.error().describe());
        }
        all.insert(all.end(), std::make_move_iterator(evaluated->first.begin()),
                   std::make_move_iterator(evaluated->first.end()));
    }
    if (m_stats.evaluated + m_stats.cached == 0 && firstError) {
        // Nothing was evaluated: let the chain's static provider answer under its own name.
        return std::unexpected(std::move(firstError->withContext("MSBuild evaluated no project")));
    }
    log::info(kLogCategory, "MSBuild compile commands: {} evaluated, {} cached, {} from the static model",
              m_stats.evaluated, m_stats.cached, m_stats.fellBack);
    return writeCompileCommandsLocation(m_fs, m_cacheDirectory, all, CompileCommandsSource::Evaluated);
}

std::shared_ptr<ICompileCommandsProvider>
solutionCompileCommandsChain(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                             std::shared_ptr<const project::SolutionModel> model, std::string root,
                             std::string cacheDirectory, MsBuildProviderOptions options) {
    SynthesisOptions synthesis = options.fallback;
    if (synthesis.configuration.empty()) {
        synthesis.configuration = options.configuration;
    }
    std::function<Result<MsvcEnvironment>()> msvc = options.msvc;
    std::vector<std::shared_ptr<ICompileCommandsProvider>> chain;
    chain.push_back(std::make_shared<ExistingCompileCommandsProvider>(fs, std::move(root)));
    chain.push_back(std::make_shared<MsBuildCompileCommandsProvider>(
        launcher, fs, model, join(cacheDirectory, "evaluated"), std::move(options)));
    chain.push_back(std::make_shared<SolutionCompileCommandsProvider>(
        fs, std::move(model), join(cacheDirectory, "synthesized"), std::move(synthesis), std::move(msvc)));
    return std::make_shared<FirstAvailableCompileCommandsProvider>(std::move(chain));
}

} // namespace frost::lang
