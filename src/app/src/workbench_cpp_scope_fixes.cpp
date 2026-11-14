// SPDX-License-Identifier: Apache-2.0
// Module scope: actual clang-tidy exports, captured VFS inputs, one preview/REF-1 transaction.
#include "frost/app/workbench.hpp"
#include "frost/lang/inspection_scope_fixes.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "language_bridge.hpp"
#include "cmake_integration.hpp"
#include "frost/build/cmake_service.hpp"
#include "trusted_project_launcher.hpp"
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <set>
namespace frost::app {
namespace {
QString qs(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
struct Capture {
    std::string root, database, helper;
    std::uint64_t generation = 0, trustRevision = 0;
    lang::ClangdSearch search;
    lang::InspectionRequest request;
    lang::InspectionScopeCapture inputs;
    document::RefactoringPlan plan;
    lang::WorkspaceEdit targets;
    std::map<std::string, std::string> open;
    std::vector<std::pair<std::string, std::string>> overlays;
    std::vector<std::string> plugins;
    std::shared_ptr<lang::ClangdService> server;
    std::shared_ptr<sched::ResourceGovernor> governor;
    std::shared_ptr<QTemporaryDir> scratch;
    std::shared_ptr<std::stop_callback<std::function<void()>>> cancellation;
};
using Outcome = Result<std::shared_ptr<Capture>>;
sched::Task<void> prepare(io::IFileSystem* fs, io::IProcessLauncher* launcher, project::TrustStore* trust,
    std::shared_ptr<Capture> capture, std::function<void(Outcome)> done) {
    auto images = lang::captureInspectionScope(*fs, capture->request, capture->root, capture->overlays);
    if (!images) { done(std::unexpected(images.error())); co_return; }
    capture->inputs = std::move(*images);
    std::set<std::string> openPhysical;
    for (const auto& [path, text] : capture->open) {
        auto identity = fs->physicalPath(path);
        if (identity) openPhysical.insert(lang::documentKey(*identity));
        else if (identity.error().code() != ErrorCode::NotFound) { done(std::unexpected(identity.error())); co_return; }
    }
    for (const auto& image : capture->inputs.images) if (!image.buffer && openPhysical.contains(lang::documentKey(image.physicalPath))) {
        done(fail(ErrorCode::Unsupported, "A disk input aliases an uncaptured open buffer: " + image.path)); co_return;
    }
    auto tool = lang::findClangTidy(*fs, capture->search);
    if (!tool) { done(std::unexpected(tool.error())); co_return; }
    capture->request.clangTidyPath = tool->path;
    TrustedProjectLauncher admitted(*launcher, *trust, capture->root, capture->trustRevision, capture->request.stopToken);
    auto proposal = lang::runInspectionScopeFixes(admitted, *fs, capture->request, capture->inputs,
        capture->helper, capture->scratch->path().toStdString());
    if (!proposal) { done(std::unexpected(proposal.error())); co_return; }
    if (!proposal->ready()) {
        std::string reason = "The whole scope fix is unavailable; no partial edit will be applied.";
        for (const auto& group : proposal->groups) if (!group.refusal.empty()) reason += "\n" + group.rule + ": " + group.message + "\n" + group.refusal;
        if (proposal->groups.empty()) reason += "\nThe actual export contains no enabled automatic fix group.";
        done(fail(ErrorCode::Unsupported, std::move(reason))); co_return;
    }
    capture->plan.label = "Fix in Scope: " + std::to_string(proposal->groups.size()) + " exported groups; " +
        std::to_string(proposal->withoutFix) + " diagnostics have no exported fix";
    for (const auto& image : capture->inputs.images) {
        capture->plan.dependencies.push_back({image.path, image.diskBytes});
        if (image.buffer) capture->plan.bufferDependencies.push_back({image.path, image.contents});
    }
    for (const auto& path : capture->inputs.absentInputs) capture->plan.dependencies.push_back({path, std::nullopt});
    for (const auto& file : proposal->files) {
        if (file.buffer) capture->plan.buffers.push_back({file.path, file.before, file.after});
        else capture->plan.disk.push_back({file.path, file.before, file.after});
        lang::FileEdit target; target.path = file.path; capture->targets.files.push_back(std::move(target));
    }
    done(std::move(capture));
    co_return;
}
}
void Workbench::prepareCppInspectionScopeFixes(lang::InspectionScope scope, lang::InspectionProfile profile,
    std::stop_token cancel, std::function<void(Result<document::RefactoringPlan>)> done) {
    if (m_shutDown || !m_pool || !m_clangd || !m_bridge || m_projectRoot.empty()) { done(fail(ErrorCode::Busy, "Open a C++ project before requesting scope fixes")); return; }
    const auto permission = m_services.trust.snapshot(m_projectRoot);
    const auto database = m_clangd->compileCommands();
    if (!permission.trusted || !database) { done(fail(ErrorCode::Untrusted, "Scope fixes require a trusted project and actual compilation database")); return; }
    m_cppInspectionStop.request_stop(); m_cppInspectionStop = std::stop_source{};
    auto capture = std::make_shared<Capture>();
    capture->generation = ++m_cppInspectionGeneration; capture->trustRevision = permission.revision;
    m_cppInspectionTrustRevision = permission.revision;
    capture->root = m_projectRoot; capture->database = database->file; capture->server = m_clangd;
    capture->scratch = std::make_shared<QTemporaryDir>();
    if (!capture->scratch->isValid()) { done(fail(ErrorCode::IoError, "Could not allocate private inspection scratch")); return; }
    capture->request.compileCommandsDir = database->directory;
    capture->request.scope = std::move(scope); capture->request.profile = std::move(profile);
    capture->request.stopToken = m_cppInspectionStop.get_token();
    if (!m_resourceGovernor) m_resourceGovernor = std::make_shared<sched::ResourceGovernor>();
    capture->governor = m_resourceGovernor; capture->request.governor = capture->governor.get();
    const bool buildActive = (m_buildService && m_buildService->state() == build::BuildState::Running) ||
        (cmakeService() && cmakeService()->state() == build::BuildState::Running);
    capture->governor->setLegActive(sched::HeavyLegKind::Build, buildActive);
    auto settings = m_services.settings.snapshot();
    capture->search = lang::defaultClangdSearch(settings->get(settings::key::toolsClangTidyPath));
    capture->plugins = settings->get(settings::key::toolsClangTidyPlugins);
    for (const auto& plugin : capture->plugins) if (!plugin.empty()) capture->request.extraArgs.push_back("--load=" + plugin);
    capture->helper = QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef _WIN32
        QStringLiteral("frost-clang.exe")
#else
        QStringLiteral("frost-clang")
#endif
    ).toStdString();
    for (const auto& document : m_documents->documents()) if (!document->path().empty()) {
        capture->open.emplace(lang::documentKey(document->path()), document->document().text().text());
        capture->overlays.emplace_back(document->path(), document->document().text().text());
    }
    capture->cancellation = std::make_shared<std::stop_callback<std::function<void()>>>(cancel, [source = m_cppInspectionStop]() mutable { source.request_stop(); });
    const QPointer<Workbench> self(this);
    auto publish = [self, done = std::move(done)](Outcome value) mutable {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, done = std::move(done), value = std::move(value)]() mutable {
            if (!self) return;
            if (!value) { done(std::unexpected(value.error())); return; }
            auto capture = std::move(*value);
            auto admission = [self, capture]() -> Result<void> {
                if (!self || self->m_shutDown || capture->request.stopToken.stop_requested()) return fail(ErrorCode::Cancelled, "Scope fix request ended");
                const auto permission = self->m_services.trust.snapshot(capture->root);
                const auto database = self->m_clangd ? self->m_clangd->compileCommands() : std::nullopt;
                const auto settings = self->m_services.settings.snapshot();
                if (self->m_projectRoot != capture->root || self->m_clangd != capture->server ||
                    self->m_cppInspectionGeneration != capture->generation || !permission.trusted || permission.revision != capture->trustRevision ||
                    !database || database->file != capture->database || settings->get(settings::key::toolsClangTidyPath) != capture->search.configuredPath ||
                    settings->get(settings::key::toolsClangTidyPlugins) != capture->plugins)
                    return fail(ErrorCode::Busy, "Project, inspection configuration, trust or provider changed");
                std::map<std::string, std::string> open;
                std::set<std::string> physical;
                for (const auto& document : self->m_documents->documents()) if (!document->path().empty()) {
                    open.emplace(lang::documentKey(document->path()), document->document().text().text());
                    auto identity = self->m_services.fs.physicalPath(document->path());
                    if (identity) physical.insert(lang::documentKey(*identity));
                    else if (identity.error().code() != ErrorCode::NotFound) return std::unexpected(identity.error());
                }
                if (open != capture->open) return fail(ErrorCode::Busy, "Open source images/layout changed since inspection start");
                for (const auto& image : capture->inputs.images) if (!image.buffer && physical.contains(lang::documentKey(image.physicalPath)))
                    return fail(ErrorCode::Busy, "A closed inspection input now aliases an open document");
                if (auto targets = self->m_bridge->validateRefactoringTargets(capture->targets); !targets) return targets;
                return lang::validateInspectionCapture(self->m_services.fs, capture->inputs);
            };
            if (auto current = admission(); !current) { done(std::unexpected(current.error())); return; }
            auto plan = std::move(capture->plan); plan.revalidateContext = std::move(admission); done(std::move(plan));
        }, Qt::QueuedConnection);
    };
    m_pool->spawn(sched::Lane::Background, prepare(&m_services.fs, &m_services.launcher, &m_services.trust, capture, std::move(publish))).detach();
}
void Workbench::showCppInspectionScopeFixes() {
    bool accepted = false;
    const auto choice = QInputDialog::getItem(m_window, QStringLiteral("Fix in Scope"), QStringLiteral("Inspect using the project clang-tidy profile:"),
        {QStringLiteral("File"), QStringLiteral("Directory"), QStringLiteral("Target"), QStringLiteral("Project")}, 0, false, &accepted);
    if (!accepted) return;
    lang::InspectionScope scope;
    if (choice == QStringLiteral("File")) {
        const auto* view = m_actionHost.active(); if (!view || view->path().empty()) return;
        scope.kind = lang::InspectionScopeKind::File; scope.paths = {view->path()};
    } else if (choice == QStringLiteral("Directory")) {
        const auto path = QFileDialog::getExistingDirectory(m_window, QStringLiteral("Inspection directory"), qs(m_projectRoot)); if (path.isEmpty()) return;
        scope.kind = lang::InspectionScopeKind::Directory; scope.paths = {path.toStdString()};
    } else if (choice == QStringLiteral("Target")) {
        const auto target = QInputDialog::getText(m_window, QStringLiteral("Inspection target"), QStringLiteral("Exact target name from the active compilation database:"), QLineEdit::Normal, {}, &accepted);
        if (!accepted || target.isEmpty()) return;
        scope.kind = lang::InspectionScopeKind::Target; scope.targetName = target.toStdString();
    } else { scope.kind = lang::InspectionScopeKind::Project; scope.paths = {m_projectRoot}; }
    lang::InspectionProfile profile; profile.name = "Project";
    auto cancellation = std::make_shared<std::stop_source>();
    auto* progress = new QProgressDialog(QStringLiteral("Capturing module inputs and exporting actual clang-tidy fixes…"), QStringLiteral("Cancel"), 0, 0, m_window);
    progress->setAttribute(Qt::WA_DeleteOnClose); progress->show();
    connect(progress, &QProgressDialog::canceled, this, [cancellation] { cancellation->request_stop(); });
    const QPointer<Workbench> self(this);
    prepareCppInspectionScopeFixes(std::move(scope), std::move(profile), cancellation->get_token(),
        [self, cancellation, progress = QPointer<QProgressDialog>(progress)](Result<document::RefactoringPlan> result) mutable {
            if (progress) { QObject::disconnect(progress.data(), nullptr, self.data(), nullptr); progress->close(); }
            if (!self || cancellation->stop_requested()) return;
            if (!result) { QMessageBox::warning(self->m_window, QStringLiteral("Scope fix unavailable"), qs(result.error().message())); return; }
            auto plan = std::make_shared<document::RefactoringPlan>(std::move(*result));
            auto* dialog = new QDialog(self->m_window); dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowTitle(QStringLiteral("Fix in Scope — actual exported replacements"));
            auto* layout = new QVBoxLayout(dialog);
            auto* summary = new QLabel(qs(plan->label) + QStringLiteral("\nSelected translation units use the captured active database configurations. Source/header fixes apply together. Preview is capped at 1 MiB per image. Build/output/link targets and alternative/conflicting groups refuse the whole operation."), dialog);
            summary->setWordWrap(true); layout->addWidget(summary);
            auto* tabs = new QTabWidget(dialog);
            auto image = [&](const std::string& path, const std::string& before, const std::string& after) {
                auto* text = new QPlainTextEdit(dialog); text->setReadOnly(true);
                text->setPlainText(QStringLiteral("BEFORE\n") + qs(std::string_view(before).substr(0, 1024 * 1024)) + QStringLiteral("\nAFTER\n") + qs(std::string_view(after).substr(0, 1024 * 1024)));
                tabs->addTab(text, qs(path));
            };
            for (const auto& file : plan->disk) image(file.path, file.expectedBytes, file.afterBytes);
            for (const auto& file : plan->buffers) image(file.path, file.expectedText, file.afterText);
            layout->addWidget(tabs);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, dialog); layout->addWidget(buttons);
            connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, dialog, [self, dialog, plan] {
                if (!self) return;
                auto applied = self->applyRefactoring(*plan);
                if (!applied) QMessageBox::warning(dialog, QStringLiteral("Scope fix refused"), qs(applied.error().message()));
                else dialog->accept(); // PendingRecovery is already committed; never resubmit this preview.
            });
            connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
            dialog->resize(900, 650); dialog->show();
        });
}
}
