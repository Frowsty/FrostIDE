// SPDX-License-Identifier: Apache-2.0
//
// The MSBuild-evaluated compile-commands provider (msbuild_compile_commands.hpp): the evaluation
// command line, its JSON, the -pp import list, the clang-cl commands and
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
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <mutex>
#include <set>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace frost::lang {

StreamedProjectContexts::StreamedProjectContexts(const std::shared_ptr<GeneratedCommandsRetirement>& retirement,
                                                 std::shared_ptr<const project::SolutionModel> model)
    : m_retirement(retirement), m_baseModel(model), m_model(std::move(model)) {}
void StreamedProjectContexts::publish(StreamedProjectContext record) {
    std::shared_ptr<const project::SolutionModel> previous;
    {
        std::lock_guard lock(m_mutex);
        if (!m_valid)
            return;
        previous = m_model;
    }
    std::shared_ptr<const project::SolutionModel> next;
    if (previous && record.state == ProjectEvaluationState::Evaluated) {
        if (const auto project = previous->findProjectByPath(record.projectPath)) {
            next = std::shared_ptr<const project::SolutionModel>(
                new project::SolutionModel([&] {
                    auto updated =
                        previous->withEvaluatedFiles(*project, project::ItemType::ClCompile, record.compileFiles);
                    if (record.headers)
                        updated = updated.withEvaluatedFiles(*project, project::ItemType::ClInclude, *record.headers);
                    return updated;
                }()),
                [retirement = m_retirement](const project::SolutionModel* model) {
                    if (auto owner = retirement.lock())
                        owner->submit([model] { delete model; });
                });
        }
    }
    auto immutable = std::shared_ptr<const StreamedProjectContext>(
        new StreamedProjectContext(std::move(record)), [retirement = m_retirement](const StreamedProjectContext* item) {
            if (auto owner = retirement.lock())
                owner->submit([item] { delete item; });
            // A closed scheduler retains the record rather than destroying it on the caller.
        });
    std::lock_guard lock(m_mutex);
    if (m_valid) {
        if (next && m_model == previous)
            m_model = std::move(next);
        const auto existing = std::ranges::find_if(
            m_records, [&](const auto& item) { return item->projectPath == immutable->projectPath; });
        if (existing == m_records.end())
            m_records.push_back(std::move(immutable));
        else
            *existing = std::move(immutable);
    }
}
void StreamedProjectContexts::invalidate() {
    std::lock_guard lock(m_mutex);
    m_valid = false;
    m_records.clear();
    m_model.reset();
}
void StreamedProjectContexts::clear() {
    std::lock_guard lock(m_mutex);
    m_records.clear();
    m_model = m_baseModel;
}
std::vector<std::shared_ptr<const StreamedProjectContext>> StreamedProjectContexts::snapshot() const {
    std::lock_guard lock(m_mutex);
    return m_records;
}
std::shared_ptr<const project::SolutionModel> StreamedProjectContexts::modelSnapshot() const {
    std::lock_guard lock(m_mutex);
    return m_valid ? m_model : nullptr;
}

