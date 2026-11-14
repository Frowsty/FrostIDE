// SPDX-License-Identifier: Apache-2.0
// Bounded, ordered push-event delivery and physically validated console/Problems navigation.
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/toolwindows/engine_console.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/workbench/workbench.hpp"

#include "engine_integration_internal.hpp"
#include <QMetaObject>
#include <QRegularExpression>

#include <algorithm>
#include <climits>
#include <type_traits>

namespace frost::app {
namespace {
QString qs(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }

bool within(std::string_view root, std::string_view file) {
    std::string prefix = lang::documentKey(root);
    if (!prefix.ends_with('/') && !prefix.ends_with('\\'))
        prefix += io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';
    return lang::documentKey(file).starts_with(prefix);
}

Result<std::string> sourcePath(io::IFileSystem& fs, std::string_view physicalRoot, std::string_view file) {
    if (file.empty() || file.find('\0') != std::string_view::npos)
        return fail(ErrorCode::InvalidArgument, "Engine reported no source file");
    auto absolute = io::isAbsolutePath(file) ? io::normalizePath(file) : io::joinPath(physicalRoot, file);
    if (!absolute)
        return std::unexpected(absolute.error());
    auto path = fs.physicalPath(*absolute);
    if (!path || !within(physicalRoot, *path))
        return fail(ErrorCode::PermissionDenied, "Engine source is outside the selected physical project");
    const QString suffix = qs(io::fileName(*path)).section(QLatin1Char('.'), -1).toLower();
    if (suffix == QStringLiteral("sln") || suffix == QStringLiteral("slnx") || suffix == QStringLiteral("slnf") ||
        suffix == QStringLiteral("frostproj") || suffix == QStringLiteral("feproject"))
        return fail(ErrorCode::InvalidArgument, "An engine source frame cannot switch the IDE project");
    auto status = fs.status(*path);
    if (!status || status->type != io::FileType::Regular)
        return fail(ErrorCode::NotFound, "Engine source file is unavailable");
    return path;
}

void addLocation(io::IFileSystem& fs, std::string_view root, std::string_view file, std::uint64_t line,
                 std::uint64_t column, std::vector<ui::SourceLocation>& locations) {
    if (line == 0 || line > INT_MAX || locations.size() >= 64)
        return;
    auto path = sourcePath(fs, root, file);
    if (!path)
        return;
    const ui::SourceLocation location{*path, static_cast<int>(line), column <= INT_MAX ? static_cast<int>(column) : 0};
    if (!std::ranges::contains(locations, location))
        locations.push_back(location);
}

void collectLocations(io::IFileSystem& fs, std::string_view root, const lang::Json& value,
                      std::vector<ui::SourceLocation>& locations, unsigned depth = 0) {
    if (depth > 12 || locations.size() >= 64)
        return;
    if (value.isObject()) {
        std::string_view file = value.stringAt("file");
        if (file.empty())
            file = value.stringAt("path");
        addLocation(fs, root, file, value.unsignedAt("line").value_or(0), value.unsignedAt("column").value_or(0),
                    locations);
    }
    if (value.isObject() || value.isArray())
        for (const auto& child : value.items())
            collectLocations(fs, root, child, locations, depth + 1);
}

void collectLuaTraceback(io::IFileSystem& fs, std::string_view root, std::string_view message,
                         std::vector<ui::SourceLocation>& locations) {
    // A literal file:line in a Lua traceback is reported source data. It still needs physical
    // containment and file validation; an unresolvable frame remains ordinary message text.
    const QRegularExpression frame(QStringLiteral(R"((?:^|\n)\s*(.+?\.lua):(\d+):)"));
    auto matches = frame.globalMatch(qs(message));
    while (matches.hasNext() && locations.size() < 64) {
        const auto match = matches.next();
        bool valid = false;
        const auto line = match.captured(2).toULongLong(&valid);
        if (valid)
            addLocation(fs, root, match.captured(1).toUtf8().toStdString(), line, 0, locations);
    }
}

struct ResolvedPush {
    EnginePush value;
    std::optional<ui::EngineLogEntry> log;
    std::optional<build::BuildDiagnostic> diagnostic;
};

ResolvedPush resolvePush(io::IFileSystem& fs, std::string_view physicalRoot, EnginePush value) {
    ResolvedPush result{std::move(value), {}, {}};
    std::visit(
        [&](const auto& entry) {
            using T = std::decay_t<decltype(entry)>;
            if constexpr (std::is_same_v<T, engine::EngineLogEntry>) {
                ui::EngineLogEntry shown;
                shown.logger = qs(entry.logger);
                shown.message = qs(entry.message);
                if (const auto* timestamp = entry.data.find("timestamp"))
                    shown.timestamp = timestamp->isString() ? qs(*timestamp->asString()) : qs(timestamp->dump());
                shown.level = entry.level == "error"                              ? ui::EngineLogLevel::Error
                              : entry.level == "critical"                         ? ui::EngineLogLevel::Critical
                              : entry.level == "warn" || entry.level == "warning" ? ui::EngineLogLevel::Warning
                              : entry.level == "debug"                            ? ui::EngineLogLevel::Debug
                              : entry.level == "trace"                            ? ui::EngineLogLevel::Trace
                                                                                  : ui::EngineLogLevel::Info;
                collectLocations(fs, physicalRoot, entry.data, shown.locations);
                collectLuaTraceback(fs, physicalRoot, entry.message, shown.locations);
                addLocation(fs, physicalRoot, entry.file, entry.line, 0, shown.locations);
                result.log = std::move(shown);
            } else if constexpr (std::is_same_v<T, EngineCommandReply>) {
                ui::EngineLogEntry shown;
                shown.logger = QStringLiteral("Command");
                shown.message = qs(entry.method + ": " + entry.message);
                shown.level = entry.success ? ui::EngineLogLevel::Info : ui::EngineLogLevel::Error;
                collectLocations(fs, physicalRoot, entry.data, shown.locations);
                collectLuaTraceback(fs, physicalRoot, entry.message, shown.locations);
                result.log = std::move(shown);
            } else if constexpr (std::is_same_v<T, engine::EngineDiagnostic>) {
                build::BuildDiagnostic row;
                row.diagnostic.severity = entry.severity == "error"     ? build::Severity::Error
                                          : entry.severity == "warning" ? build::Severity::Warning
                                                                        : build::Severity::Message;
                row.diagnostic.message = entry.message;
                row.diagnostic.file = entry.file;
                row.diagnostic.line = entry.line <= INT_MAX ? static_cast<int>(entry.line) : 0;
                row.diagnostic.column = entry.column <= INT_MAX ? static_cast<int>(entry.column) : 0;
                auto path = sourcePath(fs, physicalRoot, entry.file);
                if (path) {
                    row.path = *path;
                    row.jumpable = row.diagnostic.line > 0;
                }
                result.diagnostic = std::move(row);
            }
        },
        result.value);
    return result;
}
} // namespace

void EngineIntegration::Impl::observeEvent(std::string_view topic, const lang::Json& data,
                                           std::optional<std::uint64_t> sequence) {
    if (sequence)
        lastEventSequence = std::max(lastEventSequence, *sequence);
    auto event = lang::Json::object()
                     .set("topic", std::string(topic))
                     .set("sequence", sequence ? lang::Json(*sequence) : lang::Json{})
                     .set("data", data);
    const auto bytes = event.dump().size();
    if (bytes > 1024U * 1024U) {
        ++eventHistoryDropped;
        return;
    }
    while (!eventHistory.empty() && (eventHistory.size() >= 128 || eventHistoryBytes + bytes > 1024U * 1024U)) {
        eventHistoryBytes -= eventHistory.front().second;
        eventHistory.pop_front();
        ++eventHistoryDropped;
    }
    eventHistoryBytes += bytes;
    eventHistory.emplace_back(std::move(event), bytes);
}

void EngineIntegration::Impl::drainPushEvents() {
    const auto queue = eventQueue;
    std::vector<QueuedEnginePush> batch;
    std::uint64_t dropped = 0;
    {
        std::scoped_lock lock(queue->mutex);
        std::size_t bytes = 0;
        while (!queue->pending.empty() && batch.size() < 64 && bytes < 512U * 1024U) {
            auto entry = std::move(queue->pending.front());
            queue->pending.pop_front();
            queue->bytes -= entry.bytes;
            bytes += entry.bytes;
            batch.push_back(std::move(entry));
        }
        if (queue->coalescedState) {
            batch.push_back(std::move(*queue->coalescedState));
            queue->coalescedState.reset();
        }
        dropped = queue->dropped - queue->reportedDropped;
        queue->reportedDropped = queue->dropped;
        if (batch.empty() && dropped == 0) {
            queue->scheduled = false;
            return;
        }
    }
    // Exactly one filesystem batch is in flight for this session. One shared bounded ingress
    // queue absorbs further pushes; neither GUI callback count nor worker count grows per event.
    const auto currentEpoch = epoch;
    const auto currentSessionEpoch = sessionEpoch;
    const auto expectedRoot = root;
    const auto originalPhysicalRoot = physicalRoot;
    const auto originalKey = projectKey;
    const auto admission = physicalAdmission;
    const auto revision = trustRevision;
    auto* fs = &workbench.m_services.fs;
    QPointer<EngineIntegration> self(&owner);
    background([self, queue, fs, currentEpoch, currentSessionEpoch, expectedRoot, originalPhysicalRoot, originalKey,
                admission, revision, dropped, batch = std::move(batch)]() mutable {
        if (!retainEngineProjectIdentity(*fs, expectedRoot, originalKey, admission))
            return;
        std::ranges::stable_sort(batch, {}, &QueuedEnginePush::order);
        std::vector<ResolvedPush> resolved;
        resolved.reserve(batch.size());
        for (auto& event : batch) {
            if (!queue->active.load())
                return;
            resolved.push_back(resolvePush(*fs, originalPhysicalRoot, std::move(event.value)));
        }
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, queue, currentEpoch, currentSessionEpoch, expectedRoot, revision, dropped,
             resolved = std::move(resolved)]() mutable {
                if (!self)
                    return;
                auto& impl = *self->m_impl;
                if (impl.eventQueue != queue || impl.sessionEpoch != currentSessionEpoch ||
                    !impl.admitted(currentEpoch, expectedRoot, revision))
                    return;
                bool diagnosticsChanged = false;
                bool statePublished = false;
                for (auto& event : resolved) {
                    std::visit(
                        [&](const auto& entry) {
                            using T = std::decay_t<decltype(entry)>;
                            if (!impl.currentIdentity(entry.identity))
                                return;
                            if constexpr (std::is_same_v<T, engine::EngineState>) {
                                if (impl.state && entry.sequence < impl.state->sequence)
                                    return;
                                impl.state = entry;
                                statePublished = true;
                                impl.observeEvent("play.state", entry.data, entry.sequence);
                                impl.indicator->setState(entry.state == "DebuggerPaused"
                                                             ? ui::EngineConnectionState::DebuggerPaused
                                                         : entry.paused ? ui::EngineConnectionState::Paused
                                                                        : ui::EngineConnectionState::Connected);
                                impl.console->setPlaying(entry.playing);
                                impl.updateControls();
                            } else if constexpr (std::is_same_v<T, engine::EngineLogEntry>) {
                                impl.observeEvent("log.entry", entry.data, entry.eventSequence);
                                impl.console->append(std::move(*event.log));
                            } else if constexpr (std::is_same_v<T, engine::EngineDiagnostic>) {
                                impl.observeEvent("diagnostic", entry.data, entry.eventSequence);
                                const auto& row = *event.diagnostic;
                                const auto bytes =
                                    row.diagnostic.message.size() + row.diagnostic.file.size() + row.path.size() + 256U;
                                while (!impl.diagnostics.empty() &&
                                       (impl.diagnostics.size() >= 10000 ||
                                        impl.diagnosticBytes + bytes > 4U * 1024U * 1024U)) {
                                    impl.diagnosticBytes -= impl.diagnosticSizes.front();
                                    impl.diagnosticSizes.pop_front();
                                    impl.diagnostics.erase(impl.diagnostics.begin());
                                }
                                impl.diagnosticBytes += bytes;
                                impl.diagnosticSizes.push_back(bytes);
                                impl.diagnostics.push_back(std::move(*event.diagnostic));
                                diagnosticsChanged = true;
                            } else {
                                impl.console->append(std::move(*event.log));
                            }
                        },
                        event.value);
                }
                if (dropped) {
                    ui::EngineLogEntry notice;
                    notice.logger = QStringLiteral("FrostIDE");
                    notice.level = ui::EngineLogLevel::Warning;
                    notice.message =
                        QStringLiteral("Dropped %1 queued engine event(s) to keep the console bounded").arg(dropped);
                    impl.console->append(std::move(notice));
                }
                if (diagnosticsChanged && impl.workbench.m_problems)
                    impl.workbench.m_problems->model()->setProblems(ui::DiagnosticSource::Engine, impl.diagnostics);
                impl.drainPushEvents();
                // Consumers re-fetch the current typed state and identity. Emit only after
                // publishing an admitted engine push, never from a successful command reply.
                if (statePublished && self)
                    emit self->stateObserved();
            },
            Qt::QueuedConnection);
    });
}

