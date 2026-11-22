// SPDX-License-Identifier: Apache-2.0
// FEC7/8 selected-engine schema editing and actual serializer/content Problems, with dirty-image guards.
#include "frost/engine/document_tools.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "engine_integration_internal.hpp"
#include "language_bridge.hpp"
#include <QDir>
#include <QMetaObject>

#include <algorithm>
#include <tuple>
namespace frost::app {
namespace {
QString displayDocument(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
build::BuildDiagnostic rowFor(const engine::DocumentIssue& issue, std::string path,
                              const text::Snapshot* source = nullptr) {
    build::BuildDiagnostic row;
    row.path = std::move(path);
    auto& diagnostic = row.diagnostic;
    diagnostic.code = issue.code;
    diagnostic.message = issue.message;
    if (!issue.location.empty())
        diagnostic.message += " [" + issue.location + "]";
    diagnostic.severity = issue.severity == "error"     ? build::Severity::Error
                          : issue.severity == "warning" ? build::Severity::Warning
                                                        : build::Severity::Message;
    diagnostic.line = static_cast<int>(issue.line);
    diagnostic.column = static_cast<int>(issue.column);
    if (source && issue.byte) {
        auto offset = std::min<std::uint64_t>(issue.byte - 1, source->size().value());
        while (offset && !source->isCharBoundary(ByteOffset{offset}))
            --offset;
        const auto position = source->utf16PointAt(ByteOffset{offset});
        diagnostic.line = static_cast<int>(position.line.value()) + 1;
        diagnostic.column = static_cast<int>(position.column.value()) + 1;
    }
    row.jumpable = !row.path.empty() && diagnostic.line > 0;
    return row;
}
} // namespace
void EngineIntegration::Impl::publishDocumentProblems() {
    if (!workbench.m_problems)
        return;
    auto rows = diagnostics;
    for (const auto& [key, values] : documentDiagnostics)
        rows.insert(rows.end(), values.begin(), values.end());
    workbench.m_problems->model()->setProblems(ui::DiagnosticSource::Engine, std::move(rows));
}
bool EngineIntegration::Impl::retainDocumentProblems(std::string key, std::vector<build::BuildDiagnostic> rows) {
    const auto size = [](const build::BuildDiagnostic& row) {
        return row.diagnostic.message.size() + row.path.size() + row.diagnostic.code.size() + 128;
    };
    documentDiagnostics.erase(key);
    std::size_t count = 0, bytes = 0;
    for (const auto& row : rows) {
        if (count == 10000 || bytes + size(row) > 4 * 1024 * 1024)
            break;
        bytes += size(row);
        ++count;
    }
    const bool complete = count == rows.size();
    rows.resize(count);
    // One bounded recent engine-validation view, alongside the separately bounded push stream.
    while (!documentDiagnostics.empty()) {
        std::size_t retainedRows = 0, retainedBytes = 0;
        for (const auto& [old, values] : documentDiagnostics) {
            retainedRows += values.size();
            for (const auto& row : values)
                retainedBytes += size(row);
        }
        if (retainedRows + count <= 10000 && retainedBytes + bytes <= 4 * 1024 * 1024 &&
            documentDiagnostics.size() < 64)
            break;
        documentVersions.erase(documentDiagnostics.begin()->first);
        documentDiagnostics.erase(documentDiagnostics.begin());
    }
    documentDiagnostics.emplace(std::move(key), std::move(rows));
    publishDocumentProblems();
    return complete;
}
void EngineIntegration::clearDocumentTools() {
    auto& impl = *m_impl;
    ++impl.documentRequestSerial;
    impl.documentRequests.clear();
    impl.documentVersions.clear();
    impl.documentDiagnostics.clear();
    impl.documentSchemasRequested = false;
    impl.publishDocumentProblems();
    if (!impl.workbench.m_bridge)
        return;
    auto& workbench = impl.workbench;
    workbench.m_bridge->clearEngineDocumentSchemas();
    if (workbench.m_language)
        workbench.updateLanguageTarget();
    workbench.refreshLiveProblems();
}
void EngineIntegration::documentEdited(std::string_view path, std::uint64_t version) {
    auto& impl = *m_impl;
    const auto key = lang::documentKey(path);
    const auto found = impl.documentVersions.find(key);
    if (found != impl.documentVersions.end() && found->second != version) {
        impl.documentVersions.erase(found);
        impl.documentDiagnostics.erase(key);
        impl.publishDocumentProblems();
    }
}
void EngineIntegration::registerDocumentActions() {
    auto& registry = m_impl->workbench.m_registry;
    for (const auto& [id, label, operation] :
         {std::tuple{"engine.document.schemas", "Refresh Engine Document Schemas", 0},
          std::tuple{"engine.document.validate", "Validate Captured Engine Document", 1},
          std::tuple{"engine.content.validate", "Validate On-Disk Engine Project", 2},
          std::tuple{"engine.document.tableSchema", "Load Actual Data Table Row Schema", 3}}) {
        input::ActionSpec action;
        action.id = id;
        action.name = label;
        action.group = "FrostEngine";
        action.handler = [this, operation](const input::ActionContext&, const input::InvokeInfo&) {
            auto result = operation == 0   ? refreshDocumentSchemas()
                          : operation == 1 ? validateDocument()
                          : operation == 2 ? validateContent()
                                           : refreshTableSchema();
            if (!result)
                m_impl->setStatus(displayDocument(result.error().message()));
        };
        (void)registry.add(std::move(action));
    }
    connect(this, &EngineIntegration::stateObserved, this, [this] {
        auto& impl = *m_impl;
        if (!impl.documentSchemasRequested && impl.state && impl.state->state == "Edit") {
            auto refreshed = refreshDocumentSchemas();
            if (!refreshed)
                impl.setStatus(displayDocument(refreshed.error().message()));
        }
    });
}
Result<void> EngineIntegration::refreshDocumentSchemas() {
    auto& impl = *m_impl;
    if (!impl.session || !impl.identity)
        return fail(ErrorCode::Busy, "Pair an editor before loading document schemas");
    impl.documentSchemasRequested = true;
    const auto serial = ++impl.documentRequestSerial;
    impl.documentRequests["$schemas"] = serial;
    QPointer<EngineIntegration> self(this);
    auto requested = contentCommand("document.schemas", lang::Json::object(), [self, serial](Result<lang::Json> reply) {
        if (!self)
            return;
        auto& impl = *self->m_impl;
        if (impl.documentRequests["$schemas"] != serial || !impl.workbench.m_bridge)
            return;
        if (!reply) {
            impl.setStatus(displayDocument(reply.error().message()));
            return;
        }
        auto installed = impl.workbench.m_bridge->installEngineDocumentSchemas(impl.root, *reply);
        if (!installed) {
            impl.setStatus(displayDocument(installed.error().message()));
            return;
        }
        for (auto* view : impl.workbench.m_editors->views())
            impl.workbench.syncJsonDocument(*view);
        impl.workbench.updateLanguageTarget();
        impl.setStatus(QStringLiteral("Actual engine serializer field shapes loaded; semantic validation is separate"));
    });
    if (!requested)
        return std::unexpected(requested.error());
    return {};
}
Result<void> EngineIntegration::validateDocument() { return requestDocumentTool("document.validate"); }
Result<void> EngineIntegration::refreshTableSchema() { return requestDocumentTool("document.table-schema"); }
Result<void> EngineIntegration::requestDocumentTool(const std::string& method) {
    auto& impl = *m_impl;
    if (!impl.session || !impl.identity)
        return fail(ErrorCode::Busy, "Pair an editor before validating a document");
    auto* view = impl.workbench.currentView();
    if (!view || (!view->path().ends_with(".fmat") && !view->path().ends_with(".fscene") &&
                  !view->path().ends_with(".fschema") && !view->path().ends_with(".ftable")))
        return fail(ErrorCode::Unsupported, "Open a supported engine material, scene, data schema or table");
    if (method == "document.table-schema" && !view->path().ends_with(".ftable"))
        return fail(ErrorCode::Unsupported, "Open a data table to load its actual resolved row schema");
    const auto source = view->document().document().text();
    if (source.size().value() > 256 * 1024)
        return fail(ErrorCode::OutOfRange, "Engine dirty-image validation is limited to 256 KiB");
    const auto path = view->path(), root = impl.root, physicalRoot = impl.physicalRoot, projectKey = impl.projectKey;
    const auto admission = impl.physicalAdmission;
    const auto epoch = impl.epoch, sessionEpoch = impl.sessionEpoch, revision = impl.trustRevision;
    const auto stop = impl.workStop.get_token();
    const auto serial = ++impl.documentRequestSerial;
    const auto key = lang::documentKey(path);
    if (impl.documentRequests.size() >= 128 && !impl.documentRequests.contains(key))
        impl.documentRequests.clear();
    impl.documentRequests[key] = serial;
    auto* fs = &impl.workbench.m_services.fs;
    QPointer<EngineIntegration> self(this);
    impl.background([self, fs, root, physicalRoot, projectKey, admission, epoch, sessionEpoch, revision, stop, path,
                     key, source, serial, method] {
        if (stop.stop_requested() || !retainEngineProjectIdentity(*fs, root, projectKey, admission))
            return;
        auto physical = fs->physicalPath(path);
        if (!physical)
            return;
        const auto relative = QDir(displayDocument(physicalRoot)).relativeFilePath(displayDocument(*physical));
        const auto portable = QDir::fromNativeSeparators(relative).toUtf8().toStdString();
        if (!portable.starts_with("Assets/") || portable.find("../") != portable.npos)
            return;
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, root, epoch, sessionEpoch, revision, path, key, physical = *physical, portable, source, serial,
             method] {
                if (!self)
                    return;
                auto& impl = *self->m_impl;
                if (impl.sessionEpoch != sessionEpoch || !impl.admitted(epoch, root, revision))
                    return;
                auto* current = impl.workbench.m_editors->find(path);
                if (!current || current->document().document().text().version() != source.version())
                    return;
                auto requested = self->contentCommand(
                    method, lang::Json::object().set("path", portable).set("source", source.text()),
                    [self, path, key, physical, portable, source, serial, method](Result<lang::Json> reply) {
                        if (!self)
                            return;
                        auto& impl = *self->m_impl;
                        if (impl.documentRequests[key] != serial)
                            return;
                        auto* view = impl.workbench.m_editors->find(path);
                        auto actual = impl.workbench.m_services.fs.physicalPath(path);
                        if (!view || view->document().document().text().version() != source.version() ||
                            view->document().document().text().text() != source.text() || !actual ||
                            lang::documentKey(*actual) != lang::documentKey(physical)) {
                            impl.setStatus(QStringLiteral("Document changed; stale engine validation was discarded"));
                            return;
                        }
                        if (!reply) {
                            impl.setStatus(displayDocument(reply.error().message()));
                            return;
                        }
                        if (method == "document.table-schema") {
                            if (reply->stringAt("path") != portable) {
                                impl.setStatus(QStringLiteral("Actual table schema reply names another image"));
                                return;
                            }
                            auto installed = impl.workbench.m_bridge->installEngineTableSchema(path, source, *reply);
                            if (!installed) {
                                impl.setStatus(displayDocument(installed.error().message()));
                                return;
                            }
                            impl.workbench.syncJsonDocument(*view);
                            impl.workbench.updateLanguageTarget();
                            impl.setStatus(QStringLiteral("Actual schema.info inherited columns, defaults and "
                                                          "constraints loaded for this captured Schema reference"));
                            return;
                        }
                        auto parsed = engine::parseDocumentValidation(*reply, portable);
                        if (!parsed) {
                            impl.setStatus(displayDocument(parsed.error().message()));
                            return;
                        }
                        std::vector<build::BuildDiagnostic> rows;
                        for (const auto& issue : parsed->issues)
                            rows.push_back(rowFor(issue, path, &source));
                        impl.documentVersions[key] = source.version();
                        const bool retained = impl.retainDocumentProblems(key, std::move(rows));
                        impl.setStatus(!retained ? QStringLiteral("Actual engine validation exceeds the Problems "
                                                                  "retention limit; a bounded prefix is shown")
                                       : parsed->valid
                                           ? QStringLiteral("Actual engine serializer accepted the captured document")
                                           : QStringLiteral("Actual engine document issues are in Problems"));
                    });
                if (!requested)
                    impl.setStatus(displayDocument(requested.error().message()));
            },
            Qt::QueuedConnection);
    });
    return {};
}
Result<void> EngineIntegration::validateContent() {
    auto& impl = *m_impl;
    const auto serial = ++impl.documentRequestSerial;
    impl.documentRequests["$content"] = serial;
    QPointer<EngineIntegration> self(this);
    auto requested =
        contentCommand("document.validate-content", lang::Json::object(), [self, serial](Result<lang::Json> reply) {
            if (!self)
                return;
            auto& impl = *self->m_impl;
            if (impl.documentRequests["$content"] != serial)
                return;
            if (!reply) {
                impl.setStatus(displayDocument(reply.error().message()));
                return;
            }
            auto parsed = engine::parseDocumentValidation(*reply);
            if (!parsed) {
                impl.setStatus(displayDocument(parsed.error().message()));
                return;
            }
            // Location paths are engine project-relative. Only validated physical descendants become jump targets.
            std::vector<build::BuildDiagnostic> rows;
            for (const auto& issue : parsed->issues) {
                std::string path;
                if (!issue.asset.empty())
                    if (auto joined = io::joinPath(impl.physicalRoot, issue.asset))
                        if (auto actual = impl.workbench.m_services.fs.physicalPath(*joined)) {
                            const auto relative =
                                QDir(displayDocument(impl.physicalRoot)).relativeFilePath(displayDocument(*actual));
                            if (!relative.startsWith(QStringLiteral("..")) && !QDir::isAbsolutePath(relative))
                                path = *actual;
                        }
                auto row = rowFor(issue, std::move(path));
                if (row.path.empty() && !issue.asset.empty())
                    row.diagnostic.message += " [" + issue.asset + "]";
                rows.push_back(std::move(row));
            }
            const bool retained = impl.retainDocumentProblems("$content", std::move(rows));
            impl.setStatus(
                retained
                    ? QStringLiteral(
                          "Actual on-disk project validation is in Problems; dirty document validation is separate")
                    : QStringLiteral(
                          "Actual on-disk report exceeds the Problems retention limit; a bounded prefix is shown"));
        });
    if (!requested)
        return std::unexpected(requested.error());
    return {};
}
} // namespace frost::app
