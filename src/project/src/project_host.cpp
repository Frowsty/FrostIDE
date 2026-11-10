// SPDX-License-Identifier: Apache-2.0
//
// ProjectHost (frost/project/project_host.hpp): the helper's JSON-lines protocol, its client
// process, the MSBuild.exe fallback (path (c)) and the conversion of an evaluation into the
// Solution Explorer's items. SolutionEvaluation is in solution_evaluation.cpp.

#include "frost/project/project_host.hpp"

#include "frost/io/path.hpp"
#include "frost/project/vcxproj.hpp"

#include "json.hpp"
#include "paths.hpp"
#include "text_util.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <unordered_set>

namespace frost::project {
namespace {

using detail::JsonNode;

// MSBuild's escaping of a -p: value: '%', ';' and ',' would otherwise split or expand it.
std::string escapeMsBuildValue(std::string_view value) {
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

// Appends `text` as a JSON string literal (UTF-8 passes through; controls are \u escaped).
void appendJsonString(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void appendMember(std::string& out, std::string_view key, std::string_view value) {
    if (out.size() > 1) {
        out += ',';
    }
    appendJsonString(out, key);
    out += ':';
    appendJsonString(out, value);
}

std::string joinList(const std::vector<std::string>& values) {
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) {
            out += ';';
        }
        out += value;
    }
    return out;
}

std::vector<std::string> requestedTypes(const ProjectEvaluationRequest& request) {
    if (!request.itemTypes.empty()) {
        return request.itemTypes;
    }
    std::vector<std::string> out;
    for (std::string_view name : fileItemTypeNames()) {
        out.emplace_back(name);
    }
    return out;
}

Result<void> checkRequest(const ProjectEvaluationRequest& request) {
    if (!io::isAbsolutePath(request.projectPath)) {
        return fail(ErrorCode::InvalidArgument, std::format("{} is not an absolute project path", request.projectPath));
    }
    if (!request.solutionPath.empty() && !io::isAbsolutePath(request.solutionPath)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("{} is not an absolute solution path", request.solutionPath));
    }
    if (request.configuration.empty() || request.platform.empty()) {
        return fail(ErrorCode::InvalidArgument, "an evaluation needs a configuration and a platform");
    }
    return {};
}

// The answer line as a JSON object, with its Error turned into IoError.
Result<JsonNode> answerObject(std::string_view line, std::string_view op) {
    auto json = detail::parseJson(line);
    if (!json || !json->isObject()) {
        return fail(ErrorCode::Corrupt, std::format("ProjectHost answered {} with something that is not JSON", op));
    }
    if (const JsonNode* error = json->find("Error"); error != nullptr && error->isString()) {
        return fail(ErrorCode::IoError, std::format("ProjectHost: {}", error->text));
    }
    if (json->stringAt("Op") != op) {
        return fail(ErrorCode::Corrupt, std::format("ProjectHost answered another request than {}", op));
    }
    return std::move(*json);
}

std::vector<std::string> stringArray(const JsonNode* node) {
    std::vector<std::string> out;
    if (node != nullptr && node->isArray()) {
        for (const JsonNode& item : node->items) {
            if (item.isString()) {
                out.push_back(item.text);
            }
        }
    }
    return out;
}

void readStringMap(const JsonNode* node, std::map<std::string, std::string, std::less<>>& out) {
    if (node == nullptr || !node->isObject()) {
        return;
    }
    for (std::size_t i = 0; i < node->items.size(); ++i) {
        if (node->items[i].isString()) {
            out.emplace(node->keys[i], node->items[i].text);
        }
    }
}

// A member of `object` matched case-insensitively (MSBuild's metadata and property names).
const JsonNode* findInsensitive(const JsonNode& object, std::string_view key) {
    for (std::size_t i = 0; i < object.keys.size(); ++i) {
        if (detail::iequals(object.keys[i], key)) {
            return &object.items[i];
        }
    }
    return nullptr;
}

/*
 * Runs one MSBuild.exe to completion, draining both pipes while wait() and terminate() stay
 * available (communicate() cannot overlap terminate()). `stop` or the timeout ends its tree.
 */
Result<io::ProcessResult> runCli(io::IProcessLauncher& launcher, const io::ProcessSpec& spec, IClock::Duration timeout,
                                 std::stop_token stop) {
    if (stop.stop_requested()) {
        return fail(ErrorCode::Cancelled, "MSBuild evaluation cancelled");
    }
    auto launched = launcher.launch(spec);
    if (!launched) {
        return std::unexpected(std::move(launched.error()));
    }
    io::IProcess& process = **launched;
    std::atomic<bool> interrupted{false};
    const auto interrupt = [&process, &interrupted] {
        if (!interrupted.exchange(true)) {
            static_cast<void>(process.terminate());
            process.abandonOutput();
        }
    };
    std::stop_callback onStop(stop, interrupt);
    std::string out;
    std::string err;
    const auto drain = [&process](bool stdoutStream, std::string& sink) {
        std::array<char, 16384> buffer{};
        for (;;) {
            auto count = stdoutStream ? process.readStdout(buffer) : process.readStderr(buffer);
            if (!count || *count == 0) {
                return;
            }
            sink.append(buffer.data(), *count);
        }
    };
    std::jthread stdoutReader([&] { drain(true, out); });
    std::jthread stderrReader([&] { drain(false, err); });
    auto status = process.wait(timeout);
    const bool expired = !status && status.error().code() == ErrorCode::Timeout;
    if (!status) {
        interrupt();
        status = process.wait(std::chrono::seconds(5));
    }
    stdoutReader.join();
    stderrReader.join();
    if (stop.stop_requested()) {
        return fail(ErrorCode::Cancelled, "MSBuild evaluation cancelled");
    }
    if (expired) {
        return fail(ErrorCode::Timeout, "MSBuild evaluation timed out");
    }
    if (!status) {
        return std::unexpected(std::move(status.error()));
    }
    return io::ProcessResult{status->exitCode, status->signal, std::move(out), std::move(err)};
}

} // namespace