void EngineIntegration::Impl::openLocation(std::string file, int line, int column) {
    if (!admitted(epoch, root, trustRevision))
        return;
    const auto currentEpoch = epoch;
    const auto expectedRoot = root;
    const auto originalPhysicalRoot = physicalRoot;
    const auto originalKey = projectKey;
    const auto admission = physicalAdmission;
    const auto revision = trustRevision;
    auto* fs = &workbench.m_services.fs;
    QPointer<EngineIntegration> self(&owner);
    background([self, fs, currentEpoch, expectedRoot, originalPhysicalRoot, originalKey, admission, revision,
                file = std::move(file), line, column] {
        if (!retainEngineProjectIdentity(*fs, expectedRoot, originalKey, admission))
            return;
        auto path = sourcePath(*fs, originalPhysicalRoot, file);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, currentEpoch, expectedRoot, revision, path = std::move(path), line, column] {
                if (!self || !self->m_impl->admitted(currentEpoch, expectedRoot, revision))
                    return;
                if (path) {
                    if (!self->m_impl->workbench.m_nav)
                        return;
                    std::optional<ui::LinePosition> at;
                    if (line > 0)
                        at = ui::LinePosition{line, std::max(column, 1)};
                    (void)self->m_impl->workbench.m_nav->openFile(*path, at);
                } else
                    self->m_impl->setStatus(qs(path.error().message()));
            },
            Qt::QueuedConnection);
    });
}

} // namespace frost::app
