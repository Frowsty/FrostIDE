// SPDX-License-Identifier: Apache-2.0
// Explicit trusted .NET console debug entry point and verified source presentation.
#include "frost/app/workbench.hpp"
#include "frost/core/mutex.hpp"
#include "frost/debug/coreclr_dap.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"

#include "cmake_integration.hpp"
#include "engine_integration.hpp"
#include "managed_debug_preparation.hpp"
#include "managed_source_snapshot.hpp"
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>

namespace frost::app {
struct ManagedSnapshotJob {
    Mutex mutex;
    Workbench* target FROST_GUARDED_BY(mutex) = nullptr;
    std::atomic_bool cancelled{false};
};
namespace {
using SnapshotCompletion = std::function<void(Result<ManagedSourceSnapshot>)>;
sched::Task<void> captureSnapshot(std::shared_ptr<ManagedSnapshotJob> job, io::IFileSystem* fs,
                                  project::TrustStore* trust, std::string root, PreparedManagedDebug prepared,
                                  SnapshotCompletion done) {
    auto result = captureManagedSources(*fs, *trust, root, prepared, [job] { return job->cancelled.load(); });
    LockGuard lock(job->mutex);
    if (job->target && !job->cancelled.load()) {
        QMetaObject::invokeMethod(
            job->target, [done = std::move(done), result = std::move(result)]() mutable { done(std::move(result)); },
            Qt::QueuedConnection);
    }
    co_return;
}
bool sameCanonical(io::IFileSystem& fs, std::string_view path) {
    auto canonical = fs.canonicalPath(path);
    auto normalized = io::normalizePath(path);
    return canonical && normalized && managedPathHasNoLinks(fs, path) && *canonical == *normalized;
}
bool below(std::string_view root, std::string_view path) {
    const auto r = io::normalizePath(root).value_or("");
    const auto p = io::normalizePath(path).value_or("");
    return !r.empty() && p.size() > r.size() && p.starts_with(r) &&
           (r.back() == '/' || r.back() == '\\' || p[r.size()] == '/' || p[r.size()] == '\\');
}
sched::Task<void> validateBreakpoint(std::shared_ptr<ManagedSnapshotJob> job, io::IFileSystem* fs,
                                     project::TrustStore* trust, std::string root, std::string path,
                                     std::shared_ptr<const ManagedSourceSnapshot> snapshot,
                                     std::function<void(bool)> done) {
    bool valid = false;
    const auto found = snapshot->sources.find(path);
    if (!job->cancelled.load() && found != snapshot->sources.end() && trust->isTrusted(root) &&
        sameCanonical(*fs, path) && trust->isTrusted(io::parentPath(path))) {
        auto status = fs->status(path);
        if (status && status->type == io::FileType::Regular && status->size == found->second.size() &&
            !job->cancelled.load()) {
            auto bytes = fs->readFile(path);
            valid = bytes && *bytes == found->second;
        }
    }
    LockGuard lock(job->mutex);
    if (job->target && !job->cancelled.load()) {
        QMetaObject::invokeMethod(
            job->target, [done = std::move(done), valid]() mutable { done(valid); }, Qt::QueuedConnection);
    }
    co_return;
}
} // namespace

void Workbench::invalidateManagedDebug() {
    for (const auto& job : m_managedBreakpointJobs) {
        job->cancelled.store(true);
        LockGuard lock(job->mutex);
        job->target = nullptr;
    }
    m_managedBreakpointJobs.clear();
    if (m_managedSnapshotJob) {
        m_managedSnapshotJob->cancelled.store(true);
        {
            LockGuard lock(m_managedSnapshotJob->mutex);
            m_managedSnapshotJob->target = nullptr;
        }
        m_managedSnapshotJob.reset();
    }
    if (m_managedDebugPreparation && m_managedDebugPreparation->active())
        m_managedDebugPreparation->cancel();
    if (m_managedDebugSnapshot) {
        m_managedDebugSnapshot.reset();
        if (m_debug && m_debug->debugging())
            m_debug->stop();
    }
    m_managedDebugRoot.clear();
}

bool Workbench::managedDebugFrameAllowed(const debug::DebugFrame& frame) const {
    if (!m_managedDebugSnapshot || m_projectRoot != m_managedDebugRoot ||
        !m_services.trust.isTrusted(m_managedDebugRoot) || !frame.hasSource())
        return false;
    return QString::fromStdString(frame.module)
                   .compare(QString::fromStdString(m_managedDebugSnapshot->moduleId), Qt::CaseInsensitive) == 0 &&
           m_managedDebugSnapshot->sources.contains(frame.path);
}

void Workbench::validateManagedBreakpoint(std::string path, std::function<void(bool)> done) {
    if (!m_managedDebugSnapshot || m_projectRoot != m_managedDebugRoot ||
        !m_services.trust.isTrusted(m_managedDebugRoot) || !m_managedDebugSnapshot->sources.contains(path)) {
        done(false);
        return;
    }
    auto job = std::make_shared<ManagedSnapshotJob>();
    {
        LockGuard lock(job->mutex);
        job->target = this;
    }
    m_managedBreakpointJobs.push_back(job);
    const auto snapshot = m_managedDebugSnapshot;
    const auto root = m_managedDebugRoot;
    auto finish = [this, job, snapshot, root, path, done = std::move(done)](bool valid) mutable {
        std::erase(m_managedBreakpointJobs, job);
        auto* view = m_editors->find(path);
        done(valid && !job->cancelled.load() && m_managedDebugSnapshot == snapshot && m_projectRoot == root &&
             m_services.trust.isTrusted(root) && (!view || !view->document().dirty()));
    };
    std::erase_if(m_managedSnapshotTasks, [](const auto& task) { return task.valid() && task.ready(); });
    m_managedSnapshotTasks.push_back(
        m_pool->spawn(sched::Lane::Background, validateBreakpoint(job, &m_services.fs, &m_services.trust, root,
                                                                  std::move(path), snapshot, std::move(finish))));
}

void Workbench::showManagedDebugSource(const std::string& path, int line) {
    if (!m_managedDebugSnapshot)
        return;
    const auto found = m_managedDebugSnapshot->sources.find(path);
    if (found == m_managedDebugSnapshot->sources.end())
        return;
    const QByteArray bytes(found->second.data(), static_cast<qsizetype>(found->second.size()));
    const QString source = QString::fromUtf8(bytes);
    if (source.toUtf8() != bytes) {
        notify(QStringLiteral("Debug .NET"), QStringLiteral("The verified source is not valid UTF-8."),
               ui::NotificationKind::Error);
        return;
    }
    if (!m_managedSourceDialog) {
        auto* dialog = new QDialog(m_window);
        dialog->setObjectName(QStringLiteral("managedVerifiedSource"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        auto* layout = new QVBoxLayout(dialog);
        auto* view = new QPlainTextEdit(dialog);
        view->setReadOnly(true);
        layout->addWidget(view);
        ui::installDialogChrome(dialog, true);
        dialog->resize(850, 600);
        m_managedSourceDialog = dialog;
        m_managedSourceView = view;
    }
    m_managedSourceDialog->setWindowTitle(
        QStringLiteral("Verified debug source: %1").arg(QString::fromStdString(path)));
    m_managedSourceView->setPlainText(source);
    auto cursor = m_managedSourceView->textCursor();
    const auto block = m_managedSourceView->document()->findBlockByNumber(qMax(0, line - 1));
    if (block.isValid())
        cursor.setPosition(block.position());
    m_managedSourceView->setTextCursor(cursor);
    m_managedSourceView->centerCursor();
    m_managedSourceDialog->show();
}

void Workbench::startManagedDebug() {
    if (m_projectRoot.empty() || m_buildService->projectEditInProgress() || !m_managedDebugPreparation ||
        m_managedDebugPreparation->active() || m_managedSnapshotJob || !m_debug || m_debug->debugging() ||
        m_runController->running() || !m_runService->activeRuns().empty())
        return;
    const std::string root = m_projectRoot;
    if (!sameCanonical(m_services.fs, root) || !m_services.trust.isTrusted(root)) {
        notify(QStringLiteral("Debug .NET"),
               QStringLiteral("Open and trust a project folder without filesystem aliases."),
               ui::NotificationKind::Error);
        return;
    }
    std::optional<ManagedDebugSelection> selected;
    if (m_options.selectManagedDebug) {
        selected = m_options.selectManagedDebug(root);
        if (!selected)
            return;
    }
    const QString project =
        selected ? QString::fromStdString(selected->project)
                 : QFileDialog::getOpenFileName(m_window, QStringLiteral("Select .NET project"),
                                                QString::fromStdString(root), QStringLiteral("C# project (*.csproj)"));
    if (project.isEmpty())
        return;
    const std::string projectPath = project.toStdString();
    if (!below(root, projectPath) || !sameCanonical(m_services.fs, projectPath) ||
        !m_services.trust.isTrusted(io::parentPath(projectPath))) {
        notify(QStringLiteral("Debug .NET"),
               QStringLiteral("The selected project must be inside the trusted folder without aliases."),
               ui::NotificationKind::Error);
        return;
    }
    QDialog dialog(m_window);
    dialog.setWindowTitle(QStringLiteral("Debug .NET console project"));
    auto* layout = new QFormLayout(&dialog);
    QString defaultDotnet =
        QString::fromStdString(m_services.settings.snapshot()->get(settings::key::toolsManagedDotnetPath));
    if (defaultDotnet.isEmpty()) {
#ifdef _WIN32
        const QString exeName = QStringLiteral("dotnet.exe");
#else
        const QString exeName = QStringLiteral("dotnet");
#endif
        const QString dotnetRoot = qEnvironmentVariable("DOTNET_ROOT");
        if (!dotnetRoot.isEmpty() && QFileInfo::exists(QDir(dotnetRoot).filePath(exeName))) {
            defaultDotnet = QDir(dotnetRoot).filePath(exeName);
        } else if (QFileInfo::exists(
                       QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("dotnet/") + exeName))) {
            defaultDotnet = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("dotnet/") + exeName);
        } else {
#ifdef _WIN32
            const QString pf = qEnvironmentVariable("ProgramFiles", QStringLiteral("C:/Program Files"));
            if (QFileInfo::exists(QDir(pf).filePath(QStringLiteral("dotnet/dotnet.exe"))))
                defaultDotnet = QDir(pf).filePath(QStringLiteral("dotnet/dotnet.exe"));
#endif
            if (defaultDotnet.isEmpty())
                defaultDotnet = QStandardPaths::findExecutable(QStringLiteral("dotnet"));
        }
    }
    QLineEdit dotnet(defaultDotnet, &dialog);
    QLineEdit configuration(QStringLiteral("Debug"), &dialog);
    QLineEdit framework(QStringLiteral("net10.0"), &dialog);
    QLineEdit rid(&dialog);
    const QString bundle = QCoreApplication::applicationDirPath() + QStringLiteral("/FrostManagedDebug");
#ifdef _WIN32
    QLineEdit adapter(bundle + QStringLiteral("/adapter/netcoredbg.exe"), &dialog);
#else
    QLineEdit adapter(bundle + QStringLiteral("/adapter/netcoredbg"), &dialog);
#endif
    if (selected) {
        dotnet.setText(QString::fromStdString(selected->dotnet));
        configuration.setText(QString::fromStdString(selected->configuration));
        framework.setText(QString::fromStdString(selected->framework));
        rid.setText(QString::fromStdString(selected->runtimeIdentifier));
        if (!selected->adapter.empty())
            adapter.setText(QString::fromStdString(selected->adapter));
    }
    layout->addRow(QStringLiteral(".NET host"), &dotnet);
    layout->addRow(QStringLiteral("Configuration"), &configuration);
    layout->addRow(QStringLiteral("Target framework"), &framework);
    layout->addRow(QStringLiteral("Runtime identifier (optional)"), &rid);
    layout->addRow(QStringLiteral("CoreCLR debugger"), &adapter);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(&buttons);
    ui::installDialogChrome(&dialog, false);
    if (!selected && dialog.exec() != QDialog::Accepted)
        return;
    // The project picker and options dialog run nested event loops: any competing operation,
    // project switch or trust revocation may have happened while either was open.
    if (m_buildService->externalLaunchAdmissionBusy() || m_projectRoot != root || !canStartLuaDebug() ||
        !m_services.trust.isTrusted(root) || !sameCanonical(m_services.fs, root)) {
        notify(QStringLiteral("Debug .NET"), QStringLiteral("The project or debug state changed; try again."),
               ui::NotificationKind::Warning);
        return;
    }
    if (framework.text() != QStringLiteral("net10.0")) {
        notify(QStringLiteral("Debug .NET"),
               QStringLiteral("This debugger currently supports net10.0 console projects."),
               ui::NotificationKind::Error);
        return;
    }
    const std::string guard = m_options.managedPdbGuardPath.empty()
                                  ? (bundle + QStringLiteral("/PdbGuard/PdbGuard.dll")).toStdString()
                                  : m_options.managedPdbGuardPath;
    const std::string adapterPath = adapter.text().toStdString();
    if (!io::isAbsolutePath(guard) || !QFileInfo::exists(QString::fromStdString(guard)) ||
        !io::isAbsolutePath(adapterPath) || !QFileInfo::exists(adapter.text())) {
        notify(QStringLiteral("Debug .NET"),
               QStringLiteral("The debugger or the installed source verifier is unavailable."),
               ui::NotificationKind::Error);
        return;
    }
    const auto saved = saveAllDocuments();
    if (!saved.failed.empty()) {
        notify(QStringLiteral("Debug .NET"), QString::fromStdString(saved.failed.front().error.message()),
               ui::NotificationKind::Error);
        return;
    }
    build::ManagedProjectRequest request{.dotnet = dotnet.text().toStdString(),
                                         .project = projectPath,
                                         .configuration = configuration.text().toStdString(),
                                         .targetFramework = framework.text().toStdString(),
                                         .runtimeIdentifier = rid.text().toStdString()};
    m_managedDebugRoot = root;
    auto current = [this, root, projectPath] {
        return !m_shutDown && !m_buildService->externalLaunchAdmissionBusy() &&
               (!m_cmake || (!m_cmake->running() && m_cmake->service().state() == build::BuildState::Idle)) &&
               m_debug && !m_debug->debugging() && !m_debug->rerunPending() && m_runController &&
               !m_runController->running() && m_runService && m_runService->activeRuns().empty() &&
               m_projectRoot == root && m_services.trust.isTrusted(root) && sameCanonical(m_services.fs, root) &&
               sameCanonical(m_services.fs, projectPath) && m_services.trust.isTrusted(io::parentPath(projectPath));
    };
    auto started = m_managedDebugPreparation->start(
        request, {root}, guard, current,
        [this, root, adapterPath, current](Result<PreparedManagedDebug> result) {
            if (m_window)
                m_window->refreshActions();
            if (!result || !current()) {
                if (!result && result.error().code() != ErrorCode::Cancelled)
                    notify(QStringLiteral("Debug .NET"), QString::fromStdString(result.error().message()),
                           ui::NotificationKind::Error);
                return;
            }
            auto job = std::make_shared<ManagedSnapshotJob>();
            {
                LockGuard lock(job->mutex);
                job->target = this;
            }
            m_managedSnapshotJob = job;
            const auto target = result->target;
            auto done = [this, root, adapterPath, current, target, job](Result<ManagedSourceSnapshot> snapshot) {
                if (m_managedSnapshotJob != job || job->cancelled.load())
                    return;
                m_managedSnapshotJob.reset();
                if (!snapshot || !current()) {
                    notify(QStringLiteral("Debug .NET"),
                           QStringLiteral("The verified output or source changed before launch."),
                           ui::NotificationKind::Error);
                    return;
                }
                m_managedDebugSnapshot = std::make_shared<ManagedSourceSnapshot>(std::move(*snapshot));
                std::vector<std::string> sources;
                for (const auto& [path, bytes] : m_managedDebugSnapshot->sources) {
                    (void)bytes;
                    sources.push_back(path);
                }
                auto factory = debug::coreClrAdapterFactory(m_services.launcher, adapterPath);
                auto launched = m_debug->startCoreClrTarget(
                    target, {root}, std::move(factory),
                    [this](const debug::DebugFrame& frame) { return managedDebugFrameAllowed(frame); },
                    std::move(sources),
                    [this](std::string path, std::function<void(bool)> done) {
                        validateManagedBreakpoint(std::move(path), std::move(done));
                    });
                if (!launched) {
                    m_managedDebugSnapshot.reset();
                    notify(QStringLiteral("Debug .NET"), QString::fromStdString(launched.error().message()),
                           ui::NotificationKind::Error);
                }
                if (m_window)
                    m_window->refreshActions();
            };
            std::erase_if(m_managedSnapshotTasks, [](const auto& task) { return task.valid() && task.ready(); });
            m_managedSnapshotTasks.push_back(
                m_pool->spawn(sched::Lane::Background, captureSnapshot(job, &m_services.fs, &m_services.trust, root,
                                                                       std::move(*result), std::move(done))));
            if (m_window)
                m_window->refreshActions();
        },
        [this](build::OutputStream stream, std::string_view line) {
            if (ui::RunConsole* console = runConsole())
                console->appendOutput(stream == build::OutputStream::Stdout ? build::RunOutputKind::Stdout
                                                                            : build::RunOutputKind::Stderr,
                                      std::string(line) + "\n");
        });
    if (!started)
        notify(QStringLiteral("Debug .NET"), QString::fromStdString(started.error().message()),
               ui::NotificationKind::Error);
    if (m_window)
        m_window->refreshActions();
}

} // namespace frost::app