// --- protocol ---

std::vector<std::pair<std::string, std::string>> evaluationGlobalProperties(const ProjectEvaluationRequest& request) {
    std::vector<std::pair<std::string, std::string>> out{{"Configuration", request.configuration},
                                                         {"Platform", request.platform}};
    if (!request.solutionPath.empty()) {
        std::string directory(io::parentPath(request.solutionPath));
        if (!directory.empty() && directory.back() != '\\' && directory.back() != '/') {
            directory += '\\';
        }
        const std::string file(io::fileName(request.solutionPath));
        const std::size_t dot = file.find_last_of('.');
        out.emplace_back("SolutionDir", directory);
        out.emplace_back("SolutionPath", request.solutionPath);
        out.emplace_back("SolutionName", file.substr(0, dot));
        out.emplace_back("SolutionFileName", file);
        out.emplace_back("SolutionExt", dot == std::string::npos ? std::string() : file.substr(dot));
    }
    return out;
}

std::string projectHostHelloRequest() { return R"({"Op":"Hello"})"; }

Result<std::string> projectHostEvaluateRequest(const ProjectEvaluationRequest& request, std::string_view id) {
    if (auto checked = checkRequest(request); !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    std::string out = "{";
    appendMember(out, "Op", "Evaluate");
    appendMember(out, "Id", id);
    // The caller (SolutionEvaluation) only evaluates a trusted root; the helper checks the flag.
    appendMember(out, "Trusted", "true");
    appendMember(out, "Project", request.projectPath);
    appendMember(out, "ItemTypes", joinList(requestedTypes(request)));
    appendMember(out, "Metadata", joinList(request.metadata));
    appendMember(out, "Properties", joinList(request.properties));
    for (const auto& [name, value] : evaluationGlobalProperties(request)) {
        appendMember(out, "Property:" + name, value);
    }
    out += '}';
    return out;
}

Result<ProjectHostHello> parseProjectHostHello(std::string_view line) {
    auto json = answerObject(line, "Hello");
    if (!json) {
        return std::unexpected(std::move(json.error()));
    }
    ProjectHostHello hello;
    hello.protocol = static_cast<int>(json->integerAt("Protocol").value_or(0));
    if (hello.protocol != kProjectHostProtocol) {
        return fail(ErrorCode::Unsupported, std::format("ProjectHost speaks protocol {}, FrostIDE speaks {}",
                                                        hello.protocol, kProjectHostProtocol));
    }
    hello.msbuildVersion = json->stringAt("MSBuildVersion");
    hello.msbuildAssembly = json->stringAt("MSBuildAssembly");
    hello.visualStudioVersion = json->stringAt("VisualStudioVersion");
    hello.processId = static_cast<std::uint32_t>(json->integerAt("ProcessId").value_or(0));
    return hello;
}

Result<HostEvaluation> parseProjectHostEvaluation(std::string_view line) {
    auto json = answerObject(line, "Evaluate");
    if (!json) {
        return std::unexpected(std::move(json.error()));
    }
    HostEvaluation out;
    out.projectPath = json->stringAt("Project");
    out.configuration = json->stringAt("Configuration");
    out.platform = json->stringAt("Platform");
    const JsonNode* items = json->find("Items");
    if (items == nullptr || !items->isArray()) {
        return fail(ErrorCode::Corrupt, "ProjectHost's evaluation has no Items array");
    }
    for (const JsonNode& item : items->items) {
        if (!item.isObject()) {
            return fail(ErrorCode::Corrupt, "a ProjectHost item is not an object");
        }
        HostItem parsed{.type = item.stringAt("Type"),
                        .include = item.stringAt("Include"),
                        .fullPath = item.stringAt("FullPath"),
                        .definingProject = item.stringAt("DefiningProject"),
                        .metadata = {}};
        readStringMap(item.find("Metadata"), parsed.metadata);
        out.items.push_back(std::move(parsed));
    }
    out.imports = stringArray(json->find("Imports"));
    readStringMap(json->find("Properties"), out.properties);
    out.projectConfigurations = stringArray(json->find("ProjectConfigurations"));
    out.globs = stringArray(json->find("Globs"));
    return out;
}

std::vector<std::pair<std::string, std::string>>
clCommandLinesGlobalProperties(const ProjectEvaluationRequest& request) {
    auto out = evaluationGlobalProperties(request);
    out.emplace_back("DesignTimeBuild", "true");
    out.emplace_back("BuildProjectReferences", "false");
    return out;
}

std::vector<std::string> clCommandLinesPropertyNames() { return {"IncludePath", "ExternalIncludePath"}; }

Result<std::string> projectHostClCommandLinesRequest(const ProjectEvaluationRequest& request, std::string_view id) {
    if (auto checked = checkRequest(request); !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    std::string out = "{";
    appendMember(out, "Op", "ClCommandLines");
    appendMember(out, "Id", id);
    appendMember(out, "Trusted", "true");
    appendMember(out, "Project", request.projectPath);
    appendMember(out, "Properties", joinList(clCommandLinesPropertyNames()));
    for (const auto& [name, value] : clCommandLinesGlobalProperties(request)) {
        appendMember(out, "Property:" + name, value);
    }
    out += '}';
    return out;
}

Result<ProjectClCommandLines> parseProjectHostClCommandLines(std::string_view line) {
    auto json = answerObject(line, "ClCommandLines");
    if (!json) {
        return std::unexpected(std::move(json.error()));
    }
    ProjectClCommandLines out;
    out.projectPath = detail::normalizeAbsolute(json->stringAt("Project")).value_or(json->stringAt("Project"));
    out.configuration = json->stringAt("Configuration");
    out.platform = json->stringAt("Platform");
    const JsonNode* lines = json->find("CommandLines");
    if (lines == nullptr || !lines->isArray()) {
        return fail(ErrorCode::Corrupt, "ProjectHost's GetClCommandLines answer has no CommandLines array");
    }
    for (const JsonNode& entry : lines->items) {
        if (!entry.isObject()) {
            return fail(ErrorCode::Corrupt, "a ProjectHost command line is not an object");
        }
        ClCommandLine parsed;
        parsed.commandLine = entry.stringAt("CommandLine");
        parsed.workingDirectory =
            entry.stringAt("WorkingDirectory").empty()
                ? std::string()
                : detail::normalizeAbsolute(entry.stringAt("WorkingDirectory")).value_or(std::string());
        parsed.configurationOptions = detail::iequals(entry.stringAt("ConfigurationOptions"), "true");
        for (std::string_view file : detail::splitTrimmed(entry.stringAt("Files"), ';')) {
            // The target's own placeholder for the project-wide options is not a source.
            if (detail::iequals(io::fileName(file), "__temporary.cpp")) {
                continue;
            }
            if (auto normalized = detail::normalizeAbsolute(file)) {
                parsed.files.push_back(std::move(*normalized));
            }
        }
        out.commandLines.push_back(std::move(parsed));
    }
    if (const JsonNode* properties = json->find("Properties"); properties != nullptr && properties->isObject()) {
        // A relative directory (Notepad++ lists "..\src\json") is relative to the project's
        // directory, where MSBuild's CL task runs cl.exe with INCLUDE and EXTERNAL_INCLUDE set.
        const std::string projectDirectory =
            io::isAbsolutePath(out.projectPath) ? std::string(io::parentPath(out.projectPath)) : std::string();
        const auto directories = [&projectDirectory](std::string_view list) {
            std::vector<std::string> dirs;
            for (std::string_view entry : detail::splitTrimmed(list, ';')) {
                auto resolved = io::isAbsolutePath(entry) || projectDirectory.empty()
                                    ? detail::normalizeAbsolute(entry)
                                    : detail::resolvePath(projectDirectory, entry);
                if (resolved) {
                    dirs.push_back(std::move(*resolved));
                }
            }
            return dirs;
        };
        out.includePath = directories(properties->stringAt("IncludePath"));
        out.externalIncludePath = directories(properties->stringAt("ExternalIncludePath"));
    }
    return out;
}

// --- the MSBuild.exe fallback ---

Result<io::ProcessSpec> msbuildCliEvaluationSpec(std::string_view msbuildPath,
                                                 const ProjectEvaluationRequest& request) {
    if (!io::isAbsolutePath(msbuildPath)) {
        return fail(ErrorCode::InvalidArgument, std::format("{} is not an absolute MSBuild path", msbuildPath));
    }
    if (auto checked = checkRequest(request); !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    io::ProcessSpec spec;
    spec.executable = std::string(msbuildPath);
    spec.arguments = {request.projectPath, "-nologo", "-nodeReuse:false",
                      "-getItem:" + joinList(requestedTypes(request))};
    // MSBuild prints a bare value, not JSON, for -getProperty alone; with -getItem it is JSON.
    if (!request.properties.empty()) {
        spec.arguments.push_back("-getProperty:" + joinList(request.properties));
    }
    for (const auto& [name, value] : evaluationGlobalProperties(request)) {
        spec.arguments.push_back("-p:" + name + "=" + escapeMsBuildValue(value));
    }
    spec.workingDirectory = std::string(io::parentPath(request.projectPath));
    spec.environment = {
        {"DOTNET_CLI_UI_LANGUAGE", "en-US"}, {"MSBUILDTERMINALLOGGER", "off"}, {"MSBUILDDISABLENODEREUSE", "1"}};
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    return spec;
}

Result<HostEvaluation> parseMsBuildCliEvaluation(std::string_view output, const ProjectEvaluationRequest& request) {
    const std::size_t brace = output.find('{');
    if (brace == std::string_view::npos) {
        return fail(ErrorCode::Corrupt, "MSBuild printed no JSON");
    }
    auto json = detail::parseJson(output.substr(brace));
    if (!json || !json->isObject()) {
        return fail(ErrorCode::Corrupt, "MSBuild's evaluation output is not a JSON object");
    }
    HostEvaluation out;
    out.projectPath = request.projectPath;
    out.configuration = request.configuration;
    out.platform = request.platform;
    if (const JsonNode* properties = json->find("Properties"); properties != nullptr) {
        readStringMap(properties, out.properties);
    }
    const JsonNode* items = json->find("Items");
    if (items == nullptr || !items->isObject()) {
        return fail(ErrorCode::Corrupt, "MSBuild's evaluation output has no Items object");
    }
    // Items come grouped by type; the requested order keeps the result deterministic.
    for (const std::string& type : requestedTypes(request)) {
        const JsonNode* group = findInsensitive(*items, type);
        if (group == nullptr) {
            continue;
        }
        if (!group->isArray()) {
            return fail(ErrorCode::Corrupt, std::format("MSBuild's {} items are not an array", type));
        }
        for (const JsonNode& item : group->items) {
            if (!item.isObject()) {
                return fail(ErrorCode::Corrupt, "an MSBuild item is not an object");
            }
            HostItem parsed;
            parsed.type = type;
            if (const JsonNode* value = findInsensitive(item, "Identity"); value != nullptr && value->isString()) {
                parsed.include = value->text;
            }
            if (const JsonNode* value = findInsensitive(item, "FullPath"); value != nullptr && value->isString()) {
                parsed.fullPath = value->text;
            }
            if (const JsonNode* value = findInsensitive(item, "DefiningProjectFullPath");
                value != nullptr && value->isString()) {
                parsed.definingProject = value->text;
            }
            for (const std::string& name : request.metadata) {
                const JsonNode* value = findInsensitive(item, name);
                parsed.metadata.emplace(name, value != nullptr && value->isString() ? value->text : std::string());
            }
            out.items.push_back(std::move(parsed));
        }
    }
    return out;
}

EvaluatedProjectItems toEvaluatedProjectItems(const HostEvaluation& evaluation, EvaluationSource source,
                                              std::string_view msbuildVersion) {
    EvaluatedProjectItems out{.projectPath = evaluation.projectPath,
                              .configuration = evaluation.configuration,
                              .platform = evaluation.platform,
                              .source = source,
                              .msbuildVersion = std::string(msbuildVersion),
                              .items = {},
                              .imports = {},
                              .warnings = {}};
    std::unordered_set<std::string> seen;
    for (const HostItem& item : evaluation.items) {
        const std::optional<ItemType> type = fileItemTypeOf(item.type);
        if (!type) {
            continue;
        }
        auto path = detail::normalizeAbsolute(item.fullPath);
        if (!path) {
            out.warnings.push_back(std::format("{} item '{}' has no usable full path", item.type, item.include));
            continue;
        }
        if (!seen.insert(detail::pathKey(*path)).second) {
            continue;
        }
        out.items.push_back(ProjectItem{.type = *type,
                                        .itemName = item.type,
                                        .include = item.include,
                                        .path = std::move(*path),
                                        .fromWildcard = false});
    }
    std::unordered_set<std::string> importsSeen;
    for (const std::string& import : evaluation.imports) {
        auto path = detail::normalizeAbsolute(import);
        if (path && importsSeen.insert(detail::pathKey(*path)).second) {
            out.imports.push_back(std::move(*path));
        }
    }
    return out;
}

// --- the client ---

struct ProjectHostClient::Shared {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::deque<std::string> lines;
    bool ended = false;
    std::string stderrTail; // The last few KiB the helper wrote to stderr.
};

Result<io::ProcessSpec> projectHostSpec(const ProjectHostLaunch& launch) {
    if (!io::isAbsolutePath(launch.hostPath) || !io::isAbsolutePath(launch.msbuildPath)) {
        return fail(ErrorCode::InvalidArgument, "ProjectHost and MSBuild need absolute paths");
    }
    io::ProcessSpec spec;
    spec.executable = launch.hostPath;
    spec.arguments = {std::string(io::parentPath(launch.msbuildPath))};
    spec.workingDirectory = std::string(io::parentPath(launch.hostPath));
    spec.environment = {
        {"DOTNET_CLI_UI_LANGUAGE", "en-US"}, {"MSBUILDTERMINALLOGGER", "off"}, {"MSBUILDDISABLENODEREUSE", "1"}};
    spec.environment.insert(spec.environment.end(), launch.environment.begin(), launch.environment.end());
    spec.stdinMode = io::Stdio::Pipe;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    return spec;
}

Result<std::unique_ptr<ProjectHostClient>>
ProjectHostClient::start(io::IProcessLauncher& launcher, const ProjectHostLaunch& launch, std::stop_token stop) {
    auto spec = projectHostSpec(launch);
    if (!spec) {
        return std::unexpected(std::move(spec.error()));
    }
    auto launched = launcher.launch(*spec);
    if (!launched) {
        return std::unexpected(std::move(launched.error()));
    }
    std::unique_ptr<ProjectHostClient> client(new ProjectHostClient());
    client->m_process = std::move(*launched);
    client->m_pid = client->m_process->pid();
    client->m_requestTimeout = launch.requestTimeout;
    client->m_shared = std::make_shared<Shared>();
    io::IProcess* process = client->m_process.get();
    client->m_stdoutReader = std::jthread([process, shared = client->m_shared] {
        std::array<char, 65536> buffer{};
        std::string pending;
        for (;;) {
            auto count = process->readStdout(buffer);
            if (!count || *count == 0) {
                break;
            }
            pending.append(buffer.data(), *count);
            std::size_t newline = 0;
            std::vector<std::string> complete;
            while ((newline = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, newline);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                complete.push_back(std::move(line));
                pending.erase(0, newline + 1);
            }
            if (!complete.empty()) {
                std::lock_guard lock(shared->mutex);
                for (std::string& line : complete) {
                    shared->lines.push_back(std::move(line));
                }
                shared->changed.notify_all();
            }
        }
        std::lock_guard lock(shared->mutex);
        shared->ended = true;
        shared->changed.notify_all();
    });
    client->m_stderrReader = std::jthread([process, shared = client->m_shared] {
        std::array<char, 4096> buffer{};
        for (;;) {
            auto count = process->readStderr(buffer);
            if (!count || *count == 0) {
                return;
            }
            std::lock_guard lock(shared->mutex);
            shared->stderrTail.append(buffer.data(), *count);
            if (shared->stderrTail.size() > 4096) {
                shared->stderrTail.erase(0, shared->stderrTail.size() - 4096);
            }
        }
    });
    if (auto written = client->m_process->writeStdin(projectHostHelloRequest() + "\n"); !written) {
        return std::unexpected(client->endedError());
    }
    auto line = client->nextLine(launch.startTimeout, stop);
    if (!line) {
        return std::unexpected(std::move(line.error()));
    }
    auto hello = parseProjectHostHello(*line);
    if (!hello) {
        return std::unexpected(std::move(hello.error()));
    }
    client->m_hello = std::move(*hello);
    return client;
}

ProjectHostClient::~ProjectHostClient() {
    if (m_process) {
        m_process->closeStdin();
        static_cast<void>(m_process->terminate());
        m_process->abandonOutput();
    }
    if (m_stdoutReader.joinable()) {
        m_stdoutReader.join();
    }
    if (m_stderrReader.joinable()) {
        m_stderrReader.join();
    }
}

bool ProjectHostClient::running() const {
    std::lock_guard lock(m_shared->mutex);
    return !m_shared->ended;
}

bool ProjectHostClient::waitUntilEnded(std::stop_token stop) {
    std::unique_lock lock(m_shared->mutex);
    return m_shared->changed.wait(lock, stop, [this] { return m_shared->ended; });
}

Error ProjectHostClient::endedError() {
    std::string tail;
    {
        std::lock_guard lock(m_shared->mutex);
        tail = detail::trim(m_shared->stderrTail);
    }
    auto status = m_process->wait(std::chrono::seconds(2));
    std::string how = status ? std::format("exit code {}", status->exitCode) : std::string("still running");
    if (!tail.empty()) {
        if (const std::size_t newline = tail.find_last_of('\n'); newline != std::string::npos) {
            tail = detail::trim(tail.substr(newline + 1)); // The last line usually names the exception.
        }
        how += ": " + tail;
    }
    return Error(ErrorCode::IoError, std::format("ProjectHost ended ({})", how));
}

Result<std::string> ProjectHostClient::nextLine(IClock::Duration timeout, std::stop_token stop) {
    std::unique_lock lock(m_shared->mutex);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const bool ready = m_shared->changed.wait_until(lock, stop, deadline,
                                                    [this] { return !m_shared->lines.empty() || m_shared->ended; });
    if (!m_shared->lines.empty()) {
        std::string line = std::move(m_shared->lines.front());
        m_shared->lines.pop_front();
        return line;
    }
    lock.unlock();
    if (stop.stop_requested()) {
        return fail(ErrorCode::Cancelled, "ProjectHost request cancelled");
    }
    if (!ready) {
        // A helper that does not answer in time is ended, so it cannot answer late and out of order.
        static_cast<void>(m_process->terminate());
        return fail(ErrorCode::Timeout, "ProjectHost did not answer in time; it was ended");
    }
    return std::unexpected(endedError());
}

Result<std::string> ProjectHostClient::exchange(const std::string& line, std::stop_token stop) {
    if (!running()) {
        return std::unexpected(endedError());
    }
    if (auto written = m_process->writeStdin(line + "\n"); !written) {
        // A closed stdin means the helper is going away; let the reader see its stdout end, so
        // running() is false when the caller asks why the request failed.
        std::unique_lock lock(m_shared->mutex);
        m_shared->changed.wait_for(lock, stop, std::chrono::seconds(10), [this] { return m_shared->ended; });
        lock.unlock();
        return std::unexpected(endedError());
    }
    return nextLine(m_requestTimeout, stop);
}

Result<ProjectClCommandLines> ProjectHostClient::clCommandLines(const ProjectEvaluationRequest& request,
                                                                std::stop_token stop) {
    auto line = projectHostClCommandLinesRequest(request, std::to_string(m_nextId++));
    if (!line) {
        return std::unexpected(std::move(line.error()));
    }
    auto answer = exchange(*line, stop);
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    auto parsed = parseProjectHostClCommandLines(*answer);
    if (parsed && parsed->projectPath.empty()) {
        parsed->projectPath = request.projectPath;
    }
    return parsed;
}

Result<HostEvaluation> ProjectHostClient::evaluate(const ProjectEvaluationRequest& request, std::stop_token stop) {
    const std::string id = std::to_string(m_nextId++);
    auto line = projectHostEvaluateRequest(request, id);
    if (!line) {
        return std::unexpected(std::move(line.error()));
    }
    auto answer = exchange(*line, stop);
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    auto parsed = parseProjectHostEvaluation(*answer);
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (parsed->projectPath.empty()) {
        parsed->projectPath = request.projectPath;
    }
    return parsed;
}

namespace detail {

// For solution_evaluation.cpp: one MSBuild.exe fallback evaluation.
Result<HostEvaluation>
evaluateWithMsBuildCli(io::IProcessLauncher& launcher, std::string_view msbuildPath,
                       const ProjectEvaluationRequest& request,
                       const std::vector<std::pair<std::string, std::optional<std::string>>>& environment,
                       IClock::Duration timeout, std::stop_token stop) {
    auto spec = msbuildCliEvaluationSpec(msbuildPath, request);
    if (!spec) {
        return std::unexpected(std::move(spec.error()));
    }
    spec->environment.insert(spec->environment.end(), environment.begin(), environment.end());
    auto ran = runCli(launcher, *spec, timeout, stop);
    if (!ran) {
        return std::unexpected(std::move(ran.error()));
    }
    if (ran->exitCode != 0) {
        // MSBuild prints its error (MSB4025, MSB4019, ...) on stdout.
        std::string message(detail::trim(ran->stdoutBytes.empty() ? ran->stderrBytes : ran->stdoutBytes));
        if (message.size() > 400) {
            message.resize(400);
        }
        return fail(ErrorCode::IoError, std::format("MSBuild.exe exited with {}: {}", ran->exitCode, message));
    }
    return parseMsBuildCliEvaluation(ran->stdoutBytes, request);
}

} // namespace detail

} // namespace frost::project
