// SPDX-License-Identifier: Apache-2.0
// Runs the bundled snapshot validator off the UI thread and publishes only current results.

#include "frost/app/document_editor.hpp"
#include "frost/app/workbench.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/snapshot_validator.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"
#include "frost/ui/shell/notifications.hpp"

#include "language_bridge.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>

#include <algorithm>
#include <chrono>

namespace frost::app {
namespace {

using Diagnostics = std::vector<lang::LiveDiagnostic>;

Result<Diagnostics> decodeValidation(std::string_view bytes, std::string_view path, const text::Snapshot& snapshot,
                                     bool engineSchema) {
    if (bytes.size() > lang::kSnapshotValidationMaxOutputBytes)
        return fail(ErrorCode::OutOfRange, "Validator response exceeded its output limit");
    auto response = rpc::parseJsonStrict(bytes);
    if (!response || !response->isObject() || response->size() != 2 ||
        response->integerAt("version") != lang::kSnapshotValidationVersion)
        return fail(ErrorCode::Corrupt, "Unsupported validator response");
    const auto* issues = response->find("issues");
    if (!issues || !issues->isArray() || issues->size() > lang::kSnapshotValidationMaxIssues)
        return fail(ErrorCode::Corrupt, "Invalid validator issue array");

    const bool cmake = lang::isCMakeSource(path);
    const std::string_view provider = cmake          ? "FrostIDE CMake source parser"
                                      : engineSchema ? "frost-engine-schema-shape"
                                                     : "frost-json-schema";
    Diagnostics diagnostics;
    diagnostics.reserve(issues->size());
    for (const auto& issue : issues->items()) {
        const auto source = issue.stringAt("source");
        const auto code = issue.stringAt("code");
        const auto message = issue.stringAt("message");
        const auto begin = issue.integerAt("begin"), end = issue.integerAt("end");
        const bool validCode = cmake ? (code == "cmake.syntax" || code == "cmake.duplicate-target")
                                     : (code == "syntax" || code == "schema");
        if (!issue.isObject() || issue.size() != 5 || source != provider || !validCode || message.empty() || !begin ||
            !end || *begin < 0 || *end < *begin || static_cast<std::uint64_t>(*end) > snapshot.size().value())
            return fail(ErrorCode::Corrupt, "Invalid validator issue or document range");
        const ByteOffset first{static_cast<std::uint64_t>(*begin)}, last{static_cast<std::uint64_t>(*end)};
        if (!snapshot.isCharBoundary(first) || !snapshot.isCharBoundary(last))
            return fail(ErrorCode::Corrupt, "Validator range splits a Unicode character");

        lang::LiveDiagnostic diagnostic;
        diagnostic.source = source;
        diagnostic.code = code;
        diagnostic.message = message;
        diagnostic.file = path;
        if (!cmake)
            diagnostic.range = text::ByteRange{first, last};
        diagnostic.textVersion = snapshot.version();
        diagnostic.lspRange = {lang::toLspPosition(snapshot, first, lang::PositionEncoding::Utf16),
                               lang::toLspPosition(snapshot, last, lang::PositionEncoding::Utf16)};
        diagnostic.line = static_cast<int>(diagnostic.lspRange.start.line) + 1;
        diagnostic.column = static_cast<int>(diagnostic.lspRange.start.character) + 1;
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}

sched::Task<void> validateSnapshot(io::IProcessLauncher& launcher, io::ProcessSpec spec, std::string path,
                                   text::Snapshot snapshot, std::shared_ptr<lang::JsonSchemaService> schemas,
                                   std::stop_token stop, std::function<void(Result<Diagnostics>)> done) {
    auto result = [&]() -> Result<Diagnostics> {
        const auto engineSchema = lang::isCMakeSource(path) ? std::nullopt : schemas->snapshotValidationSchema(path);
        auto request = lang::makeSnapshotValidationRequest(path, snapshot, engineSchema);
        if (!request)
            return std::unexpected(std::move(request.error()));
        auto response = io::runToCompletion(launcher, spec, *request, std::chrono::seconds(5), stop);
        if (!response)
            return std::unexpected(std::move(response.error()));
        if (response->exitCode != 0)
            return fail(ErrorCode::IoError, response->stderrBytes.empty()
                                                ? "Validator exited without returning diagnostics"
                                                : response->stderrBytes.substr(0, 512));
        return decodeValidation(response->stdoutBytes, path, snapshot, engineSchema.has_value());
    }();
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [done = std::move(done), result = std::move(result)]() mutable { done(std::move(result)); },
        Qt::QueuedConnection);
    co_return;
}

} // namespace

void LanguageBridge::closeJsonDocument(std::string_view path) {
    m_json->removeDocument(path);
    m_validationDocuments.erase(lang::documentKey(path));
}

void LanguageBridge::invalidateJsonValidation() {
    const auto isJsonValidation = [](const ValidationDocument& document) {
        return !lang::isCMakeSource(document.path);
    };
    for (auto* view : m_workbench.m_editors->views())
        if (const auto entry = m_validationDocuments.find(lang::documentKey(view->path()));
            entry != m_validationDocuments.end() && isJsonValidation(entry->second))
            view->editor().setDiagnostics(ui::EditorDiagnosticSource::SnapshotValidation, {});
    std::erase_if(m_validationDocuments, [&](const auto& pair) { return isJsonValidation(pair.second); });
}

Result<void> LanguageBridge::installEngineDocumentSchemas(std::string root, const lang::Json& schemas) {
    auto installed = m_json->installEngineSchemas(std::move(root), schemas);
    if (installed)
        invalidateJsonValidation();
    return installed;
}

Result<void> LanguageBridge::installEngineTableSchema(std::string path, const text::Snapshot& snapshot,
                                                      const lang::Json& reply) {
    auto installed = m_json->installEngineTableSchema(path, snapshot, reply);
    if (installed)
        m_validationDocuments.erase(lang::documentKey(path));
    return installed;
}

void LanguageBridge::clearEngineDocumentSchemas() {
    m_json->clearEngineSchemas();
    invalidateJsonValidation();
    if (m_workbench.m_editors && !m_workbench.m_shutDown)
        for (auto* view : m_workbench.m_editors->uniqueDocumentViews())
            syncSnapshotValidation(*view);
}

std::vector<lang::LiveDiagnostic> LanguageBridge::snapshotDiagnostics(std::string_view path) const {
    const auto found = m_validationDocuments.find(lang::documentKey(path));
    if (found == m_validationDocuments.end() || found->second.diagnostics.empty())
        return {};
    auto* view = m_workbench.m_editors->find(path);
    if (!view || view->document().document().version() != found->second.version)
        return {};
    return found->second.diagnostics;
}

void LanguageBridge::paintSnapshotValidation(DocumentEditorView& view) const {
    lang::FileDiagnostics set;
    set.file = view.path();
    set.textVersion = view.editor().editor().text().version();
    set.diagnostics = snapshotDiagnostics(view.path());
    view.editor().setDiagnostics(ui::EditorDiagnosticSource::SnapshotValidation, ui::lang::toEditorDiagnostics(set));
}

void LanguageBridge::syncSnapshotValidation(DocumentEditorView& view) {
    const auto& path = view.path();
    const bool json = handlesJsonDocument(path);
    if (!json && !lang::isCMakeSource(path))
        return;
    const auto snapshot = view.editor().editor().text();
    const auto key = lang::documentKey(path);
    auto [entry, inserted] = m_validationDocuments.try_emplace(key);
    if (!inserted && entry->second.version == snapshot.version()) {
        paintSnapshotValidation(view);
        return;
    }
    entry->second = {path, snapshot.version(), ++m_validationSerial, snapshot, {}};
    if (json)
        m_json->updateDocument(path, snapshot);
    paintSnapshotValidation(view);
    for (auto* peer : m_workbench.m_editors->views())
        if (peer != &view && lang::documentKey(peer->path()) == key)
            paintSnapshotValidation(*peer);
    m_workbench.refreshLiveProblems();
    if (!m_validationRunning && !m_validationTimer.isActive())
        m_validationTimer.start();
}

void LanguageBridge::startSnapshotValidation() {
    if (m_validationRunning || m_workbench.m_shutDown)
        return;
    auto next = m_validationDocuments.end();
    for (auto entry = m_validationDocuments.begin(); entry != m_validationDocuments.end(); ++entry)
        if (entry->second.pending &&
            (next == m_validationDocuments.end() || entry->second.serial < next->second.serial))
            next = entry;
    if (next == m_validationDocuments.end())
        return;

    auto& document = next->second;
    auto snapshot = std::move(*document.pending);
    document.pending.reset();
    m_validationRunning = true;
#ifdef _WIN32
    const auto executable = QStringLiteral("frost-snapshot-validator.exe");
#else
    const auto executable = QStringLiteral("frost-snapshot-validator");
#endif
    io::ProcessSpec spec;
    spec.executable = QDir(QCoreApplication::applicationDirPath()).filePath(executable).toUtf8().toStdString();
    spec.workingDirectory = QCoreApplication::applicationDirPath().toUtf8().toStdString();
    spec.stdinMode = io::Stdio::Pipe;
    spec.belowNormalPriority = true;
    const auto done = [self = QPointer<Workbench>(&m_workbench), key = next->first,
                       serial = document.serial](Result<Diagnostics> result) {
        if (self && !self->m_shutDown)
            self->m_bridge->finishSnapshotValidation(key, serial, std::move(result));
    };
    m_workbench.m_pool
        ->spawn(sched::Lane::Background,
                validateSnapshot(m_workbench.m_services.launcher, std::move(spec), document.path, std::move(snapshot),
                                 m_json, m_validationStop.get_token(), done))
        .detach();
}

void LanguageBridge::finishSnapshotValidation(const std::string& key, std::uint64_t serial,
                                              Result<std::vector<lang::LiveDiagnostic>> result) {
    const auto found = m_validationDocuments.find(key);
    auto* view = found != m_validationDocuments.end() ? m_workbench.m_editors->find(found->second.path) : nullptr;
    const bool current =
        view && found->second.serial == serial && view->document().document().version() == found->second.version;
    // Launch failure describes the shared executable, even if the requesting tab has closed.
    const bool validatorMissing = !result && result.error().code() == ErrorCode::NotFound;
    if (current || validatorMissing) {
        if (result) {
            m_validationError.clear();
            found->second.diagnostics = std::move(*result);
        } else {
            if (validatorMissing)
                for (auto& [path, document] : m_validationDocuments)
                    document.diagnostics.clear();
            const auto& reason = result.error().message();
            if (m_validationError != reason) {
                m_validationError = reason;
                m_workbench.notify(QStringLiteral("Live document validation unavailable"),
                                   QString::fromStdString(m_validationError), ui::NotificationKind::Warning);
            }
        }
        for (auto* peer : m_workbench.m_editors->views()) {
            const auto peerKey = lang::documentKey(peer->path());
            if (peerKey == key || (validatorMissing && m_validationDocuments.contains(peerKey)))
                paintSnapshotValidation(*peer);
        }
        m_workbench.refreshLiveProblems();
    }
    m_validationRunning = false;
    startSnapshotValidation();
}

void LanguageBridge::stopSnapshotValidation() {
    m_validationTimer.stop();
    m_validationStop.request_stop();
}

} // namespace frost::app