namespace {

// MSBuild has no stdin input. Drain both pipes independently while wait/terminate remain
// concurrent, as IProcess permits; communicate() cannot overlap terminate(). The watchdog
// also bounds a descendant retaining an output pipe after the MSBuild process exits.
Result<io::ProcessResult> runEvaluation(io::IProcessLauncher& launcher, const io::ProcessSpec& spec,
                                        IClock::Duration timeout,
                                        std::stop_token stop) { // NOLINT(performance-unnecessary-value-param): Virtual
                                                                // process operations may replace the caller's token.
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "MSBuild evaluation cancelled");
    auto launched = launcher.launch(spec);
    if (!launched)
        return std::unexpected(std::move(launched.error()));
    auto& process = **launched;
    process.closeStdin();
    std::atomic<bool> interrupted{false};
    std::atomic<bool> expired{false};
    auto interrupt = [&] {
        if (!interrupted.exchange(true)) {
            static_cast<void>(process.terminate());
            process.abandonOutput();
        }
    };
    std::stop_callback cancel(stop, interrupt);
    std::mutex timerMutex;
    std::condition_variable_any timer;
    std::jthread watchdog([&](const std::stop_token& done) {
        std::unique_lock lock(timerMutex);
        if (!timer.wait_for(lock, done, timeout, [] { return false; }) && !done.stop_requested()) {
            expired.store(true);
            interrupt();
        }
    });
    std::string out, err;
    std::optional<Error> outError, errError;
    auto drain = [&](bool stdoutStream, std::string& sink, std::optional<Error>& error) {
        std::array<char, 8192> buffer{};
        for (;;) {
            auto count = stdoutStream ? process.readStdout(buffer) : process.readStderr(buffer);
            if (!count) {
                error = std::move(count.error());
                return;
            }
            if (*count == 0)
                return;
            sink.append(buffer.data(), *count);
        }
    };
    std::jthread stdoutReader([&] { drain(true, out, outError); });
    std::jthread stderrReader([&] { drain(false, err, errError); });
    Result<io::ExitStatus> status = fail(ErrorCode::Timeout, "MSBuild is running");
    while (!interrupted.load()) {
        status = process.wait(std::chrono::milliseconds(50));
        if (status || status.error().code() != ErrorCode::Timeout)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Scripted waits may return immediately.
    }
    if (!status && status.error().code() != ErrorCode::Timeout)
        interrupt();
    if (interrupted.load())
        status = process.wait(std::chrono::seconds(5));
    stdoutReader.join();
    stderrReader.join();
    watchdog.request_stop();
    watchdog.join();
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "MSBuild evaluation cancelled");
    if (expired.load())
        return fail(ErrorCode::Timeout, "MSBuild evaluation timed out");
    if (!status)
        return std::unexpected(std::move(status.error()));
    if (outError)
        return std::unexpected(std::move(*outError));
    if (errError)
        return std::unexpected(std::move(*errError));
    return io::ProcessResult{status->exitCode, status->signal, std::move(out), std::move(err)};
}

using namespace detail; // msvc_flags.hpp

constexpr std::string_view kLogCategory = "lang";

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

[[nodiscard]] std::string join(std::string_view base, std::string_view name) {
    auto joined = io::joinPath(base, name);
    return joined ? std::move(*joined) : std::string(base);
}

[[nodiscard]] std::string outputDirectory(std::string_view base) {
    static std::atomic<std::uint64_t> sequence{0};
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return {};
    const auto started = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
#else
    const auto pid = getpid();
    const std::uint64_t started = 0;
#endif
    return join(base, std::format("evaluation-{}-{}-{}", pid, started, sequence.fetch_add(1)));
}

class UnavailableCompileCommandsProvider final : public ICompileCommandsProvider {
public:
    Result<std::optional<CompileCommandsLocation>> locate() override {
        return fail(ErrorCode::IoError, "cannot allocate an isolated compile commands directory");
    }
};

