// SPDX-License-Identifier: Apache-2.0
// Workbench integration for trusted C# projects and managed document diagnostics.
#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/lane.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "language_bridge.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>

#include <algorithm>
#include <limits>

namespace frost::app {
namespace {
sched::Task<void> stopOwnedManaged(std::shared_ptr<lang::ManagedService> server) { co_await server->stop(); }
} // namespace
bool isManagedSource(std::string_view path) noexcept {
    if (path.size() < 4)
        return false;
    auto ext = path.substr(path.size() - 3);
    return ext[0] == '.' && (ext[1] == 'c' || ext[1] == 'C') && (ext[2] == 's' || ext[2] == 'S');
}
void Workbench::offerManagedTrust() {
    if (m_projectRoot.empty() || m_services.trust.isTrusted(m_projectRoot) || !m_window || m_managedTrustDialog)
        return;
    auto* dialog = new ui::TrustDialog(
        *m_build, {QString::fromUtf8(m_projectRoot.data(), static_cast<qsizetype>(m_projectRoot.size()))}, m_window,
        ui::TrustPurpose::ManagedLanguageServer);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_managedTrustDialog = dialog;
    const std::string root = m_projectRoot;
    connect(dialog, &QDialog::accepted, this, [this, root] {
        m_managedTrustDialog.clear();
        if (m_projectRoot == root && m_services.trust.isTrusted(root))
            startManaged(root);
    });
    dialog->open();
}
void Workbench::startManaged(const std::string& root) {
    if (m_shutDown)
        return;
    stopManaged();
    const auto snapshot = m_services.settings.snapshot();
    m_managedDotnetPath = snapshot->get(settings::key::toolsManagedDotnetPath);
    m_managedServerPath = snapshot->get(settings::key::toolsManagedServerPath);
    if (!m_services.trust.isTrusted(root)) {
        for (auto* view : m_editors->views()) {
            if (isManagedSource(view->path())) {
                offerManagedTrust();
                break;
            }
        }
        return;
    }
    std::string dotnetPath = m_managedDotnetPath;
    std::string serverPath = m_managedServerPath;
    if (dotnetPath.empty()) {
        const QString dotnetRoot = qEnvironmentVariable("DOTNET_ROOT");
#ifdef _WIN32
        const QString executable = QStringLiteral("dotnet.exe");
#else
        const QString executable = QStringLiteral("dotnet");
#endif
        if (!dotnetRoot.isEmpty())
            dotnetPath = QDir(dotnetRoot).filePath(executable).toUtf8().toStdString();
    }
    if (serverPath.empty()) {
        QString base = qEnvironmentVariable("FROST_SDK");
        if (base.isEmpty())
            base = QCoreApplication::applicationDirPath();
        serverPath = QDir(base).filePath(QStringLiteral("managed/CSharpLanguageServer.dll")).toUtf8().toStdString();
    }
    if (!io::isAbsolutePath(dotnetPath) || !io::isAbsolutePath(serverPath)) {
        return;
    }
    const auto dotnet = m_services.fs.status(dotnetPath);
    const auto assembly = m_services.fs.status(serverPath);
    if (!dotnet || !assembly || dotnet->type != io::FileType::Regular || assembly->type != io::FileType::Regular) {
        if (!m_managedDotnetPath.empty() || !m_managedServerPath.empty())
            notify(QStringLiteral("C# language server"),
                   QStringLiteral("The configured .NET SDK or server was not found."), ui::NotificationKind::Warning);
        return;
    }
    if (!m_reactor) {
        auto reactor = io::Reactor::create(io::ReactorOptions{1, "frost-managed-io"});
        if (!reactor) {
            log::warn("workbench", "C# language server reactor: {}", reactor.error().describe());
            return;
        }
        m_reactor = std::move(*reactor);
    }
    const QString cliHome = qEnvironmentVariable("FROST_DOTNET_CLI_HOME");
    if (!cliHome.isEmpty() && (!QDir::isAbsolutePath(cliHome) || !QDir().mkpath(cliHome))) {
        notify(QStringLiteral("C# language server"), QStringLiteral("Could not create the configured .NET CLI home."),
               ui::NotificationKind::Warning);
        return;
    }
    auto options = lang::managedServiceOptions(dotnetPath, serverPath, root, true);
    if (!cliHome.isEmpty())
        options.environment.emplace_back("DOTNET_CLI_HOME", cliHome.toUtf8().toStdString());
    m_managed = std::make_shared<lang::ManagedService>(m_services.launcher, io::AsyncContext{*m_reactor, *m_pool},
                                                       std::move(options));
    m_managedListener = m_managed->diagnostics().addListener([this](const lang::FileDiagnostics& data) {
        QMetaObject::invokeMethod(this, [this, file = data.file] { onManagedDiagnostics(file); }, Qt::QueuedConnection);
    });
    if (auto started = m_managed->start(); !started) {
        log::warn("workbench", "C# language server startup: {}", started.error().describe());
        stopManaged();
        return;
    }
    m_bridge->setManagedServer(m_managed);
    for (auto* view : m_editors->views())
        syncManagedDocument(*view);
    updateLanguageTarget();
}
void Workbench::stopManaged() {
    if (m_managedTrustDialog) {
        m_managedTrustDialog->close();
        m_managedTrustDialog.clear();
    }
    if (!m_managed)
        return;
    m_language->editorChanged();
    m_bridge->setManagedServer(nullptr);
    m_managed->diagnostics().removeListener(m_managedListener);
    m_managedListener = 0;
    for (auto* view : m_editors->views()) {
        if (!isManagedSource(view->path()))
            continue;
        view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, {});
        view->editor().setInsightEnabled(false);
        m_liveMarkers.erase(lang::documentKey(view->path()));
        refreshMarkers(*view);
    }
    auto old = std::move(m_managed);
    m_retiredManaged.push_back(m_pool->spawn(sched::Lane::Background, stopOwnedManaged(std::move(old))));
    refreshLiveProblems();
}
void Workbench::syncManagedDocument(DocumentEditorView& view) {
    if (!m_managed || !isManagedSource(view.path()))
        return;
    const auto& document = view.document().document();
    if (m_managed->openDocument(view.path(), document.text())) {
        view.editor().setInsightEnabled(true);
        return;
    }
    text::ChangeSet whole;
    whole.baseVersion = std::numeric_limits<std::uint64_t>::max();
    whole.version = document.version();
    (void)m_managed->changeDocument(view.path(), whole, document.text());
}
void Workbench::onManagedDiagnostics(const std::string& file) {
    if (!m_managed || !isManagedSource(file))
        return;
    auto set = m_managed->diagnostics().forFile(file);
    if (set && set->stale)
        return;
    std::vector<ui::LineMarker> markers;
    if (set)
        for (const auto& d : set->diagnostics) {
            if (d.line <= 0 ||
                (d.severity != lang::DiagnosticSeverity::Error && d.severity != lang::DiagnosticSeverity::Warning))
                continue;
            markers.push_back({LineIndex{static_cast<std::uint32_t>(d.line - 1)},
                               d.severity == lang::DiagnosticSeverity::Error ? ui::MarkerSeverity::Error
                                                                             : ui::MarkerSeverity::Warning,
                               d.message});
        }
    const auto key = lang::documentKey(file);
    if (markers.empty())
        m_liveMarkers.erase(key);
    else
        m_liveMarkers[key] = std::move(markers);
    if (auto* view = m_editors->find(file)) {
        view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live,
                                      set ? ui::lang::toEditorDiagnostics(*set) : std::vector<ui::EditorDiagnostic>{});
        refreshMarkers(*view);
    }
    refreshLiveProblems();
}
} // namespace frost::app
