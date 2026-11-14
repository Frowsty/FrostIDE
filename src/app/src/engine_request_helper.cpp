// SPDX-License-Identifier: Apache-2.0
// Filesystem and engine-controlled paths are resolved only in the cancellable helper process.
#include "engine_request_helper.hpp"

#include "frost/engine/command_policy.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/engine_automation.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/trust_store.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <chrono>
#include <optional>

namespace frost::app {
Result<lang::Json> executeEngineSessionHelper(const lang::Json& input);
namespace {
QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
std::string utf8(const QString& s) { return s.toUtf8().toStdString(); }
QString physical(std::string_view path) {
    if (!io::isAbsolutePath(path) || path.size() > 32768)
        return {};
    return QFileInfo(qs(path)).canonicalFilePath();
}
bool sameProject(std::string_view a, std::string_view b) {
    const QString left = physical(a), right = physical(b);
    return !left.isEmpty() && !right.isEmpty() && lang::documentKey(utf8(left)) == lang::documentKey(utf8(right));
}
bool insideRoot(const QString& file, const QString& root) {
    auto prefix = lang::documentKey(utf8(root));
#ifdef _WIN32
    constexpr char separator = '\\';
#else
    constexpr char separator = '/';
#endif
    if (prefix.empty() || prefix.back() != separator)
        prefix.push_back(separator);
    return lang::documentKey(utf8(file)).starts_with(prefix);
}
Result<lang::Json> readRecord(std::string_view path) {
    auto& fs = io::realFileSystem();
    auto info = fs.status(path);
    if (!info)
        return std::unexpected(std::move(info.error()));
    if (info->type != io::FileType::Regular || info->size > 65536)
        return fail(ErrorCode::Corrupt, "invalid engine discovery record");
    auto bytes = fs.readFile(path);
    if (!bytes || bytes->size() > 65536)
        return fail(ErrorCode::Corrupt, "invalid engine discovery record");
    auto json = lang::parseJson(*bytes);
    if (!json || !json->isObject())
        return fail(ErrorCode::Corrupt, "invalid engine discovery record");
    return json;
}
Result<lang::Json> findRecord(std::string_view directory, std::string_view root) {
    auto projects = io::joinPath(directory, "Projects");
    if (!projects)
        return fail(ErrorCode::InvalidArgument, "invalid engine discovery directory");
    auto entries = io::realFileSystem().listDirectory(*projects);
    std::optional<lang::Json> matched;
    if (entries) {
        if (entries->size() > 512)
            return fail(ErrorCode::OutOfRange, "too many engine discovery records");
        for (const auto& entry : *entries) {
            if (entry.type != io::FileType::Regular || entry.name.size() != 45 || !entry.name.ends_with(".json"))
                continue;
            auto path = io::joinPath(*projects, entry.name);
            if (!path)
                continue;
            auto record = readRecord(*path);
            if (!record || !sameProject(root, record->stringAt("projectPath")))
                continue;
            if (matched)
                return fail(ErrorCode::Busy, "multiple engine editors advertise this project");
            matched = std::move(*record);
        }
    } else if (entries.error().code() != ErrorCode::NotFound) {
        return fail(ErrorCode::IoError, "cannot inspect engine discovery directory");
    }
    if (matched)
        return std::move(*matched);
    auto fallback = io::joinPath(directory, "Automation.json");
    if (!fallback)
        return fail(ErrorCode::InvalidArgument, "invalid engine discovery directory");
    return readRecord(*fallback);
}
void collectLocations(const lang::Json& value, const QString& root, lang::Json& locations, std::size_t& count) {
    if (count >= 4096)
        return;
    if (value.isArray()) {
        for (const auto& item : value.items())
            collectLocations(item, root, locations, count);
        return;
    }
    if (!value.isObject())
        return;
    const auto* source = value.find("source");
    const std::string_view path = source && source->isObject() ? source->stringAt("path") : value.stringAt("path");
    if (!path.empty() && path.size() <= 4096 && path.find('\0') == std::string_view::npos &&
        !io::isAbsolutePath(path)) {
        QString relative = QDir::fromNativeSeparators(qs(path));
        const auto parts = relative.split('/');
        if (relative.startsWith(QStringLiteral("Assets/")) && !parts.contains(QStringLiteral("..")) &&
            relative.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive)) {
            const QString resolved = QFileInfo(QDir(root).filePath(relative)).canonicalFilePath();
            if (!resolved.isEmpty() && QFileInfo(resolved).isFile() && insideRoot(resolved, root)) {
                locations.set(std::string(path), utf8(resolved));
                ++count;
            }
        }
    }
    for (const auto& item : value.items())
        collectLocations(item, root, locations, count);
}
} // namespace