class OwnedCompileCommandsProvider final : public ICompileCommandsProvider {
public:
    OwnedCompileCommandsProvider(std::shared_ptr<ICompileCommandsProvider> inner,
                                 std::shared_ptr<GeneratedCommandsLease> lease)
        : m_inner(std::move(inner)), m_lease(std::move(lease)) {}
    Result<std::optional<CompileCommandsLocation>> locate() override {
        auto result = m_inner->locate();
        if (result && *result)
            (**result).generatedLease = m_lease;
        return result;
    }

private:
    std::shared_ptr<ICompileCommandsProvider> m_inner;
    std::shared_ptr<GeneratedCommandsLease> m_lease;
};

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
        if (!properties->isObject())
            return fail(ErrorCode::Corrupt, "MSBuild Properties is not an object");
        for (std::size_t i = 0; i < properties->size(); ++i) {
            if (const std::string* value = properties->items()[i].asString()) {
                out.properties.emplace(lower(properties->keys()[i]), *value);
            }
        }
    }
    const Json* items = json->find("Items");
    if (items == nullptr || !items->isObject())
        return fail(ErrorCode::Corrupt, "MSBuild Items is missing or is not an object");
    const auto readItems = [](const Json& source, std::vector<EvaluatedItem>& target) -> Result<void> {
        if (!source.isArray())
            return fail(ErrorCode::Corrupt, "MSBuild item group is not an array");
        for (const Json& item : source.items()) {
            if (!item.isObject())
                return fail(ErrorCode::Corrupt, "MSBuild item is not an object");
            EvaluatedItem evaluated;
            for (std::size_t i = 0; i < item.size(); ++i) {
                if (const std::string* value = item.items()[i].asString()) {
                    evaluated.metadata.emplace(lower(item.keys()[i]), *value);
                }
            }
            evaluated.fullPath = valueOf(evaluated.metadata, "fullpath");
            if (!io::isAbsolutePath(evaluated.fullPath, io::PathStyle::Windows))
                return fail(ErrorCode::Corrupt, "MSBuild item FullPath is missing or is not absolute");
            target.push_back(std::move(evaluated));
        }
        return {};
    };
    const Json* clCompile = items->find("ClCompile");
    if (clCompile == nullptr)
        return fail(ErrorCode::Corrupt, "MSBuild ClCompile item group is missing");
    if (auto read = readItems(*clCompile, out.clCompile); !read)
        return std::unexpected(std::move(read.error()));
    if (const Json* clInclude = items->find("ClInclude")) {
        out.clInclude.emplace();
        if (auto read = readItems(*clInclude, *out.clInclude); !read)
            return std::unexpected(std::move(read.error()));
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
        spec.arguments.emplace_back("-getItem:ClCompile;ClInclude");
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
                                                               const std::string& cacheDirectory,
                                                               MsBuildProviderOptions options,
                                                               const std::string& cleanupBoundary,
                                                               std::vector<std::string> cleanupParents)
    : m_launcher(launcher), m_fs(fs), m_model(std::move(model)), m_cacheDirectory(outputDirectory(cacheDirectory)),
      m_lease(std::make_shared<GeneratedCommandsLease>(fs, cleanupBoundary.empty() ? cacheDirectory : cleanupBoundary,
                                                       m_cacheDirectory, std::move(cleanupParents),
                                                       options.retirement)),
      m_options(std::move(options)) {}

Result<std::optional<CompileCommandsLocation>> MsBuildCompileCommandsProvider::locate() { return locate({}); }

Result<std::optional<CompileCommandsLocation>> MsBuildCompileCommandsProvider::locate(std::stop_token stop) {
    m_stats = {};
    m_warnings.clear();
    if (m_options.streamedProjects)
        m_options.streamedProjects->clear();
    if (stop.stop_requested()) {
        if (m_options.streamedProjects)
            m_options.streamedProjects->invalidate();
        return fail(ErrorCode::Cancelled, "MSBuild evaluation cancelled");
    }
    if (m_cacheDirectory.empty())
        return fail(ErrorCode::IoError, "cannot allocate an isolated compile commands directory");
    if constexpr (io::kNativePathStyle != io::PathStyle::Windows) {
        return std::optional<CompileCommandsLocation>(); // No MSBuild off Windows.
    } else {
        return evaluateAll(stop);
    }
}

