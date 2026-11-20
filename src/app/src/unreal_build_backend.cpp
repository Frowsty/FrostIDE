// SPDX-License-Identifier: Apache-2.0
// Unreal tooling execution through the same contained BuildSession and UI events as other builds.
#include "unreal_build_backend.hpp"

#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/ui/shell/shell_window.hpp"

#include <QAction>
#include <QInputDialog>
#include <QFileDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QStandardPaths>
#include <QDialog>
#include <QEventLoop>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <thread>
#include "frost/rpc/json.hpp"

namespace frost::app {
void UnrealBuildBackend::attachMenu(ui::ShellWindow& window, std::function<Result<void>(std::string)> addEditor,
    std::function<void(const std::string&)> openFile, std::function<void()> semanticProfileChanged) {
    m_semanticProfileChanged = std::move(semanticProfileChanged);
    m_menu = new QMenu(tr("Unreal"), &window);
    window.menuBar()->addMenu(m_menu);
    const QPointer<UnrealBuildBackend> self(this);
    const QPointer<ui::ShellWindow> host(&window);
    auto report = [host](Result<void> result) {
        if (host && !result) QMessageBox::warning(host.data(), tr("Unreal"), QString::fromStdString(result.error().message()));
    };
    connect(m_menu->addAction(tr("Project modules, plugins and targets…")), &QAction::triggered, this,
        [self, host, openFile] { if (self && host) self->showProjectModel(host.data(), openFile); });
    connect(m_menu->addAction(tr("Select generated compilation database for current profile…")), &QAction::triggered, this,
        [self, host, report] {
            if (!self || !host) return;
            const auto identity = self->configurationIdentity();
            if (identity.empty()) { report(fail(ErrorCode::InvalidArgument, "Select build tools and target first")); return; }
            const auto file = QFileDialog::getOpenFileName(host.data(), tr("Select actual generated database for the selected UBT profile"), {}, tr("Compilation database (compile_commands.json)"));
            if (!self || !host || file.isEmpty()) return;
            if (self->configurationIdentity() != identity) { report(fail(ErrorCode::Busy, "Unreal profile changed while selecting its database")); return; }
            report(self->selectCompileDatabase(file.toUtf8().toStdString()));
        });
    if (openFile) connect(m_menu->addAction(tr("Open existing UHT generated header…")), &QAction::triggered, this,
        [self, host, openFile] {
            const auto file = QFileDialog::getOpenFileName(host.data(), tr("Open actual UHT output"), {}, tr("Generated headers (*.generated.h)"));
            if (self && host && !file.isEmpty()) openFile(file.toUtf8().toStdString());
        });
    connect(m_menu->addAction(tr("Select installed native visualizer…")), &QAction::triggered, this, [this, self, host] {
        const auto file = QFileDialog::getOpenFileName(host.data(), tr("Select installed Unreal .natvis file"), {}, tr("Native visualizer (*.natvis)"));
        if (!self || !host) return;
        if (!file.isEmpty()) m_visualizer = file.toUtf8().toStdString();
    });
    if (addEditor) connect(m_menu->addAction(tr("Add native Editor run/debug configuration…")), &QAction::triggered, this,
        [self, host, report, addEditor=std::move(addEditor)] {
            const auto editor = QFileDialog::getOpenFileName(host.data(), tr("Select installed Unreal Editor executable"));
            if (!self || !host) return;
            if (!editor.isEmpty()) report(addEditor(editor.toUtf8().toStdString()));
        });
    connect(m_menu->addAction(tr("Select build tools and target…")), &QAction::triggered, this, [this, self, host, report] {
        if (m_state != build::BuildState::Idle) { report(fail(ErrorCode::Busy, "Stop the current Unreal operation first")); return; }
        auto selected = m_request;
        const auto tool = QFileDialog::getOpenFileName(host.data(), tr("Select installed UBT executable or dotnet host"));
        if (!self || !host || tool.isEmpty()) return;
        selected.tool = tool.toUtf8().toStdString();
        bool ok = false;
        const auto mode = QInputDialog::getItem(host.data(), tr("UBT invocation"), tr("Selected executable is:"),
            {tr("UBT executable"), tr("dotnet host")}, 0, false, &ok);
        if (!self || !host || !ok) return;
        selected.buildToolAssembly.clear();
        if (mode == tr("dotnet host")) {
            const auto assembly = QFileDialog::getOpenFileName(host.data(), tr("Select installed UnrealBuildTool DLL"), {}, tr("Assembly (*.dll)"));
            if (!self || !host || assembly.isEmpty()) return;
            selected.buildToolAssembly = assembly.toUtf8().toStdString();
        }
        selected.target = QInputDialog::getText(host.data(), tr("UBT target"), tr("Target name from the project's Target.cs (for example MyGameEditor):"),
            QLineEdit::Normal, QString::fromStdString(selected.target), &ok).toUtf8().toStdString();
        if (!self || !host || !ok) return;
        const auto platform = QInputDialog::getItem(host.data(), tr("UBT platform"), tr("Target platform:"), {QStringLiteral("Win64"), QStringLiteral("Linux")},
#ifdef _WIN32
            0,
#else
            1,
#endif
            false, &ok);
        if (!self || !host || !ok) return;
        selected.platform = platform == QStringLiteral("Linux") ? build::UnrealPlatform::Linux : build::UnrealPlatform::Win64;
        const auto configuration = QInputDialog::getItem(host.data(), tr("UBT configuration"), tr("Configuration:"),
            {QStringLiteral("DebugGame"), QStringLiteral("Development"), QStringLiteral("Shipping")}, 1, false, &ok);
        if (!self || !host || !ok) return;
        selected.configuration = configuration == QStringLiteral("DebugGame") ? build::UnrealConfiguration::DebugGame :
            configuration == QStringLiteral("Shipping") ? build::UnrealConfiguration::Shipping : build::UnrealConfiguration::Development;
        selected.operation = build::UnrealOperation::Build;
        report(configure(std::move(selected)));
    });
    for (const auto& item : {std::pair{tr("Generate project files"), build::UnrealOperation::GenerateProjectFiles},
                            std::pair{tr("Generate compilation database"), build::UnrealOperation::GenerateCompileDatabase}})
        connect(m_menu->addAction(item.first), &QAction::triggered, this, [this, operation=item.second, report] { report(buildTarget(operation)); });
    connect(m_menu->addAction(tr("Run UHT with selected manifest…")), &QAction::triggered, this, [this, self, host, report] {
        if (m_state != build::BuildState::Idle) { report(fail(ErrorCode::Busy, "Stop the current Unreal operation first")); return; }
        const auto manifest = QFileDialog::getOpenFileName(host.data(), tr("Select existing UHT manifest"), {}, tr("UHT manifest (*.uhtmanifest)"));
        if (!self || !host || manifest.isEmpty()) return;
        m_request.headerManifest = manifest.toUtf8().toStdString();
        report(buildTarget(build::UnrealOperation::HeaderTool));
    });
}
UnrealBuildBackend::UnrealBuildBackend(project::UnrealProject model, io::IFileSystem& fs,
                                               io::IProcessLauncher& launcher, project::TrustStore& trust,
                                               build::BuildService& admission, build::BuildServiceCallbacks callbacks)
    : m_model(std::move(model)), m_root(io::parentPath(m_model.projectFile)), m_fs(fs), m_launcher(launcher), m_trust(trust), m_admission(admission),
      m_callbacks(std::move(callbacks)) {
    m_request.projectFile = m_model.projectFile;
    m_trustWatch.setInterval(25);
    connect(&m_trustWatch, &QTimer::timeout, this, [this] { checkTrustRevision(); });
}

UnrealBuildBackend::~UnrealBuildBackend() {
    delete m_modelDialog.data();
    delete m_menu.data();
    if (m_session) {
        m_session->cancel();
        m_session.reset();
    }
    if (m_reserved)
        m_admission.releaseRun();
}
Result<void> UnrealBuildBackend::openSolution(std::shared_ptr<const project::SolutionModel>) {
    return fail(ErrorCode::Unsupported, "an Unreal project is open");
}
Result<void> UnrealBuildBackend::closeSolution() {
    if (m_state != build::BuildState::Idle)
        return fail(ErrorCode::Busy, "stop the Unreal operation before closing");
    return {};
}
Result<void> UnrealBuildBackend::setActiveConfiguration(std::size_t) {
    return fail(ErrorCode::Unsupported, "Select the Unreal target and configuration from the Unreal menu");
}
bool UnrealBuildBackend::admitted() const {
    const auto snapshot = m_trust.snapshot(m_root);
    return m_trustRevision && snapshot.trusted && snapshot.revision == *m_trustRevision;
}
void UnrealBuildBackend::checkTrustRevision() {
    if (m_state == build::BuildState::Idle || m_state == build::BuildState::NeedsTrust || admitted()) return;
    m_diagnostics.clear();
    if (m_callbacks.onDiagnosticsCleared) m_callbacks.onDiagnosticsCleared();
    m_outcome.error = Error(ErrorCode::Untrusted, "Unreal operation trust revision changed");
    m_cancelled = true;
    if (m_session) { setState(build::BuildState::Cancelling); m_session->cancel(); }
    else { ++m_generation; finish(m_outcome.error); }
}
void UnrealBuildBackend::setState(build::BuildState state) {
    m_state = state;
    if (m_callbacks.onStateChanged)
        m_callbacks.onStateChanged(state);
}
Result<void> UnrealBuildBackend::configure(build::UnrealBuildRequest request) {
    if (m_state != build::BuildState::Idle) return fail(ErrorCode::Busy, "an Unreal operation is active");
    if (request.projectFile != m_model.projectFile) return fail(ErrorCode::InvalidArgument, "Unreal project identity mismatch");
    auto spec = build::unrealBuildSpec(request);
    if (!spec) return std::unexpected(spec.error());
    const auto before = configurationIdentity();
    m_request = std::move(request);
    if (configurationIdentity() != before) {
        m_databaseFile.clear();
        if (m_semanticProfileChanged) m_semanticProfileChanged();
    }
    return {};
}
std::string UnrealBuildBackend::configurationIdentity() const {
    auto request = m_request; request.operation = build::UnrealOperation::Build;
    auto spec = build::unrealBuildSpec(request);
    if (!spec) return {};
    std::vector<rpc::Json> arguments;
    for (const auto& argument : spec->arguments) arguments.emplace_back(argument);
    return rpc::Json::object().set("tool", spec->executable).set("arguments", rpc::Json::array(std::move(arguments))).dump();
}
Result<void> UnrealBuildBackend::selectCompileDatabase(std::string file) {
    if (m_state != build::BuildState::Idle) return fail(ErrorCode::Busy, "Stop the Unreal operation before selecting a database");
    if (!io::isAbsolutePath(file) || file.find('\0') != std::string::npos || io::fileName(file) != "compile_commands.json" || configurationIdentity().empty())
        return fail(ErrorCode::InvalidArgument, "Select an absolute generated database for a configured Unreal target");
    m_databaseFile = std::move(file);
    if (m_semanticProfileChanged) m_semanticProfileChanged();
    return {};
}
void UnrealBuildBackend::showProjectModel(QWidget* parent, std::function<void(const std::string&)> openFile) {
    const QPointer<UnrealBuildBackend> self(this);
    const QPointer<QWidget> host(parent);
    const auto projectFile = m_model.projectFile;
    auto* fs = &m_fs;
    QEventLoop loop;
    std::optional<Result<project::UnrealWorkspace>> loaded;
    std::jthread worker([&loop, &loaded, fs, projectFile] {
        auto result = project::readUnrealWorkspace(*fs, projectFile);
        QMetaObject::invokeMethod(&loop, [&loop, &loaded, result=std::move(result)]() mutable { loaded=std::move(result); loop.quit(); }, Qt::QueuedConnection);
    });
    loop.exec(); worker.join();
    if (!self || !host) return;
    if (!loaded || !*loaded) {
        QMessageBox::warning(host.data(), tr("Unreal project model"), loaded ? QString::fromStdString(loaded->error().message()) : tr("Project read interrupted"));
        return;
    }
    m_workspace = std::move(**loaded);
    delete m_modelDialog.data();
    auto* dialog = new QDialog(host.data()); m_modelDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Unreal project model")); dialog->resize(720, 500);
    auto* layout = new QVBoxLayout(dialog);
    auto* note = new QLabel(tr("Project-local descriptor metadata and rule-file names. C# rules are not evaluated; installed engine/plugin contents are not searched."), dialog);
    note->setWordWrap(true); layout->addWidget(note);
    auto* tree = new QTreeWidget(dialog); tree->setObjectName(QStringLiteral("unrealProjectTree"));
    tree->setHeaderLabels({tr("Item"), tr("Metadata"), tr("Source")}); layout->addWidget(tree);
    auto row = [](QTreeWidgetItem* parentItem, const QString& name, const QString& detail, const std::string& path) {
        auto* item = new QTreeWidgetItem(parentItem, {name, detail, QString::fromStdString(path)});
        if (!path.empty()) item->setData(0, Qt::UserRole, QString::fromStdString(path));
        return item;
    };
    auto* root = new QTreeWidgetItem(tree, {QString::fromStdString(std::string(io::fileName(projectFile))), QString::fromStdString(m_workspace->project.engineAssociation), QString::fromStdString(projectFile)});
    root->setData(0, Qt::UserRole, QString::fromStdString(projectFile));
    auto* modules = row(root, tr("Project modules"), {}, {});
    for (const auto& module : m_workspace->project.modules) {
        std::string rule;
        for (const auto& file : m_workspace->moduleRules) if (file.owner == "Project" && file.name == module.name) rule = file.path;
        row(modules, QString::fromStdString(module.name), QString::fromStdString(module.type + " / " + module.loadingPhase), rule);
    }
    auto* targets = row(root, tr("Target rule files"), tr("Names only; target capabilities require UBT"), {});
    for (const auto& file : m_workspace->targetRules) row(targets, QString::fromStdString(file.name), QString::fromStdString(file.owner), file.path);
    auto* plugins = row(root, tr("Plugin references"), {}, {});
    for (const auto& plugin : m_workspace->project.plugins) {
        std::string descriptorFile;
        for (const auto& local : m_workspace->localPlugins) if (local.name == plugin.name) descriptorFile = local.descriptorFile;
        row(plugins, QString::fromStdString(plugin.name), plugin.enabled ? tr("Enabled reference") : tr("Disabled reference"), descriptorFile);
    }
    auto* localPlugins = row(root, tr("Local plugin descriptors"), tr("Discovery does not imply enablement"), {});
    for (const auto& plugin : m_workspace->localPlugins) {
        auto* item = row(localPlugins, QString::fromStdString(plugin.name), QString::fromStdString(plugin.descriptor.friendlyName), plugin.descriptorFile);
        for (const auto& module : plugin.descriptor.modules) {
            std::string rule;
            for (const auto& file : m_workspace->moduleRules) if (file.owner == "plugin:" + plugin.name && file.name == module.name) rule=file.path;
            row(item, QString::fromStdString(module.name), QString::fromStdString(module.type), rule);
        }
    }
    tree->expandToDepth(1); tree->resizeColumnToContents(0);
    connect(tree, &QTreeWidget::itemActivated, this, [openFile](QTreeWidgetItem* item, int) {
        const auto path = item->data(0, Qt::UserRole).toString();
        if (openFile && !path.isEmpty()) openFile(path.toUtf8().toStdString());
    });
    dialog->show();
}
Result<void> UnrealBuildBackend::build(build::BuildAction action, std::optional<project::ProjectId> project) {
    if (project) return fail(ErrorCode::Unsupported, "select a UBT target from the Unreal menu");
    if (action == build::BuildAction::Rebuild) return fail(ErrorCode::Unsupported, "use Clean followed by Build for the selected UBT target");
    return buildTarget(action == build::BuildAction::Clean ? build::UnrealOperation::Clean : build::UnrealOperation::Build);
}
Result<void> UnrealBuildBackend::buildTarget(build::UnrealOperation operation) {
    if (m_state != build::BuildState::Idle) return fail(ErrorCode::Busy, "an Unreal operation is already active");
    auto request = m_request;
    request.operation = operation;
    auto planned = build::unrealBuildSpec(request);
    if (!planned) return std::unexpected(planned.error());
    auto reserved = m_admission.reserveRun();
    if (!reserved)
        return reserved;
    m_reserved = true;
    ++m_generation;
    m_steps = {std::move(*planned)};
    m_step = 0;
    m_cancelled = false;
    m_diagnostics.clear();
    m_outcome = {};
    m_outcome.target.projectName = request.target;
    if (m_callbacks.onDiagnosticsCleared)
        m_callbacks.onDiagnosticsCleared();
    if (!m_trust.isTrusted(m_root)) {
        setState(build::BuildState::NeedsTrust);
        if (m_callbacks.onNeedsTrust)
            m_callbacks.onNeedsTrust({m_root});
    } else
        saveAndStart();
    return {};
}
void UnrealBuildBackend::saveAndStart() {
    const auto trust = m_trust.snapshot(m_root);
    if (!trust.trusted) { finish(Error(ErrorCode::Untrusted, "Unreal project trust changed")); return; }
    m_trustRevision = trust.revision;
    m_trustWatch.start();
    setState(build::BuildState::SavingDocuments);
    const auto generation = m_generation;
    QPointer<UnrealBuildBackend> self(this);
    auto saved = [self, generation](Result<void> result) mutable {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, generation, result = std::move(result)]() mutable {
                if (!self || self->m_generation != generation || self->m_state != build::BuildState::SavingDocuments)
                    return;
                if (!result)
                    self->finish(result.error());
                else
                    self->next();
            },
            Qt::QueuedConnection);
    };
    if (m_save)
        m_save(std::move(saved));
    else
        saved({});
}
void UnrealBuildBackend::next() {
    // Called only on the GUI thread, after the preceding session's last event.
    m_session.reset();
    if (m_cancelled) {
        finish(fail(ErrorCode::Cancelled, "Unreal operation cancelled").error());
        return;
    }
    if (m_step == m_steps.size()) {
        finish();
        return;
    }
    if (!admitted()) {
        finish(fail(ErrorCode::Untrusted, "Unreal operation trust revision changed").error());
        return;
    }
    setState(build::BuildState::Starting);
    const io::ProcessSpec spec = m_steps[m_step++];
    QPointer<UnrealBuildBackend> self(this);
    build::BuildCallbacks callbacks;
    callbacks.onLine = [self](auto stream, std::string_view line) {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, stream, line = std::string(line)] {
                if (self && self->admitted() && self->m_callbacks.onOutput)
                    self->m_callbacks.onOutput(stream == build::OutputStream::Stdout ? build::BuildOutputKind::Stdout
                                                                                     : build::BuildOutputKind::Stderr,
                                               line);
            },
            Qt::QueuedConnection);
    };
    callbacks.onDiagnostic = [self](const build::Diagnostic& diagnostic) {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, diagnostic] {
                if (!self || !self->admitted())
                    return;
                build::DiagnosticResolver resolver(self->m_fs, self->m_root);
                auto resolved = resolver.resolve(diagnostic);
                self->m_diagnostics.push_back(resolved);
                if (self->m_callbacks.onDiagnostic)
                    self->m_callbacks.onDiagnostic(resolved);
            },
            Qt::QueuedConnection);
    };
    callbacks.onFinished = [self](const build::BuildResult& result) {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, result] {
                if (!self)
                    return;
                if (!self->admitted()) {
                    self->m_diagnostics.clear();
                    if (self->m_callbacks.onDiagnosticsCleared) self->m_callbacks.onDiagnosticsCleared();
                    self->finish(Error(ErrorCode::Untrusted, "Unreal operation trust revision changed"));
                    return;
                }
                self->m_outcome.result = result;
                if (result.success && !self->m_cancelled)
                    self->next();
                else
                    self->finish();
            },
            Qt::QueuedConnection);
    };
    build::BuildSessionOptions options;
    options.parser = build::makeCompilerOutputParser;
    if (!admitted()) { finish(Error(ErrorCode::Untrusted, "Unreal operation trust revision changed")); return; }
    auto session = build::BuildSession::start(m_launcher, spec, true, std::move(callbacks), systemClock(), options);
    if (!session) {
        finish(session.error());
        return;
    }
    m_session = std::move(*session);
    setState(build::BuildState::Running);
}
void UnrealBuildBackend::finish(std::optional<Error> error) {
    m_trustWatch.stop();
    m_trustRevision.reset();
    m_session.reset();
    m_outcome.error = std::move(error);
    if (m_outcome.error)
        m_outcome.result.success = false;
    m_outcome.result.cancelled = m_outcome.result.cancelled || m_cancelled;
    if (m_outcome.result.cancelled)
        m_outcome.result.success = false;
    m_outcome.counts = {};
    for (const auto& item : m_diagnostics) {
        if (item.diagnostic.severity == build::Severity::Error)
            ++m_outcome.counts.errors;
        else if (item.diagnostic.severity == build::Severity::Warning)
            ++m_outcome.counts.warnings;
        else
            ++m_outcome.counts.messages;
    }
    if (m_reserved) {
        m_admission.releaseRun();
        m_reserved = false;
    }
    if (m_callbacks.onFinished)
        m_callbacks.onFinished(m_outcome);
    setState(build::BuildState::Idle);
}
void UnrealBuildBackend::cancel() {
    if (m_state == build::BuildState::Idle)
        return;
    m_cancelled = true;
    if (m_session) {
        setState(build::BuildState::Cancelling);
        m_session->cancel();
    } else {
        ++m_generation;
        finish(fail(ErrorCode::Cancelled, "Unreal operation cancelled").error());
    }
}
Result<void> UnrealBuildBackend::trust(std::string_view root) {
    auto result = m_trust.trust(root);
    if (result && m_state == build::BuildState::NeedsTrust && m_trust.isTrusted(m_root))
        saveAndStart();
    return result;
}
std::vector<std::string> UnrealBuildBackend::untrustedRoots() const {
    return m_trust.isTrusted(m_root) ? std::vector<std::string>{} : std::vector<std::string>{m_root};
}
} // namespace frost::app