Result<lang::Json> executeEngineHelper(const lang::Json& input, const lang::EngineConnect& connect) {
    const std::string root(input.stringAt("root")), directory(input.stringAt("directory"));
    const std::string trustPath(input.stringAt("trustPath")), method(input.stringAt("method"));
    const auto* params = input.find("params");
    if (!io::isAbsolutePath(root) || !io::isAbsolutePath(directory) || !io::isAbsolutePath(trustPath) || !params ||
        !params->isObject())
        return fail(ErrorCode::InvalidArgument, "invalid engine request");
    const QString canonicalRoot = physical(root);
    if (canonicalRoot.isEmpty())
        return fail(ErrorCode::NotFound, "project directory is unavailable");
    auto trust = project::TrustStore::open(io::realFileSystem(), trustPath);
    if (!trust || !(*trust)->isTrusted(root))
        return fail(ErrorCode::Untrusted, "project is not trusted");
    if (method == "script.validate" || method == "navigation.resolve") {
        const auto path = params->stringAt("path");
        if (path.empty() || path.size() > 4096 || path.find('\0') != std::string_view::npos || io::isAbsolutePath(path))
            return fail(ErrorCode::InvalidArgument, "invalid script path");
        const QString relative = QDir::fromNativeSeparators(qs(path));
        if (!relative.startsWith(QStringLiteral("Assets/")) || relative.split('/').contains(QStringLiteral("..")) ||
            !relative.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive))
            return fail(ErrorCode::InvalidArgument, "invalid script path");
        const QString target = QFileInfo(QDir(canonicalRoot).filePath(relative)).canonicalFilePath();
        if (target.isEmpty() || !QFileInfo(target).isFile() || !insideRoot(target, canonicalRoot))
            return fail(ErrorCode::InvalidArgument, "script is outside the project");
        if (method == "navigation.resolve")
            return lang::Json::object()
                .set("result", lang::Json::object().set("path", utf8(target)))
                .set("locations", lang::Json::object());
    }
    auto record = findRecord(directory, root);
    if (!record)
        return std::unexpected(std::move(record.error()));
    // Re-read the persisted trust decision immediately before the action. The parent cancels
    // the helper on project changes and shutdown.
    auto stillTrusted = [&] {
        auto fresh = project::TrustStore::open(io::realFileSystem(), trustPath);
        return fresh && (*fresh)->isTrusted(root);
    };
    // Per-method deadline (engineMethodTimeout): test.run has none and ends only when the parent
    // cancels this helper (which destroys its process); play.* and log.get get a short one.
    const auto timeout = lang::engineMethodTimeout(method);
    const auto deadline =
        timeout ? std::chrono::steady_clock::now() + *timeout : std::chrono::steady_clock::time_point::max();
    // Explicit legacy/injected transport seam only. Production below uses contract-v2 HMAC.
    const lang::EngineCommandAdmission admission = [&](std::string_view command, const lang::Json& arguments,
                                                       const lang::Json& schemaJson, const lang::Json& server,
                                                       const lang::Json& project) -> Result<void> {
        auto schema = engine::parseSchemaDocument(schemaJson);
        if (!schema)
            return std::unexpected(schema.error());
        const auto instance = server.stringAt("instanceId");
        const auto generation = server.stringAt("generation");
        if (instance.empty() || generation.empty())
            return fail(ErrorCode::Unsupported, "Legacy engine has no authenticated schema identity");
        const std::string boundProject = lang::documentKey(utf8(canonicalRoot));
        const std::string boundSession = std::string(instance) + "/" + std::string(generation);
        engine::CommandPolicy policy(std::move(*schema), boundProject, boundSession);
        engine::CommandPolicyContext context;
        context.connected = context.paired = context.mutationAllowed = true;
        context.trusted = stillTrusted();
        context.editorAvailable = server.stringAt("role") == "editor";
        context.selectedProject = boundProject;
        context.sessionProject = lang::documentKey(utf8(physical(project.stringAt("directory"))));
        context.selectedSession = context.sessionId = boundSession;
        context.state = server.stringAt("state"); // Absence never implies Edit.
        if (const auto* capabilities = server.find("capabilities"); capabilities && capabilities->isArray())
            for (const auto& capability : capabilities->items())
                if (const auto* text = capability.asString())
                    context.capabilities.push_back(*text);
        for (const auto& entry : policy.schema().commands)
            context.availableCommands.push_back(entry.name);
        return policy.admit(command, arguments, context);
    };
    auto result = lang::runEngineCommand(*record, root, method, *params, sameProject, stillTrusted, connect, deadline,
                                         {}, admission);
    if (!result)
        return std::unexpected(std::move(result.error()));
    auto locations = lang::Json::object();
    std::size_t count = 0;
    collectLocations(*result, canonicalRoot, locations, count);
    return lang::Json::object().set("result", std::move(*result)).set("locations", std::move(locations));
}
Result<lang::Json> executeEngineHelper(const lang::Json& input) {
    // Navigation is a local, trust-gated physical-path check; no engine connection is necessary.
    if (input.stringAt("method") == "navigation.resolve")
        return executeEngineHelper(input, lang::EngineConnect{});
    const QString root = physical(input.stringAt("root"));
    if (root.isEmpty())
        return fail(ErrorCode::NotFound, "The project directory is unavailable");
    auto result = executeEngineSessionHelper(input);
    if (!result)
        return std::unexpected(result.error());
    if (physical(input.stringAt("root")) != root)
        return fail(ErrorCode::PermissionDenied, "The physical project changed while the engine was responding");
    auto locations = lang::Json::object();
    std::size_t count = 0;
    collectLocations(*result, root, locations, count);
    return lang::Json::object().set("result", std::move(*result)).set("locations", std::move(locations));
}
} // namespace frost::app