Result<std::optional<CompileCommandsLocation>> MsBuildCompileCommandsProvider::evaluateAll(std::stop_token stop) {
    if (!m_options.trusted || !m_options.trusted()) {
        if (m_options.streamedProjects)
            m_options.streamedProjects->invalidate();
        log::info(kLogCategory, "{} is not trusted: compile commands come from the static project model",
                  m_model->path());
        return std::optional<CompileCommandsLocation>();
    }
    auto configurationIndex = resolveConfigurationIndex(*m_model, m_options.configuration);
    if (!configurationIndex) {
        return std::unexpected(std::move(configurationIndex.error()));
    }
    std::vector<CompileCommand> all;
    std::vector<std::vector<CompileCommand>> byProject(m_model->projects().size());
    std::optional<Error> firstError;
    bool publishedFirst = false;
    std::vector<project::ProjectId> order;
    // A partial database must have the same duplicate-file precedence as the final one.
    // An unevaluated earlier project can introduce a file absent from its static model,
    // so evaluate in solution order before publishing any later project's commands.
    for (std::size_t p = 0; p < m_model->projects().size(); ++p) {
        const project::ProjectId id{static_cast<std::uint32_t>(p)};
        const auto& info = m_model->project(id);
        const auto& mapping = m_model->projectConfiguration(id, *configurationIndex);
        if (info.vcxproj && mapping && !info.path.empty())
            order.push_back(id);
    }
    if (m_options.streamedProjects) {
        for (const project::ProjectId id : order) {
            const auto& info = m_model->project(id);
            const auto& mapping = *m_model->projectConfiguration(id, *configurationIndex);
            m_options.streamedProjects->publish({.projectPath = info.path,
                                                 .configuration = mapping.configuration,
                                                 .platform = mapping.platform,
                                                 .state = ProjectEvaluationState::Pending,
                                                 .commands = {}});
        }
    }
    const auto failPending = [&] {
        if (!m_options.streamedProjects)
            return;
        for (const project::ProjectId id : order) {
            const auto& info = m_model->project(id);
            const auto& mapping = *m_model->projectConfiguration(id, *configurationIndex);
            m_options.streamedProjects->publish({.projectPath = info.path,
                                                 .configuration = mapping.configuration,
                                                 .platform = mapping.platform,
                                                 .state = ProjectEvaluationState::Failed,
                                                 .commands = {}});
        }
    };
    std::string msbuild = m_options.msbuildPath;
    if (msbuild.empty()) {
        if (!m_options.locateMsBuild) {
            failPending();
            return fail(ErrorCode::NotFound, "no MSBuild to evaluate the projects with");
        }
        auto found = m_options.locateMsBuild();
        if (!found) {
            failPending();
            return std::unexpected(std::move(found.error().withContext("finding MSBuild for project evaluation")));
        }
        msbuild = std::move(*found);
    }
    for (const project::ProjectId id : order) {
        if (stop.stop_requested()) {
            if (m_options.streamedProjects)
                m_options.streamedProjects->invalidate();
            return fail(ErrorCode::Cancelled, "MSBuild evaluation cancelled");
        }
        const project::ProjectInfo& info = m_model->project(id);
        const std::optional<project::ProjectConfigurationMapping>& mapping =
            m_model->projectConfiguration(id, *configurationIndex);
        if (!info.vcxproj || !mapping || info.path.empty()) {
            continue;
        }
        // Evaluation may depend on environment, absent imports, globs and property functions.
        // A previous import list cannot prove those inputs unchanged, so always query MSBuild.
        MsBuildEvaluationRequest request{.msbuildPath = msbuild,
                                         .projectPath = info.path,
                                         .solutionPath = m_model->path(),
                                         .configuration = mapping->configuration,
                                         .platform = mapping->platform,
                                         .preprocessTo = {},
                                         .environment = m_options.environment};
        const auto evaluate = [&]() -> Result<std::pair<ProjectEvaluation, std::vector<CompileCommand>>> {
            if (!m_options.trusted || !m_options.trusted())
                return fail(ErrorCode::Busy, "solution trust was revoked during MSBuild evaluation");
            auto spec = msbuildEvaluationSpec(request);
            if (!spec) {
                return std::unexpected(std::move(spec.error()));
            }
            auto run = runEvaluation(m_launcher, *spec, m_options.timeout, stop);
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
            auto commands = commandsFromEvaluation(*evaluation, info.vcxproj->directory, mapping->configuration,
                                                   mapping->platform, m_options.compiler);
            return std::pair(std::move(*evaluation), std::move(commands));
        };
        auto evaluated = evaluate();
        if (stop.stop_requested() || (!evaluated && evaluated.error().code() == ErrorCode::Cancelled)) {
            if (m_options.streamedProjects)
                m_options.streamedProjects->invalidate();
            return fail(ErrorCode::Cancelled, "MSBuild evaluation cancelled");
        }
        if (!m_options.trusted || !m_options.trusted()) {
            if (m_options.streamedProjects)
                m_options.streamedProjects->invalidate();
            return fail(ErrorCode::Busy, "solution trust was revoked during MSBuild evaluation");
        }
        if (!evaluated) {
            if (m_options.streamedProjects)
                m_options.streamedProjects->publish({.projectPath = info.path,
                                                     .configuration = mapping->configuration,
                                                     .platform = mapping->platform,
                                                     .state = ProjectEvaluationState::Failed,
                                                     .commands = {}});
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
            byProject[id.value()] = std::move(fallback);
            ++m_stats.fellBack;
            continue;
        }
        ++m_stats.evaluated;
        if (m_options.streamedProjects) {
            std::vector<std::string> compileFiles;
            compileFiles.reserve(evaluated->first.clCompile.size());
            for (const auto& item : evaluated->first.clCompile)
                compileFiles.push_back(item.fullPath);
            std::optional<std::vector<std::string>> headers;
            if (evaluated->first.clInclude) {
                headers.emplace();
                for (const auto& item : *evaluated->first.clInclude)
                    headers->push_back(item.fullPath);
            }
            m_options.streamedProjects->publish({.projectPath = info.path,
                                                 .configuration = mapping->configuration,
                                                 .platform = mapping->platform,
                                                 .state = ProjectEvaluationState::Evaluated,
                                                 .commands = evaluated->second,
                                                 .compileFiles = std::move(compileFiles),
                                                 .headers = std::move(headers)});
        }
        if (!publishedFirst && m_options.onFirstDatabase && !evaluated->second.empty()) {
            std::vector<CompileCommand> prefix;
            for (std::size_t p = 0; p < id.value(); ++p)
                prefix.insert(prefix.end(), byProject[p].begin(), byProject[p].end());
            prefix.insert(prefix.end(), evaluated->second.begin(), evaluated->second.end());
            auto partial =
                writeCompileCommandsLocation(m_fs, m_cacheDirectory, prefix, CompileCommandsSource::Evaluated);
            if (!partial)
                return std::unexpected(std::move(partial.error()));
            if (stop.stop_requested() || !m_options.trusted || !m_options.trusted())
                return fail(ErrorCode::Cancelled, "MSBuild evaluation cancelled before publication");
            if (*partial) {
                (**partial).generatedLease = m_lease;
                m_options.onFirstDatabase(**partial);
                publishedFirst = true;
            }
        }
        byProject[id.value()] = std::move(evaluated->second);
    }
    // Evaluation priority must not change duplicate-file precedence in the final database.
    for (auto& commands : byProject)
        all.insert(all.end(), std::make_move_iterator(commands.begin()), std::make_move_iterator(commands.end()));
    if (m_stats.evaluated + m_stats.cached == 0 && firstError) {
        // Nothing was evaluated: let the chain's static provider answer under its own name.
        return std::unexpected(std::move(firstError->withContext("MSBuild evaluated no project")));
    }
    log::info(kLogCategory, "MSBuild compile commands: {} evaluated, {} cached, {} from the static model",
              m_stats.evaluated, m_stats.cached, m_stats.fellBack);
    if (!m_options.trusted || !m_options.trusted()) {
        if (m_options.streamedProjects)
            m_options.streamedProjects->invalidate();
        return fail(ErrorCode::Busy, "solution trust was revoked during MSBuild evaluation");
    }
    if (stop.stop_requested()) {
        if (m_options.streamedProjects)
            m_options.streamedProjects->invalidate();
        return fail(ErrorCode::Cancelled, "MSBuild evaluation cancelled");
    }
    auto result = writeCompileCommandsLocation(m_fs, m_cacheDirectory, all, CompileCommandsSource::Evaluated);
    if (result && *result)
        (**result).generatedLease = m_lease;
    return result;
}

