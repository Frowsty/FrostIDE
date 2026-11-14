// SPDX-License-Identifier: Apache-2.0
// REF-10: snapshot published project diagnostics before requesting real quickfixes, then one transaction.
#include "frost/app/workbench.hpp"
#include "frost/lang/refactoring.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/text_file.hpp"

#include "language_bridge.hpp"
#include <QCoreApplication>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>

#include <map>
#include <set>
namespace frost::app {
namespace {
struct ScopeInput {
    std::string path, text;
    std::optional<std::string> raw;
    std::optional<std::int64_t> version;
};
struct ScopeCapture {
    std::string root, database;
    std::uint64_t generation = 0, trustRevision = 0;
    lang::PositionEncoding encoding = lang::PositionEncoding::Utf16;
    std::shared_ptr<lang::ClangdService> server;
    std::vector<lang::FileDiagnostics> diagnostics;
    std::map<std::string, std::pair<text::Snapshot, std::optional<std::int64_t>>> open;
    std::map<std::string, ScopeInput> inputs;
    std::set<std::string> openPhysical;
    lang::ScopeFixes fixes;
    document::RefactoringPlan plan;
    std::stop_token stop;
};
using ScopeResult = Result<std::shared_ptr<ScopeCapture>>;
sched::Task<void> planScope(io::IFileSystem* fs, std::shared_ptr<ScopeCapture> capture,
                            std::function<void(ScopeResult)> done) {
    std::size_t retained = 0;
    for (const auto& [path, snapshot] : capture->open) {
        static_cast<void>(snapshot);
        auto physical = fs->physicalPath(path);
        if (physical)
            capture->openPhysical.insert(lang::documentKey(*physical));
        else if (physical.error().code() != ErrorCode::NotFound) {
            done(std::unexpected(physical.error()));
            co_return;
        }
    }
    if (!capture->database.empty()) {
        auto bytes = fs->readFileSnapshot(capture->database, 32U * 1024U * 1024U);
        if (!bytes) {
            done(std::unexpected(bytes.error()));
            co_return;
        }
        retained += bytes->size();
        capture->plan.dependencies.push_back({capture->database, std::move(*bytes)});
    }
    // Complete every before-image BEFORE the first provider query. No changed file can be
    // adopted later merely because an unversioned WorkspaceEdit happened to name it.
    for (const auto& file : capture->diagnostics) {
        if (capture->stop.stop_requested()) {
            done(fail(ErrorCode::Cancelled, "Fix in scope cancelled"));
            co_return;
        }
        ScopeInput input;
        input.path = file.file;
        const auto key = lang::documentKey(file.file);
        if (const auto open = capture->open.find(key); open != capture->open.end()) {
            input.text = open->second.first.text();
            input.version = open->second.second;
            if (file.lspVersion && input.version != file.lspVersion) {
                done(fail(ErrorCode::Busy, "Diagnostic version no longer matches source: " + file.file));
                co_return;
            }
            capture->plan.bufferDependencies.push_back({file.file, input.text});
        } else {
            auto physical = fs->physicalPath(file.file);
            if (!physical) {
                done(std::unexpected(physical.error()));
                co_return;
            }
            if (capture->openPhysical.contains(lang::documentKey(*physical))) {
                done(fail(ErrorCode::Busy,
                          "A scope disk input aliases an open buffer; request through its captured document path: " +
                              file.file));
                co_return;
            }
            if (file.lspVersion) {
                done(fail(ErrorCode::Busy, "Refresh closed-file diagnostics by opening: " + file.file));
                co_return;
            }
            auto raw = fs->readFileSnapshot(file.file, 64U * 1024U * 1024U);
            if (!raw) {
                done(std::unexpected(raw.error()));
                co_return;
            }
            auto decoded = text::decodeTextFile(*raw);
            if (!decoded) {
                done(std::unexpected(decoded.error()));
                co_return;
            }
            input.text = decoded->buffer.current().text();
            input.raw = std::move(*raw);
            capture->plan.dependencies.push_back({file.file, input.raw});
        }
        retained += input.text.size() + (input.raw ? input.raw->size() : 0);
        if (retained > 512U * 1024U * 1024U) {
            done(fail(ErrorCode::OutOfRange, "Fix scope snapshots exceed 512 MiB"));
            co_return;
        }
        capture->inputs.emplace(key, std::move(input));
    }
    auto fixes = co_await lang::requestFixesInScope(*capture->server, capture->diagnostics, capture->stop);
    if (!fixes) {
        done(std::unexpected(fixes.error()));
        co_return;
    }
    capture->fixes = std::move(*fixes);
    for (const auto& file : capture->fixes.edit.files) {
        const auto found = capture->inputs.find(lang::documentKey(file.path));
        if (found == capture->inputs.end()) {
            done(fail(ErrorCode::Unsupported, "Provider fix edits an uncaptured scope file: " + file.path));
            co_return;
        }
        const auto& input = found->second;
        if (file.version && file.version != input.version) {
            done(fail(ErrorCode::Busy, "Provider edit version differs from captured scope"));
            co_return;
        }
        auto after = lang::applyTextEdits(input.text, file.edits, capture->encoding);
        if (!after) {
            done(std::unexpected(after.error()));
            co_return;
        }
        if (!input.raw)
            capture->plan.buffers.push_back({input.path, input.text, std::move(*after)});
        else {
            auto decoded = text::decodeTextFile(*input.raw);
            if (!decoded) {
                done(std::unexpected(decoded.error()));
                co_return;
            }
            auto buffer = text::TextBuffer::fromUtf8(std::move(*after));
            if (!buffer) {
                done(std::unexpected(buffer.error()));
                co_return;
            }
            auto encoded = text::encodeForSave(buffer->current(), decoded->encoding);
            if (!encoded) {
                done(std::unexpected(encoded.error()));
                co_return;
            }
            capture->plan.disk.push_back({input.path, *input.raw, std::move(*encoded)});
        }
    }
    capture->plan.label = "Fix Published Project Diagnostics: " + std::to_string(capture->fixes.chosenActions.size()) +
                          " selected provider fixes; " + std::to_string(capture->fixes.unresolved.size()) +
                          " unresolved diagnostics";
    done(capture);
}
} // namespace
void Workbench::showCppPublishedFixes() {
    if (m_shutDown || !m_clangd || !m_pool || !m_bridge || m_projectRoot.empty())
        return;
    const auto permission = m_services.trust.snapshot(m_projectRoot);
    if (!permission.trusted) {
        QMessageBox::warning(m_window, QStringLiteral("Fix Published Diagnostics"),
                             QStringLiteral("Trust the project before refactoring."));
        return;
    }
    auto capture = std::make_shared<ScopeCapture>();
    capture->root = m_projectRoot;
    capture->trustRevision = permission.revision;
    capture->generation = m_cppRenameGeneration;
    capture->server = m_clangd;
    capture->encoding = m_bridge->encoding();
    if (const auto database = m_clangd->compileCommands())
        capture->database = database->file;
    for (const auto& [path, snapshot] : m_bridge->openDocuments())
        capture->open.emplace(lang::documentKey(path), std::pair{snapshot, m_clangd->documentVersion(path)});
    for (const auto& file : m_clangd->diagnostics().files())
        if (file.file.starts_with(m_projectRoot) && file.file.size() > m_projectRoot.size() &&
            (file.file[m_projectRoot.size()] == '/' || file.file[m_projectRoot.size()] == '\\'))
            capture->diagnostics.push_back(file);
    if (capture->diagnostics.empty()) {
        QMessageBox::information(m_window, QStringLiteral("Fix Published Diagnostics"),
                                 QStringLiteral("No published project diagnostics are available. This command does not "
                                                "invent diagnostics for unanalyzed files."));
        return;
    }
    auto cancellation = std::make_shared<std::stop_source>();
    capture->stop = cancellation->get_token();
    auto* progress =
        new QProgressDialog(QStringLiteral("Collecting actual provider fixes for the captured diagnostic scope…"),
                            QStringLiteral("Cancel"), 0, 0, m_window);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    connect(progress, &QProgressDialog::canceled, this, [cancellation] { cancellation->request_stop(); });
    progress->show();
    const QPointer<Workbench> self(this);
    auto publish = [self, progress = QPointer<QProgressDialog>(progress), cancellation](ScopeResult result) {
        if (auto* app = QCoreApplication::instance())
            QMetaObject::invokeMethod(
                app,
                [self, progress, cancellation, result = std::move(result)]() mutable {
                    if (progress) {
                        QObject::disconnect(progress.data(), nullptr, self.data(), nullptr);
                        progress->close();
                    }
                    if (!self)
                        return;
                    if (!result) {
                        QMessageBox::warning(self->m_window, QStringLiteral("Fix in Scope refused"),
                                             QString::fromStdString(result.error().message()));
                        return;
                    }
                    auto capture = std::move(*result);
                    auto validate = [self, capture, cancellation]() -> Result<void> {
                        static_cast<void>(
                            cancellation); // Keep the caller's stop state alive through preview/admission.
                        if (!self || self->m_shutDown || capture->stop.stop_requested())
                            return fail(ErrorCode::Cancelled, "Fix scope cancelled");
                        const auto trust = self->m_services.trust.snapshot(capture->root);
                        const auto database = self->m_clangd ? self->m_clangd->compileCommands() : std::nullopt;
                        if (self->m_projectRoot != capture->root || self->m_clangd != capture->server ||
                            self->m_cppRenameGeneration != capture->generation ||
                            self->m_bridge->encoding() != capture->encoding || !trust.trusted ||
                            trust.revision != capture->trustRevision ||
                            (database ? database->file : std::string{}) != capture->database)
                            return fail(ErrorCode::Busy, "Project/configuration/trust changed during Fix in Scope");
                        const auto open = self->m_bridge->openDocuments();
                        if (open.size() != capture->open.size())
                            return fail(ErrorCode::Busy, "Open scope layout changed");
                        for (const auto& [path, snapshot] : open) {
                            const auto found = capture->open.find(lang::documentKey(path));
                            if (found == capture->open.end() || found->second.first.version() != snapshot.version() ||
                                found->second.second != capture->server->documentVersion(path))
                                return fail(ErrorCode::Busy, "A scope document version changed");
                        }
                        return self->m_bridge->validateRefactoringTargets(capture->fixes.edit);
                    };
                    if (auto current = validate(); !current) {
                        QMessageBox::warning(self->m_window, QStringLiteral("Fix in Scope refused"),
                                             QString::fromStdString(current.error().message()));
                        return;
                    }
                    if (capture->fixes.edit.empty()) {
                        QMessageBox::information(
                            self->m_window, QStringLiteral("Fix Published Diagnostics"),
                            QStringLiteral("No unique direct provider fixes were available; no files changed."));
                        return;
                    }
                    auto plan = std::move(capture->plan);
                    plan.revalidateContext = std::move(validate);
                    if (!self->previewCppRefactoring(plan))
                        return;
                    auto applied = self->applyRefactoring(plan);
                    if (!applied)
                        QMessageBox::warning(self->m_window, QStringLiteral("Fix in Scope refused"),
                                             QString::fromStdString(applied.error().message()));
                },
                Qt::QueuedConnection);
    };
    m_pool->spawn(sched::Lane::Background, planScope(&m_services.fs, capture, std::move(publish))).detach();
}
} // namespace frost::app
