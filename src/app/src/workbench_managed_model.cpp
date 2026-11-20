// SPDX-License-Identifier: Apache-2.0
// GUI-owned selected managed context; actual SDK discovery/evaluation runs on background workers.
#include "frost/app/workbench.hpp"
#include "frost/build/managed_evaluation.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include "managed_build_backend.hpp"
#include "managed_trust_watch.hpp"
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <variant>
namespace frost::app {
struct ManagedModelJob {
    Mutex mutex;
    Workbench* target FROST_GUARDED_BY(mutex) = nullptr;
    std::stop_source stop;
};
namespace {
using ModelResult = std::variant<build::ManagedProjectProperties, build::ManagedProjectEvaluation>;
sched::Task<void> evaluateManaged(std::shared_ptr<ManagedModelJob> job, io::IFileSystem* fs,
                                  io::IProcessLauncher* launcher, project::TrustStore* trust, std::string root,
                                  std::uint64_t revision, build::ManagedProjectRequest request,
                                  std::function<void(Result<ModelResult>)> completed) {
    auto trustWatch = watchManagedTrust(*trust, root, revision, job->stop);
    const auto current = [&] {
        auto snapshot = trust->snapshot(root);
        auto prefix = lang::documentKey(root);
        if (!prefix.ends_with('/') && !prefix.ends_with(char{92}))
            prefix += io::kNativePathStyle == io::PathStyle::Windows ? char{92} : '/';
        auto physicalRoot = fs->physicalPath(root), physicalProject = fs->physicalPath(request.project);
        return !job->stop.stop_requested() && snapshot.trusted && snapshot.revision == revision && physicalRoot &&
               physicalProject && lang::documentKey(*physicalRoot) == lang::documentKey(root) &&
               lang::documentKey(*physicalProject) == lang::documentKey(request.project) &&
               trust->isTrusted(request.project) && lang::documentKey(request.project).starts_with(prefix);
    };
    auto result = [&]() -> Result<ModelResult> {
        if (!current())
            return fail(ErrorCode::Untrusted, "Managed context no longer belongs to the trusted physical project");
        auto spec = request.targetFramework.empty() ? build::managedProjectQuerySpec(request)
                                                    : build::managedProjectEvaluationSpec(request);
        if (!spec)
            return std::unexpected(spec.error());
        auto output = io::runToCompletion(*launcher, *spec, {}, std::chrono::minutes(2), job->stop.get_token());
        if (!output)
            return std::unexpected(output.error());
        if (!current())
            return fail(ErrorCode::Cancelled, "Managed context changed during SDK evaluation");
        if (output->exitCode != 0)
            return fail(ErrorCode::IoError, "dotnet evaluation failed: " + output->stderrBytes + output->stdoutBytes);
        if (request.targetFramework.empty()) {
            auto parsed = build::parseManagedProjectProperties(output->stdoutBytes);
            if (!parsed)
                return std::unexpected(parsed.error());
            return ModelResult{std::move(*parsed)};
        }
        auto parsed = build::parseManagedProjectEvaluation(request, output->stdoutBytes);
        if (!parsed)
            return std::unexpected(parsed.error());
        auto launchPath = io::joinPath(io::parentPath(request.project), "Properties/launchSettings.json");
        if (!launchPath)
            return std::unexpected(launchPath.error());
        auto launchExists = fs->exists(*launchPath);
        if (!launchExists)
            return std::unexpected(launchExists.error());
        std::optional<std::string> launchBytes;
        std::vector<build::ManagedLaunchProfile> launchProfiles;
        if (*launchExists) {
            auto bytes = fs->readFileSnapshot(*launchPath, 1024 * 1024);
            if (!bytes)
                return std::unexpected(bytes.error());
            auto profiles = build::parseManagedLaunchSettings(*bytes);
            if (!profiles)
                return std::unexpected(profiles.error());
            launchBytes = std::move(*bytes);
            launchProfiles = std::move(*profiles);
        }
        std::set<std::string> inputs{request.project};
        auto imported = parsed->properties.find("MSBuildAllProjects");
        if (imported != parsed->properties.end())
            for (const auto& path : QString::fromStdString(imported->second).split(';', Qt::SkipEmptyParts))
                inputs.insert(path.toStdString());
        if (inputs.size() > 256)
            return fail(ErrorCode::OutOfRange, "Managed context import receipt budget exceeds 256 files");
        std::map<std::string, std::string, std::less<>> receipts;
        std::size_t bytes = 0;
        for (const auto& path : inputs) {
            if (!io::isAbsolutePath(path))
                return fail(ErrorCode::Corrupt, "SDK reported a relative import");
            auto content = fs->readFileSnapshot(path, 2U * 1024U * 1024U);
            if (!content)
                return std::unexpected(content.error());
            bytes += content->size();
            if (bytes > 32U * 1024U * 1024U)
                return fail(ErrorCode::OutOfRange, "Managed import receipts exceed 32 MiB");
            receipts.emplace(path, std::move(*content));
        }
        auto finalOutput = io::runToCompletion(*launcher, *spec, {}, std::chrono::minutes(2), job->stop.get_token());
        if (!finalOutput)
            return std::unexpected(finalOutput.error());
        if (finalOutput->exitCode != 0)
            return fail(ErrorCode::IoError, "Final managed evaluation failed");
        auto finalModel = build::parseManagedProjectEvaluation(request, finalOutput->stdoutBytes);
        if (!finalModel)
            return std::unexpected(finalModel.error());
        for (const auto& [path, content] : receipts) {
            auto actual = fs->readFileSnapshot(path, 2U * 1024U * 1024U);
            if (!actual || *actual != content)
                return fail(ErrorCode::Busy, "Managed import changed during evaluation");
        }
        auto finalImports = finalModel->properties.find("MSBuildAllProjects");
        if (finalImports != finalModel->properties.end())
            for (const auto& path : QString::fromStdString(finalImports->second).split(';', Qt::SkipEmptyParts))
                if (!receipts.contains(path.toStdString()))
                    return fail(ErrorCode::Busy, "Managed import set changed during evaluation; retry");
        if (!current())
            return fail(ErrorCode::Cancelled, "Managed context changed before publication");
        auto finalLaunchExists = fs->exists(*launchPath);
        if (!finalLaunchExists || *finalLaunchExists != *launchExists)
            return fail(ErrorCode::Busy, "launchSettings presence changed during evaluation");
        if (launchBytes) {
            auto bytes = fs->readFileSnapshot(*launchPath, 1024 * 1024);
            if (!bytes || *bytes != *launchBytes)
                return fail(ErrorCode::Busy, "launchSettings changed during evaluation");
            receipts.emplace(*launchPath, *launchBytes);
        }
        finalModel->launchSettingsPath = *launchPath;
        finalModel->launchSettingsPresent = *launchExists;
        finalModel->launchProfiles = std::move(launchProfiles);
        finalModel->inputFiles = std::move(receipts);
        return ModelResult{std::move(*finalModel)};
    }();
    LockGuard lock(job->mutex);
    if (job->target && !job->stop.stop_requested())
        QMetaObject::invokeMethod(
            job->target,
            [completed = std::move(completed), result = std::move(result)]() mutable { completed(std::move(result)); },
            Qt::QueuedConnection);
    co_return;
}
} // namespace
void Workbench::clearManagedProjectContext() {
    if (auto job = std::exchange(m_managedModelJob, {})) {
        job->stop.request_stop();
        LockGuard lock(job->mutex);
        job->target = nullptr;
    }
    if (m_managedEvaluation)
        stopManaged();
    m_managedContextLease->store(false);
    m_managedContextLease = std::make_shared<std::atomic<bool>>(true);
    ++m_managedModelGeneration;
    m_managedEvaluation.reset();
    m_managedContextRoot.clear();
    m_managedDesiredContext.reset();
    if (auto retiring = std::move(m_managedBuildBackend)) {
        m_build->setBackend(m_buildBackend.get());
    }
    emit managedProjectChanged();
}
Result<void> Workbench::configureManagedProject(build::ManagedProjectRequest request) {
    if (m_build && m_build->isBuilding())
        return fail(ErrorCode::Busy, "Stop the current build before changing managed context");
    if (m_shutDown || m_projectRoot.empty() || !m_services.trust.isTrusted(m_projectRoot))
        return fail(ErrorCode::Untrusted, "Open and trust the managed workspace before evaluating a context");
    if (!io::isAbsolutePath(request.project) || !request.project.ends_with(".csproj") ||
        !m_services.trust.isTrusted(request.project))
        return fail(ErrorCode::InvalidArgument, "Choose an absolute trusted SDK C# project");
    for (auto* view : m_editors->uniqueDocumentViews()) {
        auto key = lang::documentKey(view->path());
        if (view->document().dirty() && (key.ends_with(".csproj") || key.ends_with(".props") ||
                                         key.ends_with(".targets") || io::fileName(key) == "global.json"))
            return fail(ErrorCode::Busy, "Save the modified managed project/import before evaluating its disk context");
    }
    auto spec = request.targetFramework.empty() ? build::managedProjectQuerySpec(request)
                                                : build::managedProjectEvaluationSpec(request);
    if (!spec)
        return std::unexpected(spec.error());
    stopManaged();
    clearManagedProjectContext();
    m_managedDesiredContext = request;
    const auto generation = m_managedModelGeneration;
    const auto root = m_projectRoot;
    const auto revision = m_services.trust.snapshot(root).revision;
    auto job = std::make_shared<ManagedModelJob>();
    job->target = this;
    m_managedContextTrustRevision = revision;
    m_managedModelJob = job;
    auto completed = [this, job, root, revision, generation, request](Result<ModelResult> result) mutable {
        if (m_shutDown || m_projectRoot != root || m_managedModelGeneration != generation || m_managedModelJob != job ||
            !m_services.trust.snapshot(root).trusted || m_services.trust.snapshot(root).revision != revision)
            return;
        m_managedModelJob.reset();
        if (!result) {
            notify(QStringLiteral(".NET project context"), QString::fromStdString(result.error().message()),
                   ui::NotificationKind::Error);
            return;
        }
        if (auto* discovery = std::get_if<build::ManagedProjectProperties>(&*result)) {
            const auto list = QString::fromStdString(discovery->targetFrameworks.empty() ? discovery->targetFramework
                                                                                         : discovery->targetFrameworks)
                                  .split(';', Qt::SkipEmptyParts);
            if (list.empty()) {
                notify(QStringLiteral(".NET project context"), QStringLiteral("The SDK reported no target frameworks"),
                       ui::NotificationKind::Error);
                return;
            }
            QPointer<Workbench> owner(this);
            bool accepted = true;
            const auto selected =
                list.size() == 1
                    ? list.front()
                    : QInputDialog::getItem(m_window, QStringLiteral(".NET target framework"),
                                            QStringLiteral("Evaluated target framework"), list, 0, false, &accepted);
            if (!owner || !accepted || owner->m_shutDown || owner->m_projectRoot != root ||
                owner->m_managedModelGeneration != generation || !owner->m_services.trust.snapshot(root).trusted ||
                owner->m_services.trust.snapshot(root).revision != revision)
                return;
            request.targetFramework = selected.toStdString();
            if (auto started = configureManagedProject(std::move(request)); !started)
                notify(QStringLiteral(".NET project context"), QString::fromStdString(started.error().message()),
                       ui::NotificationKind::Error);
            return;
        }
        if (m_build->isBuilding()) {
            notify(QStringLiteral(".NET project context"),
                   QStringLiteral("A build started while the context was evaluating; refresh after it finishes."),
                   ui::NotificationKind::Warning);
            return;
        }
        m_managedEvaluation = std::make_shared<const build::ManagedProjectEvaluation>(
            std::move(std::get<build::ManagedProjectEvaluation>(*result)));
        m_managedContextRoot = root;
        m_managedContextTrustRevision = revision;
        m_managedBuildBackend = std::make_unique<ManagedBuildBackend>(
            m_managedEvaluation, root, revision, m_services.fs, m_services.launcher, m_services.trust, *m_buildService,
            *m_pool, m_managedBuildTasks, m_build->serviceCallbacks([this, generation, root, revision] {
                const auto current = m_services.trust.snapshot(root);
                return m_managedModelGeneration == generation && m_projectRoot == root && current.trusted &&
                       current.revision == revision;
            }));
        m_build->setBackend(m_managedBuildBackend.get());
        startManaged(root);
        loadRunConfigurations();
        emit managedProjectChanged();
    };
    m_managedModelTasks.push_back(m_pool->spawn(
        sched::Lane::Background, evaluateManaged(job, &m_services.fs, &m_services.launcher, &m_services.trust, root,
                                                 revision, std::move(request), std::move(completed))));
    emit managedProjectChanged();
    return {};
}
void Workbench::showManagedProjectContext() {
    if (m_projectRoot.empty())
        return;
    QPointer<Workbench> owner(this);
    const auto root = m_projectRoot;
    const auto generation = m_managedModelGeneration;
    const auto revision = m_services.trust.snapshot(root).revision;
    const auto admitted = [owner, root, generation, revision] {
        return owner && !owner->m_shutDown && owner->m_projectRoot == root &&
               owner->m_managedModelGeneration == generation && owner->m_services.trust.snapshot(root).trusted &&
               owner->m_services.trust.snapshot(root).revision == revision;
    };
    const auto file =
        QFileDialog::getOpenFileName(m_window, QStringLiteral("Select SDK C# project"), QString::fromStdString(root),
                                     QStringLiteral("C# project (*.csproj)"));
    if (!admitted() || file.isEmpty())
        return;
    QDialog dialog; // Stack-owned: a closing main window must not delete it during nested dispatch.
    dialog.setWindowTitle(QStringLiteral("Evaluate .NET project context"));
    QFormLayout layout(&dialog);
    std::string host = m_services.settings.snapshot()->get(settings::key::toolsManagedDotnetPath);
    if (host.empty()) {
#ifdef _WIN32
        const auto name = QStringLiteral("dotnet.exe");
#else
        const auto name = QStringLiteral("dotnet");
#endif
        const auto installed = qEnvironmentVariable("DOTNET_ROOT");
        host = installed.isEmpty() ? QStandardPaths::findExecutable(name).toStdString()
                                   : QDir(installed).filePath(name).toStdString();
    }
    QLineEdit dotnet(QString::fromStdString(host), &dialog);
    auto active = m_buildService->activeConfiguration();
    QLineEdit configuration(QString::fromStdString(active ? active->configuration : "Debug"), &dialog);
    QLineEdit rid(&dialog);
    layout.addRow(QStringLiteral(".NET SDK executable"), &dotnet);
    layout.addRow(QStringLiteral("Configuration"), &configuration);
    layout.addRow(QStringLiteral("Runtime identifier (optional)"), &rid);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || !admitted())
        return;
    if (auto started = configureManagedProject({.dotnet = dotnet.text().toStdString(),
                                                .project = file.toStdString(),
                                                .configuration = configuration.text().toStdString(),
                                                .runtimeIdentifier = rid.text().toStdString()});
        !started)
        notify(QStringLiteral(".NET project context"), QString::fromStdString(started.error().message()),
               ui::NotificationKind::Error);
}
void Workbench::attachManagedProjectView() {
    auto* panel = new QWidget(m_window);
    panel->hide();
    auto* layout = new QVBoxLayout(panel);
    auto* select = new QPushButton(QStringLiteral("Choose SDK project / framework..."), panel);
    layout->addWidget(select);
    auto* refresh = new QPushButton(QStringLiteral("Refresh selected SDK context"), panel);
    layout->addWidget(refresh);
    connect(refresh, &QPushButton::clicked, this, [this] {
        if (const auto model = managedProjectEvaluation())
            (void)configureManagedProject(model->request);
        else if (m_managedDesiredContext)
            (void)configureManagedProject(*m_managedDesiredContext);
    });
    auto* diagnostics = new QPushButton(QStringLiteral("Collect .NET diagnostics..."), panel);
    layout->addWidget(diagnostics);
    connect(diagnostics, &QPushButton::clicked, this, [this] { showManagedDiagnostics(); });
    input::ActionSpec diagnosticsAction;
    diagnosticsAction.id = "managed.diagnostics";
    diagnosticsAction.name = ".NET Diagnostics...";
    diagnosticsAction.group = ".NET";
    diagnosticsAction.handler = [this](const input::ActionContext&, const input::InvokeInfo&) {
        showManagedDiagnostics();
    };
    (void)m_registry.add(std::move(diagnosticsAction));
    auto* status = new QLabel(QStringLiteral("No evaluated .NET context"), panel);
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* tree = new QTreeWidget(panel);
    tree->setObjectName(QStringLiteral("managedProjectTree"));
    tree->setColumnCount(2);
    tree->setHeaderLabels({QStringLiteral("Evaluated item/property"), QStringLiteral("Actual SDK value")});
    layout->addWidget(tree);
    auto* browser = new QPushButton(QStringLiteral("Open running profile URL"), panel);
    browser->setObjectName(QStringLiteral("managedProfileBrowser"));
    browser->setToolTip(
        QStringLiteral("Explicit launchSettings URL; no HTTP readiness or browser debugger is inferred"));
    layout->addWidget(browser);
    connect(browser, &QPushButton::clicked, this, [this, tree, status] {
        const auto model = managedProjectEvaluation();
        auto* row = tree->currentItem();
        if (!model || !row || !m_runManager)
            return;
        bool selected = false;
        const auto index = row->data(0, Qt::UserRole + 2).toUInt(&selected);
        if (!selected || index >= model->launchProfiles.size())
            return;
        const auto& profile = model->launchProfiles[index];
        if (!profile.launchBrowser || !profile.unavailable.empty()) {
            status->setText(QStringLiteral("Selected profile does not request a supported browser launch"));
            return;
        }
        const auto suffix =
            ".profile." + QCryptographicHash::hash(QByteArray::fromStdString(profile.name), QCryptographicHash::Sha256)
                              .toHex()
                              .toStdString();
        const auto configurations = managedRunConfigurations();
        auto configuration =
            std::ranges::find_if(configurations, [&](const auto& config) { return config.id.ends_with(suffix); });
        if (configuration == configurations.end())
            return;
        const auto active = m_runManager->activeRun(configuration->id);
        const auto running = active ? m_runManager->status(*active) : std::nullopt;
        if (!running || running->phase != run::RunPhase::Running || running->configuration != *configuration) {
            status->setText(QStringLiteral("Start this exact Project profile before opening its browser URL"));
            return;
        }
        const auto allowed = managedRunAdmissionFor(*configuration)();
        if (!allowed) {
            status->setText(QString::fromStdString(allowed.error().message()));
            return;
        }
        const auto urls = QString::fromStdString(profile.applicationUrl).split(';', Qt::SkipEmptyParts);
        QUrl url(QString::fromStdString(profile.launchUrl));
        if (url.isRelative() && !urls.empty())
            url = QUrl(urls.front()).resolved(url);
        if (!url.isValid() || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) ||
            url.host().isEmpty() || !url.userInfo().isEmpty()) {
            status->setText(QStringLiteral("Profile has no valid absolute HTTP(S) browser URL"));
            return;
        }
        if (!QDesktopServices::openUrl(url))
            status->setText(QStringLiteral("System browser launch failed"));
    });
    connect(select, &QPushButton::clicked, this, [this] { showManagedProjectContext(); });
    connect(this, &Workbench::managedProjectChanged, tree, [this, tree, status] {
        tree->clear();
        const auto model = managedProjectEvaluation();
        if (!model) {
            status->setText(QStringLiteral("Select and evaluate a trusted SDK project; no model is inferred from XML"));
            return;
        }
        status->setText(QString::fromStdString(model->projectFile + " — " + model->request.configuration + " / " +
                                               model->request.targetFramework));
        auto* properties = new QTreeWidgetItem(tree, {QStringLiteral("Properties")});
        for (const auto& [key, value] : model->properties)
            new QTreeWidgetItem(properties, {QString::fromStdString(key), QString::fromStdString(value)});
        auto* profiles = new QTreeWidgetItem(tree, {QStringLiteral("launchSettings profiles")});
        for (std::size_t profileIndex = 0; profileIndex < model->launchProfiles.size(); ++profileIndex) {
            const auto& profile = model->launchProfiles[profileIndex];
            auto* row = new QTreeWidgetItem(
                profiles, {QString::fromStdString(profile.name), profile.unavailable.empty()
                                                                     ? QStringLiteral("Project Run configuration")
                                                                     : QString::fromStdString(profile.unavailable)});
            row->setData(0, Qt::UserRole + 2, static_cast<uint>(profileIndex));
            row->setToolTip(0, QString::fromStdString(profile.applicationUrl + "\n" + profile.launchUrl));
        }
        new QTreeWidgetItem(tree,
                            {QStringLiteral("Framework semantics"),
                             QStringLiteral("Razor/Blazor/XAML/F#/VB require an explicit installed language-server "
                                            "registry provider; no designer or browser debugger is inferred")});
        std::size_t remaining = 4000;
        for (const auto& [kind, items] : model->items) {
            auto* group = new QTreeWidgetItem(
                tree, {QString::fromStdString(kind), QString::number(static_cast<qulonglong>(items.size()))});
            for (const auto& item : items) {
                if (!remaining)
                    break;
                --remaining;
                auto* row = new QTreeWidgetItem(group, {QString::fromStdString(item.identity)});
                if (const auto path = item.metadata.find("FullPath"); path != item.metadata.end()) {
                    row->setText(1, QString::fromStdString(path->second));
                    row->setData(0, Qt::UserRole, QString::fromStdString(path->second));
                }
                if (const auto link = item.metadata.find("Link"); link != item.metadata.end())
                    row->setToolTip(0, QString::fromStdString(link->second));
            }
        }
        if (!remaining)
            new QTreeWidgetItem(
                tree, {QStringLiteral("Display limited to 4000 evaluated rows; complete values remain in the model")});
        tree->expandToDepth(0);
    });
    connect(tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* row, int) {
        const auto path = row->data(0, Qt::UserRole).toString();
        if (!path.isEmpty())
            (void)openPath(path.toStdString());
    });
    ui::ToolWindowSpec spec;
    spec.id = "managedProject";
    spec.title = QStringLiteral(".NET Project");
    spec.side = ui::ToolWindowSide::Left;
    spec.createContent = [panel = QPointer<QWidget>(panel)] { return panel.data(); };
    (void)m_window->toolWindows().add(std::move(spec));
    input::ActionSpec action;
    action.id = "managed.projectModel";
    action.name = ".NET Project";
    action.group = ".NET";
    action.handler = [this](const input::ActionContext&, const input::InvokeInfo&) {
        m_window->toolWindows().show("managedProject");
    };
    (void)m_registry.add(std::move(action));
}
void Workbench::managedProjectInputChanged(std::string_view path) {
    if (!m_managedEvaluation)
        return;
    const auto key = lang::documentKey(path);
    if (!m_managedEvaluation->launchSettingsPath.empty() &&
        key == lang::documentKey(m_managedEvaluation->launchSettingsPath)) {
        auto request = m_managedEvaluation->request;
        clearManagedProjectContext();
        m_managedDesiredContext = std::move(request);
        return;
    }
    for (const auto& [input, bytes] : m_managedEvaluation->inputFiles) {
        static_cast<void>(bytes);
        if (lang::documentKey(input) == key) {
            auto request = m_managedEvaluation->request;
            clearManagedProjectContext();
            m_managedDesiredContext = std::move(request);
            return;
        }
    }
}
void Workbench::onManagedFilesChanged(const io::FileChangeBatch& batch) {
    if (m_shutDown || m_projectRoot.empty() || (!m_managed && !m_managedEvaluation && !m_managedDesiredContext))
        return;
    bool modelChanged = false, sourceChanged = false;
    for (const auto& change : batch.changes)
        for (const auto& path : {change.path, change.oldPath}) {
            if (path.empty())
                continue;
            auto key = lang::documentKey(path);
            std::replace(key.begin(), key.end(), char{92}, '/');
            bool imported = false;
            if (m_managedEvaluation)
                for (const auto& [input, bytes] : m_managedEvaluation->inputFiles) {
                    static_cast<void>(bytes);
                    if (lang::documentKey(input) == lang::documentKey(path)) {
                        imported = true;
                        break;
                    }
                }
            if (imported) {
                modelChanged = true;
                continue;
            }
            if (key.find("/obj/") != std::string::npos || key.find("/bin/") != std::string::npos)
                continue;
            const auto name = io::fileName(key);
            modelChanged = modelChanged || key.ends_with(".csproj") || key.ends_with(".props") ||
                           key.ends_with(".targets") || name == "global.json" || name == ".editorconfig" ||
                           name == "project.assets.json" || change.kind == io::FileChangeKind::Rescan;
            if (key.ends_with(".cs")) {
                if (change.kind != io::FileChangeKind::Modified)
                    modelChanged = true;
                else if (!m_editors->find(path))
                    sourceChanged = true;
            }
        }
    if (!modelChanged && !sourceChanged)
        return;
    if (!m_managedDesiredContext && !m_managedEvaluation) {
        if (m_managed)
            startManaged(m_projectRoot);
        return;
    }
    const auto request = m_managedEvaluation ? std::optional{m_managedEvaluation->request} : m_managedDesiredContext;
    clearManagedProjectContext();
    m_managedDesiredContext = request;
    if (m_managedModelRefreshQueued)
        return;
    m_managedModelRefreshQueued = true;
    const auto root = m_projectRoot;
    QTimer::singleShot(100, this, [this, root] {
        m_managedModelRefreshQueued = false;
        if (m_shutDown || m_projectRoot != root || m_managedModelJob || !m_managedDesiredContext)
            return;
        auto request = *m_managedDesiredContext;
        (void)configureManagedProject(std::move(request));
    });
}
} // namespace frost::app