IndependentProjectEvaluation::IndependentProjectEvaluation(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                                           std::shared_ptr<const project::SolutionModel> model,
                                                           const std::string& cacheDirectory,
                                                           MsBuildProviderOptions options)
    : m_trusted(options.trusted) {
    options.onFirstDatabase = [this](const CompileCommandsLocation& first) {
        std::lock_guard lock(m_mutex);
        m_first = first;
        m_changed.notify_all();
    };
    m_provider = std::make_shared<MsBuildCompileCommandsProvider>(launcher, fs, std::move(model), cacheDirectory,
                                                                  std::move(options));
    m_worker = std::jthread([this] {
        auto result = m_provider->locate(m_stop.get_token());
        std::lock_guard lock(m_mutex);
        m_final = std::move(result);
        m_changed.notify_all();
    });
}

IndependentProjectEvaluation::~IndependentProjectEvaluation() { stopAndJoin(); }
void IndependentProjectEvaluation::requestStop() {
    m_stop.request_stop();
    m_changed.notify_all();
}
void IndependentProjectEvaluation::stopAndJoin() {
    requestStop();
    if (m_worker.joinable())
        m_worker.join();
}
Result<std::optional<CompileCommandsLocation>> IndependentProjectEvaluation::locate() { return locate({}); }
Result<std::optional<CompileCommandsLocation>> IndependentProjectEvaluation::locate(std::stop_token stop) {
    std::unique_lock lock(m_mutex);
    m_changed.wait(lock, stop, [this] { return m_first.has_value() || m_final.has_value(); });
    if (stop.stop_requested() || m_stop.stop_requested())
        return fail(ErrorCode::Cancelled, "project evaluation cancelled");
    if (!m_trusted || !m_trusted())
        return fail(ErrorCode::Busy, "project trust changed during evaluation");
    if (m_final && !*m_final)
        return std::unexpected(m_final->error());
    if (m_first)
        return std::optional<CompileCommandsLocation>(*m_first);
    if (m_final)
        return **m_final;
    return fail(ErrorCode::Cancelled, "project evaluation cancelled");
}

std::shared_ptr<ICompileCommandsProvider> solutionCompileCommandsChain(
    io::IProcessLauncher& launcher, io::IFileSystem& fs, std::shared_ptr<const project::SolutionModel> model,
    std::string root, std::string cacheDirectory, MsBuildProviderOptions options,
    std::shared_ptr<ICompileCommandsProvider> evaluated, std::shared_ptr<ICompileCommandsProvider> clCommandLines) {
    const std::string cleanupBoundary = cacheDirectory;
    cacheDirectory = outputDirectory(cacheDirectory);
    SynthesisOptions synthesis = options.fallback;
    if (synthesis.configuration.empty()) {
        synthesis.configuration = options.configuration;
    }
    std::function<Result<MsvcEnvironment>()> msvc = options.msvc;
    auto retirement = options.retirement;
    std::vector<std::shared_ptr<ICompileCommandsProvider>> chain;
    chain.push_back(std::make_shared<ExistingCompileCommandsProvider>(fs, std::move(root)));
    if (clCommandLines)
        chain.push_back(std::move(clCommandLines));
    if (cacheDirectory.empty()) {
        chain.push_back(std::make_shared<UnavailableCompileCommandsProvider>());
        return std::make_shared<FirstAvailableCompileCommandsProvider>(std::move(chain));
    }
    if (evaluated)
        chain.push_back(std::move(evaluated));
    else
        chain.push_back(std::make_shared<MsBuildCompileCommandsProvider>(
            launcher, fs, model, join(cacheDirectory, "evaluated"), std::move(options), cleanupBoundary,
            std::vector<std::string>{join(cacheDirectory, "evaluated"), cacheDirectory}));
    const std::string synthesized = join(cacheDirectory, "synthesized");
    auto fallback = std::make_shared<SolutionCompileCommandsProvider>(fs, std::move(model), synthesized,
                                                                      std::move(synthesis), std::move(msvc));
    chain.push_back(std::make_shared<OwnedCompileCommandsProvider>(
        std::move(fallback),
        std::make_shared<GeneratedCommandsLease>(fs, cleanupBoundary, synthesized,
                                                 std::vector<std::string>{cacheDirectory}, std::move(retirement))));
    return std::make_shared<FirstAvailableCompileCommandsProvider>(std::move(chain));
}

} // namespace frost::lang
