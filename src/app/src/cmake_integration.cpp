// SPDX-License-Identifier: Apache-2.0
// CMake folder composition for the workbench.

#include "cmake_integration.hpp"

#include "frost/build/run_target.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/run/run_types.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/store.hpp"
#include "frost/ui/cmake/cmake_tool_window.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_tool_window.hpp"
#include "frost/ui/shell/background_tasks.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "cmake_build_backend.hpp"
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QInputDialog>
#include <QFileDialog>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QSignalBlocker>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cctype>
#include <utility>

namespace frost::app {
namespace {
constexpr std::string_view kTargets = "cmakeTargets";
constexpr std::string_view kCMake = "cmake";

} // namespace

struct CMakeIntegration::RunRelay {
    Mutex mutex;
    CMakeIntegration* target FROST_GUARDED_BY(mutex) = nullptr;
    template<class F>
    void post(F fn) {
        LockGuard lock(mutex);
        if (target) {
            auto* self = target;
            QMetaObject::invokeMethod(self, [self, fn = std::move(fn)] { fn(*self); }, Qt::QueuedConnection);
        }
    }
};

CMakeIntegration::CMakeIntegration(io::IFileSystem& fs, io::IProcessLauncher& launcher, project::TrustStore& trust,
                                   settings::SettingsStore& settingsStore, ui::BuildController& buildController,
                                   build::BuildService& sharedAdmission, ui::ShellWindow& window,
                                   ui::RunToolWindow& runWindow, run::RunManager& runs, std::string statePath,
                                   std::function<void()> runStateChanged,
                                   std::function<void(const std::string&)> openFile, std::function<void()> modelChanged)
    : m_build(buildController), m_sharedAdmission(sharedAdmission), m_window(window), m_trust(trust),
      m_settings(settingsStore), m_runWindow(runWindow), m_runs(runs), m_runStateChanged(std::move(runStateChanged)),
      m_controller(std::make_unique<ui::cmake::CMakeController>()), m_runRelay(std::make_shared<RunRelay>()) {
    {
        LockGuard lock(m_runRelay->mutex);
        m_runRelay->target = this;
    }
    const auto snapshot = settingsStore.snapshot();
    build::CMakeServiceOptions options;
    options.reserveProjectCode = [&sharedAdmission] { return sharedAdmission.reserveRun(); };
    options.releaseProjectCode = [&sharedAdmission] { sharedAdmission.releaseRun(); };
    options.reserveOwnedProjectCode = [&sharedAdmission](std::uint64_t owner) {
        return sharedAdmission.reserveExternalChild(owner);
    };
    options.releaseOwnedProjectCode = [&sharedAdmission](std::uint64_t owner, std::uint64_t child) {
        sharedAdmission.releaseExternalChild(owner, child);
    };
    options.stateFilePath = std::move(statePath);
    options.cmakePath = snapshot->get(settings::key::toolsCmakePath);
    options.ninjaPath = snapshot->get(settings::key::toolsNinjaPath);
    // The pinned SDK's cmake and Ninja come before Visual Studio's (cmake_toolchain.hpp, step 2), as
    // the clangd, LuaLS and lldb-dap searches take FROST_SDK's (R1, CMK-1).
    options.sdkDirectory = qEnvironmentVariable("FROST_SDK").toStdString();
    options.profileRecords = build::cmakeProfileRecords(snapshot->get(settings::key::cmakeProfiles));
    options.buildWrappers = build::cmakeBuildWrappers(snapshot->get(settings::key::cmakeBuildWrappers));
    m_service = build::CMakeService::create(
        fs, launcher, trust, std::move(options),
        cmakeBuildCallbacks(
            m_controller->callbacks(),
            buildController.serviceCallbacks([self = QPointer<CMakeIntegration>(this)] { return !self.isNull(); })));
    m_controllerBackend =
        std::make_unique<ui::cmake::CMakeServiceBackend>(*m_service, [this] { return m_build.buildAdmitted(); });
    m_buildBackend = std::make_unique<CMakeBuildBackend>(*m_service);
    m_controller->setBackend(m_controllerBackend.get());

    auto& toolbar = window.mainToolbar();
    auto* runCombo = toolbar.runConfigurations();
    auto* solutionCombo = toolbar.findChild<QWidget*>(QStringLiteral("solutionConfigurations"));
    QAction* before = nullptr;
    for (QAction* action : toolbar.actions()) {
        if (toolbar.widgetForAction(action) == runCombo) {
            before = action;
            m_runAction = action;
        }
        if (solutionCombo && toolbar.widgetForAction(action) == solutionCombo)
            m_solutionAction = action;
    }
    if (m_solutionAction) {
        m_solutionActionVisible = m_solutionAction->isVisible();
        m_solutionAction->setVisible(false);
    }
    // RUN-3: the toolbar's configuration combo (runConfigurations) stays: it lists the run
    // configurations, one per executable target among them (RUN-2), so it replaces this
    // integration's own target combo, which stays hidden and keeps the target choice in step with
    // the CMake Targets window.
    if (m_runAction)
        m_runActionVisible = m_runAction->isVisible();
    m_profiles = new QComboBox(&toolbar);
    m_profiles->setObjectName(QStringLiteral("cmakeToolbarProfiles"));
    m_profiles->setToolTip(tr("CMake profile"));
    m_profiles->setPlaceholderText(tr("No CMake profile"));
    m_profileAction = toolbar.insertWidget(before, m_profiles);
    m_targets = new QComboBox(&toolbar);
    m_targets->setObjectName(QStringLiteral("cmakeToolbarTargets"));
    m_targets->setToolTip(tr("CMake Run/Debug target"));
    m_targets->setPlaceholderText(tr("No executable target"));
    m_targetAction = toolbar.insertWidget(before, m_targets);
    m_targetAction->setVisible(false);
    m_wrapperAction = new QAction(tr("Build Wrapper…"), &toolbar);
    m_wrapperAction->setObjectName(QStringLiteral("cmakeBuildWrapperAction"));
    toolbar.insertAction(before, m_wrapperAction);
    connect(m_wrapperAction, &QAction::triggered, this, [this] { editBuildWrapper(); });
    // CMK-12: generator-provided targets (notably install) are not necessarily File API targets.
    // They still use the service's Save All, trust, admission and contained cmake --build path.
    m_buildMenu = new QMenu(tr("CMake Build"), &window);
    window.menuBar()->addMenu(m_buildMenu);
    connect(m_buildMenu->addAction(tr("Build named target…")), &QAction::triggered, this, [this] {
        bool accepted = false;
        const auto target = QInputDialog::getText(&m_window, tr("CMake target"), tr("Literal target name:"),
                                                 QLineEdit::Normal, {}, &accepted);
        if (!accepted) return;
        auto started = m_service->buildNamedTarget(target.toStdString());
        if (!started) QMessageBox::warning(&m_window, tr("Build target"), QString::fromStdString(started.error().message()));
    });
    connect(m_buildMenu->addAction(tr("Install")), &QAction::triggered, this, [this] {
        auto started = m_service->buildNamedTarget("install");
        if (!started) QMessageBox::warning(&m_window, tr("Install"), QString::fromStdString(started.error().message()));
    });
    for (const auto& [label, action] : {std::pair{tr("Build file…"), build::BuildAction::Build},
                                     std::pair{tr("Rebuild file…"), build::BuildAction::Rebuild},
                                     std::pair{tr("Clean file…"), build::BuildAction::Clean}}) {
        connect(m_buildMenu->addAction(label), &QAction::triggered, this, [this, action] {
            const auto source = QFileDialog::getOpenFileName(&m_window, tr("Source file"),
                QString::fromStdString(m_service->sourceDirectory()));
            if (source.isEmpty()) return;
            auto started = m_service->buildFile(action, source.toStdString());
            if (!started) QMessageBox::warning(&m_window, tr("File build"), QString::fromStdString(started.error().message()));
        });
    }
    connect(m_profiles, &QComboBox::activated, this, [this](int index) { m_controller->selectProfile(index); });
    connect(m_targets, &QComboBox::activated, this, [this](int index) {
        if (!targetsCurrent() || m_displayProfileGeneration != m_service->profileGeneration())
            return;
        const std::string key = "target:" + m_targets->itemData(index).toString().toUtf8().toStdString();
        const QModelIndex selected = m_controller->treeModel()->indexForKey(key);
        if (selected.isValid()) {
            m_tree->setCurrentIndex(selected);
            // Activation can choose the already-current fallback, which emits no currentChanged.
            rememberTreeTarget();
        } else
            refreshRunTarget();
    });
    connect(m_controller.get(), &ui::cmake::CMakeController::profilesChanged, this, [this] { refreshProfiles(); });

    auto& windows = window.toolWindows();
    if (!windows.contains(kTargets)) {
        ui::ToolWindowSpec spec;
        spec.id = std::string(kTargets);
        spec.title = tr("CMake Targets");
        spec.side = ui::ToolWindowSide::Left;
        spec.iconId = std::string(ui::icons::kBuildBuild);
        (void)windows.add(std::move(spec));
    }
    if (!windows.contains(kCMake)) {
        ui::ToolWindowSpec spec;
        spec.id = std::string(kCMake);
        spec.title = tr("CMake");
        spec.side = ui::ToolWindowSide::Bottom;
        spec.iconId = std::string(ui::icons::kBuildBuild);
        (void)windows.add(std::move(spec));
    }
    auto* tree = new QTreeView();
    tree->setObjectName(QStringLiteral("cmakeTargets"));
    tree->setModel(m_controller->treeModel());
    tree->setHeaderHidden(true);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree = tree;
    auto* targets = new QWidget();
    auto* targetsLayout = new QVBoxLayout(targets);
    targetsLayout->setContentsMargins(0, 0, 0, 0);
    m_targetLabel = new QLabel(targets);
    m_targetLabel->setObjectName(QStringLiteral("cmakeRunTarget"));
    m_targetLabel->setWordWrap(true);
    m_targetLabel->setContentsMargins(6, 4, 6, 4);
    m_guidanceLabel = new QLabel(targets);
    m_guidanceLabel->setObjectName(QStringLiteral("cmakeConfigureGuidance"));
    m_guidanceLabel->setWordWrap(true);
    m_guidanceLabel->setContentsMargins(6, 4, 6, 4);
    targetsLayout->addWidget(m_guidanceLabel);
    targetsLayout->addWidget(m_targetLabel);
    auto* argumentsLabel = new QLabel(tr("Run/Debug arguments"), targets);
    m_arguments = new QLineEdit(targets);
    m_arguments->setObjectName(QStringLiteral("cmakeRunArguments"));
    m_arguments->setMaxLength(4096); // At most 16384 UTF-8 bytes in the session file.
    m_arguments->setToolTip(tr("Arguments for the selected target, using Windows command-line quoting; no shell."));
    argumentsLabel->setBuddy(m_arguments);
    targetsLayout->addWidget(argumentsLabel);
    targetsLayout->addWidget(m_arguments);
    targetsLayout->addWidget(tree, 1);
    connect(m_arguments, &QLineEdit::textChanged, this, [this](const QString& text) {
        const auto model = m_controller->model();
        const auto profile = m_service->activeProfile();
        if (!profile || !model || !targetsCurrent() || m_displayProfileGeneration != m_service->profileGeneration())
            return;
        const auto index = runTarget(*model);
        if (!index)
            return;
        ++m_targetGeneration;
        const auto& target = model->targets[*index].name;
        auto entry = std::ranges::find_if(m_argumentPreferences, [&](const auto& item) {
            return item.profile == profile->name && item.target == target;
        });
        if (entry == m_argumentPreferences.end()) {
            m_argumentPreferences.push_back({profile->name, target, text.toUtf8().toStdString()});
        } else {
            entry->arguments = text.toUtf8().toStdString();
        }
        if (m_argumentPreferencesChanged)
            m_argumentPreferencesChanged(m_argumentPreferences);
    });
    connect(tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
        if (!m_restoringTarget && m_tree && m_tree->currentIndex().isValid())
            ++m_targetGeneration;
        rememberTreeTarget();
        refreshRunTarget();
    });
    connect(tree, &QTreeView::customContextMenuRequested, this, [this](QPoint point) {
        const QModelIndex index = m_tree->indexAt(point);
        const QVariant target = index.data(ui::cmake::CMakeTreeModel::TargetIndexRole);
        if (!target.isValid() || target.toInt() < 0)
            return;
        const auto selected = static_cast<std::size_t>(target.toInt());
        const auto model = m_controller->model();
        if (!model)
            return;
        QMenu menu(m_tree);
        QAction* buildAction = menu.addAction(tr("Build Target"));
        if (menu.exec(m_tree->viewport()->mapToGlobal(point)) == buildAction) {
            if (!m_build.buildAdmitted()) {
                console().appendOutput(build::RunOutputKind::System, "a Lua debug launch is pending");
                return;
            }
            if (auto result = m_service->build(build::BuildAction::Build, selected, {}, model); !result)
                console().appendOutput(build::RunOutputKind::System, result.error().message());
        }
    });
    connect(m_controller->treeModel(), &QAbstractItemModel::modelAboutToBeReset, this, [this] {
        m_expandedKeys.clear();
        const auto visit = [this](auto&& self, const QModelIndex& parent) -> void {
            auto* model = m_controller->treeModel();
            for (int row = 0; row < model->rowCount(parent); ++row) {
                const QModelIndex index = model->index(row, 0, parent);
                if (m_tree->isExpanded(index))
                    m_expandedKeys.push_back(index.data(ui::cmake::CMakeTreeModel::KeyRole).toString());
                self(self, index);
            }
        };
        visit(visit, {});
    });
    connect(m_controller->treeModel(), &QAbstractItemModel::modelReset, this, [this] {
        for (const QString& key : m_expandedKeys) {
            const QModelIndex index = m_controller->treeModel()->indexForKey(key.toStdString());
            if (index.isValid())
                m_tree->setExpanded(index, true);
        }
    });
    connect(tree, &QTreeView::activated, this, [open = std::move(openFile)](const QModelIndex& index) {
        const auto kind =
            static_cast<project::CMakeViewNodeKind>(index.data(ui::cmake::CMakeTreeModel::KindRole).toInt());
        if (kind != project::CMakeViewNodeKind::Source && kind != project::CMakeViewNodeKind::CMakeFile)
            return;
        const QString path = index.data(ui::cmake::CMakeTreeModel::PathRole).toString();
        if (!path.isEmpty())
            open(path.toStdString());
    });
    connect(m_controller.get(), &ui::cmake::CMakeController::modelChanged, this,
            [this, changed = std::move(modelChanged)] {
                if (targetsCurrent()) {
                    m_displayProfileGeneration = m_service->profileGeneration();
                    restoreRunTarget();
                }
                refreshRunTarget();
                changed();
            });
    connect(m_controller.get(), &ui::cmake::CMakeController::busyChanged, this, [this](bool busy) {
        refreshProfiles();
        refreshRunTarget();
        if (!busy)
            maybeRerun();
    });
    connect(m_controller.get(), &ui::cmake::CMakeController::progressChanged, this, [this] { refreshConfigureTask(); });
    refreshRunTarget();
    refreshProfiles();
    windows.setContent(kTargets, targets);
    auto* tool = new ui::cmake::CMakeToolWindow(*m_controller);
    windows.setContent(kCMake, tool);
}

CMakeIntegration::~CMakeIntegration() {
    delete m_buildMenu.data();
    {
        LockGuard lock(m_runRelay->mutex);
        m_runRelay->target = nullptr;
    }
    // The RunManager ends the program's tree; its later events are dropped by the relay.
    if (m_runId)
        m_runs.stop(*m_runId);
    m_runId.reset();
    detachTab(m_running);
    m_runAdmission.reset();
    m_configureStop.reset();
    if (m_configureTask)
        m_configureTask->finish();
    auto& toolbar = m_window.mainToolbar();
    if (m_profileAction)
        toolbar.removeAction(m_profileAction);
    if (m_targetAction)
        toolbar.removeAction(m_targetAction);
    if (m_wrapperAction)
        toolbar.removeAction(m_wrapperAction);
    delete m_profileAction.data();
    delete m_wrapperAction.data();
    delete m_targetAction.data();
    if (m_solutionAction)
        m_solutionAction->setVisible(m_solutionActionVisible);
    if (m_runAction)
        m_runAction->setVisible(m_runActionVisible);
    // The tool window holds a controller reference, so dispose widgets before the controller.
    m_window.toolWindows().hide(kTargets);
    m_window.toolWindows().hide(kCMake);
    m_window.toolWindows().setContent(kTargets, new QWidget());
    m_window.toolWindows().setContent(kCMake, new QWidget());
    m_controller->setBackend(nullptr);
}

/*
 * CMK-15: the CMake service's activity in the status bar's processes indicator. One task spans a
 * stretch of activity (configure, generate, reply read, build) and shows the controller's text as
 * its detail; cancelling it is Stop. A change of cancellability (a reply read cannot be stopped)
 * starts a new task, as a task's cancellability is fixed. The trust wait shows its own dialog.
 */
void CMakeIntegration::refreshConfigureTask() {
    const build::CMakeProgress& progress = m_controller->progress();
    const bool shown =
        progress.activity != build::CMakeActivity::Idle && progress.activity != build::CMakeActivity::WaitingForTrust;
    if (m_configureTask && (!shown || m_configureTask->state().cancellable != progress.cancellable)) {
        m_configureStop.reset();
        m_configureTask->finish();
        m_configureTask.reset();
    }
    if (!shown)
        return;
    if (!m_configureTask) {
        m_configureTask = m_window.backgroundTasks().start(tr("CMake"), progress.cancellable);
        if (progress.cancellable) {
            // Runs on the thread that cancels (the indicator's, the GUI thread).
            m_configureStop.emplace(m_configureTask->stopToken(),
                                    std::function<void()>([this] { m_controller->cancel(); }));
        }
    }
    m_configureTask->setDetail(m_controller->progressText());
}

Result<void> CMakeIntegration::debug(std::function<void(Result<debug::DebugTarget>)> done) {
    return debugPrepared(std::nullopt, std::move(done));
}

Result<void> CMakeIntegration::debug(const run::RunConfiguration& frozen,
                                     std::function<void(Result<debug::DebugTarget>)> done) {
    if (frozen.type != run::RunType::CMakeTarget || frozen.target != runTargetName())
        return fail(ErrorCode::InvalidArgument, "the captured CMake configuration does not name the selected target");
    return debugPrepared(frozen, std::move(done));
}

Result<void> CMakeIntegration::debugPrepared(std::optional<run::RunConfiguration> frozen,
                                             std::function<void(Result<debug::DebugTarget>)> done) {
    if (running() || m_debugDone)
        return fail(ErrorCode::Busy, "a CMake target is already running");
    m_debugConfiguration = std::move(frozen);
    m_debugDone = std::move(done);
    auto started = run();
    if (!started) {
        m_debugDone = {};
        m_debugConfiguration.reset();
        return std::unexpected(started.error());
    }
    return {};
}

std::optional<std::size_t> CMakeIntegration::runTarget(const project::CMakeProject& model) const {
    std::optional<std::size_t> target;
    const auto profile = m_service->activeProfile();
    if (profile) {
        const auto preferred =
            std::ranges::find(m_targetPreferences, profile->name, &std::pair<std::string, std::string>::first);
        if (preferred != m_targetPreferences.end()) {
            for (std::size_t i = 0; i < model.targets.size(); ++i)
                if (model.targets[i].name == preferred->second &&
                    model.targets[i].type == project::CMakeTargetType::Executable) {
                    target = i;
                    break;
                }
        }
    }
    if (!target) {
        for (std::size_t i = 0; i < model.targets.size(); ++i) {
            if (model.targets[i].type == project::CMakeTargetType::Executable) {
                target = i;
                break;
            }
        }
    }
    return target;
}

void CMakeIntegration::refreshRunTarget() {
    const auto model = m_controller->model();
    const auto target = model ? runTarget(*model) : std::nullopt;
    const QSignalBlocker blocker(m_targets);
    m_targets->clear();
    m_targets->setPlaceholderText(model ? tr("No executable target") : tr("Configure CMake to select a target"));
    int selected = -1;
    if (model) {
        for (std::size_t i = 0; i < model->targets.size(); ++i) {
            const auto& entry = model->targets[i];
            if (entry.type != project::CMakeTargetType::Executable ||
                (entry.generatorProvided && (!target || *target != i)))
                continue;
            const QString name = QString::fromStdString(entry.name);
            m_targets->addItem(name, name);
            if (target && *target == i)
                selected = m_targets->count() - 1;
        }
    }
    m_targets->setCurrentIndex(selected);
    m_targets->setEnabled(m_targets->count() > 0);
    const std::string folder = m_service->sourceDirectory();
    QString guidance;
    if (!folder.empty() && !m_trust.isTrusted(folder)) {
        guidance = tr("Project is not trusted. Choose Reload CMake Project below to review trust and configure.");
    } else if (!model && !folder.empty()) {
        guidance = m_service->state() == build::BuildState::Idle
                       ? tr("Choose Reload CMake Project below to configure this project.")
                       : tr("CMake is preparing the project.");
    }
    m_guidanceLabel->setText(guidance);
    m_guidanceLabel->setVisible(!guidance.isEmpty());
    m_targetLabel->setText(target && model
                               ? tr("Run/Debug: %1").arg(QString::fromStdString(model->targets[*target].name))
                               : tr("Run/Debug: configure an executable target"));
    m_targetLabel->setToolTip(tr("Select an executable in the CMake targets tree to change the Run/Debug target."));
    const auto profile = m_service->activeProfile();
    const auto entry = profile && target && model
                           ? std::ranges::find_if(m_argumentPreferences,
                                                  [&](const auto& item) {
                                                      return item.profile == profile->name &&
                                                             item.target == model->targets[*target].name;
                                                  })
                           : m_argumentPreferences.end();
    const QSignalBlocker argumentsBlocker(m_arguments);
    m_arguments->setEnabled(profile && target && model && targetsCurrent() &&
                            m_displayProfileGeneration == m_service->profileGeneration());
    m_arguments->setText(entry == m_argumentPreferences.end() ? QString() : QString::fromStdString(entry->arguments));
    if (m_runTargetObserver)
        m_runTargetObserver(target && model ? model->targets[*target].name : std::string());
}

ui::RunConsole& CMakeIntegration::console() { return m_console ? *m_console : *m_runWindow.currentConsole(); }

void CMakeIntegration::finishConsole(const build::RunOutcome& outcome) {
    // Only the run's own tab ends; a closed tab has nothing left to show.
    if (m_console)
        m_console->finish(outcome);
}

std::string CMakeIntegration::runTargetName() const {
    const auto model = m_controller->model();
    const auto target = model ? runTarget(*model) : std::nullopt;
    return target ? model->targets[*target].name : std::string();
}

void CMakeIntegration::selectRunTarget(const std::string& name) {
    if (!m_targets || name.empty() || runTargetName() == name)
        return;
    const int index = m_targets->findData(QString::fromStdString(name));
    if (index < 0)
        return;
    m_targets->setCurrentIndex(index);
    emit m_targets->activated(index); // The combo's own choice: tree selection and the preference.
}

void CMakeIntegration::rememberTreeTarget() {
    if (m_restoringTarget || !m_tree || !targetsCurrent() ||
        m_displayProfileGeneration != m_service->profileGeneration())
        return;
    const auto profile = m_service->activeProfile();
    const auto model = m_controller->model();
    // Recheck after reading the profile: a service-side presets reload can advance the
    // generation between the first fence and these reads.
    if (!profile || !model || m_displayProfileGeneration != m_service->profileGeneration() ||
        model != m_service->model())
        return;
    QModelIndex index = m_tree->currentIndex();
    while (index.isValid() && index.data(ui::cmake::CMakeTreeModel::KindRole).toInt() !=
                                  static_cast<int>(project::CMakeViewNodeKind::Target))
        index = index.parent();
    if (!index.isValid())
        return;
    const std::string name = index.data().toString().toStdString();
    const auto target = std::ranges::find(model->targets, name, &project::CMakeTarget::name);
    if (target == model->targets.end() || target->type != project::CMakeTargetType::Executable)
        return;
    auto existing = std::ranges::find(m_targetPreferences, profile->name, &std::pair<std::string, std::string>::first);
    if (existing != m_targetPreferences.end()) {
        if (existing->second == name)
            return;
        existing->second = name;
    } else {
        m_targetPreferences.emplace_back(profile->name, name);
    }
    if (m_targetPreferencesChanged)
        m_targetPreferencesChanged(m_targetPreferences);
}

void CMakeIntegration::setTargetPreferences(
    std::vector<std::pair<std::string, std::string>> preferences,
    std::function<void(const std::vector<std::pair<std::string, std::string>>&)> changed) {
    m_targetPreferences = std::move(preferences);
    m_targetPreferencesChanged = std::move(changed);
    restoreRunTarget();
    refreshRunTarget();
}

void CMakeIntegration::setArgumentPreferences(
    std::vector<ui::nav::SessionCMakeArguments> preferences,
    std::function<void(const std::vector<ui::nav::SessionCMakeArguments>&)> changed) {
    m_argumentPreferences = std::move(preferences);
    m_argumentPreferencesChanged = std::move(changed);
    refreshRunTarget();
}

void CMakeIntegration::restoreRunTarget() {
    if (!m_tree || !targetsCurrent() || m_displayProfileGeneration != m_service->profileGeneration())
        return;
    const auto model = m_controller->model();
    const auto target = runTarget(*model);
    if (!target)
        return;
    const QModelIndex selected = m_controller->treeModel()->indexForKey("target:" + model->targets[*target].name);
    if (selected.isValid()) {
        m_restoringTarget = true;
        m_tree->setCurrentIndex(selected);
        m_restoringTarget = false;
    }
}

void CMakeIntegration::refreshProfiles() {
    const QSignalBlocker blocker(m_profiles);
    m_profiles->clear();
    m_profiles->addItems(m_controller->profileLabels());
    m_profiles->setCurrentIndex(m_controller->activeProfile());
    m_profiles->setEnabled(m_profiles->count() > 1 && !m_controller->busy());
}

void CMakeIntegration::editBuildWrapper() {
    const QPointer<CMakeIntegration> self(this);
    const auto profile = m_service->activeProfile();
    const std::string root = m_service->sourceDirectory();
    if (!profile || root.empty())
        return;
    auto document = m_settings.readScopeDocument(settings::Scope::Application);
    const auto* spec = m_settings.registry().find(settings::key::cmakeBuildWrappers.name);
    if (!document || !spec) {
        QMessageBox::warning(&m_window, tr("Build Wrapper"), tr("Cannot read personal build wrapper settings."));
        return;
    }
    const auto* member = document->find(settings::key::cmakeBuildWrappers.name);
    const std::optional<std::string> expected =
        member ? std::optional<std::string>(document->rawValue(*member)) : std::nullopt;
    auto value = document->value(spec->key, spec->schema);
    if (member && !value) {
        QMessageBox::warning(&m_window, tr("Build Wrapper"), tr("The existing build wrapper settings are invalid."));
        return;
    }
    const auto oldRecords = value ? std::get<settings::RecordList>(*value) : settings::RecordList{};
    const auto oldWrappers = build::cmakeBuildWrappers(oldRecords);
    build::CMakeBuildWrapper current;
    for (const auto& wrapper : oldWrappers)
        if (lang::documentKey(wrapper.root) == lang::documentKey(root) && wrapper.profile == profile->name) {
            current = wrapper;
            break;
        }
    QDialog dialog(&m_window);
    dialog.setObjectName(QStringLiteral("cmakeBuildWrapperDialog"));
    dialog.setWindowTitle(tr("CMake Build Wrapper"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* note = new QLabel(tr("Personal settings for %1 / %2. A trusted project is required. "
                               "Enter one argument per line; {target} and {profile} are replaced as whole arguments.")
                                .arg(QString::fromStdString(root), QString::fromStdString(profile->name)),
                            &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* form = new QFormLayout();
    auto* executable = new QLineEdit(QString::fromStdString(current.executable), &dialog);
    auto* target = new QLineEdit(QString::fromStdString(current.defaultTarget), &dialog);
    auto lines = [](const std::vector<std::string>& values) {
        QStringList out;
        for (const auto& value : values)
            out.push_back(QString::fromStdString(value));
        return out.join(QLatin1Char('\n'));
    };
    auto* buildArgs = new QPlainTextEdit(lines(current.buildArguments), &dialog);
    auto* cleanArgs = new QPlainTextEdit(lines(current.cleanArguments), &dialog);
    auto* rebuildArgs = new QPlainTextEdit(lines(current.rebuildArguments), &dialog);
    executable->setObjectName(QStringLiteral("cmakeWrapperExecutable"));
    target->setObjectName(QStringLiteral("cmakeWrapperDefaultTarget"));
    buildArgs->setObjectName(QStringLiteral("cmakeWrapperBuildArguments"));
    cleanArgs->setObjectName(QStringLiteral("cmakeWrapperCleanArguments"));
    rebuildArgs->setObjectName(QStringLiteral("cmakeWrapperRebuildArguments"));
    for (auto* edit : {buildArgs, cleanArgs, rebuildArgs})
        edit->setMaximumHeight(90);
    form->addRow(tr("Executable (absolute path)"), executable);
    form->addRow(tr("Default target"), target);
    form->addRow(tr("Build arguments"), buildArgs);
    form->addRow(tr("Clean arguments"), cleanArgs);
    form->addRow(tr("Rebuild arguments"), rebuildArgs);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    ui::installDialogChrome(&dialog, true);
    const auto trustRevision = m_trust.snapshot(root).revision;
    if (dialog.exec() != QDialog::Accepted || !self)
        return;
    const auto active = m_service->activeProfile();
    if (m_service->sourceDirectory() != root || !active || active->name != profile->name ||
        m_trust.snapshot(root).revision != trustRevision) {
        QMessageBox::warning(&m_window, tr("Build Wrapper"),
                             tr("The project, profile or trust changed; reopen this dialog."));
        return;
    }
    const std::string program = executable->text().toUtf8().toStdString();
    if (!program.empty() && !io::isAbsolutePath(program)) {
        QMessageBox::warning(&m_window, tr("Build Wrapper"), tr("The executable path must be absolute."));
        return;
    }
    auto values = [](const QPlainTextEdit* edit) {
        std::vector<std::string> out;
        for (const QString& line : edit->toPlainText().split(QLatin1Char('\n')))
            if (!line.isEmpty())
                out.push_back(line.toUtf8().toStdString());
        return out;
    };
    auto records = oldRecords;
    std::erase_if(records, [&](const settings::Record& record) {
        const auto decoded = build::cmakeBuildWrappers({record});
        return !decoded.empty() && lang::documentKey(decoded.front().root) == lang::documentKey(root) &&
               decoded.front().profile == profile->name;
    });
    if (!program.empty()) {
        records.push_back(settings::makeRecord({{"root", root},
                                                {"profile", profile->name},
                                                {"executable", program},
                                                {"defaultTarget", target->text().toUtf8().toStdString()},
                                                {"buildArguments", values(buildArgs)},
                                                {"cleanArguments", values(cleanArgs)},
                                                {"rebuildArguments", values(rebuildArgs)}}));
    }
    auto reserved = m_sharedAdmission.reserveRun();
    if (!reserved) {
        QMessageBox::warning(&m_window, tr("Build Wrapper"), QString::fromStdString(reserved.error().message()));
        return;
    }
    struct Release {
        build::BuildService& service;
        ~Release() { service.releaseRun(); }
    } release{m_sharedAdmission};
    if (auto changed = m_service->setBuildWrappers(build::cmakeBuildWrappers(records)); !changed) {
        QMessageBox::warning(&m_window, tr("Build Wrapper"), QString::fromStdString(changed.error().message()));
        return;
    }
    const settings::ConditionalSettingEdit edit{std::string(settings::key::cmakeBuildWrappers.name), expected,
                                                settings::Value(records)};
    if (auto saved = m_settings.applyConditional(settings::Scope::Application, std::span(&edit, 1)); !saved) {
        (void)m_service->setBuildWrappers(oldWrappers);
        QMessageBox::warning(&m_window, tr("Build Wrapper"), QString::fromStdString(saved.error().message()));
    }
}

Result<build::RunId> CMakeIntegration::run() {
    if (!m_build.buildAdmitted())
        return fail(ErrorCode::Busy, "a Lua debug launch is pending");
    if (m_shutdown)
        return fail(ErrorCode::Cancelled, "the workbench is shutting down");
    if (running())
        return fail(ErrorCode::Busy, "a CMake target is already running");
    const auto model = m_controller->model();
    if (!model)
        return fail(ErrorCode::InvalidArgument, "configure the CMake project first");
    const auto target = runTarget(*model);
    if (!target)
        return fail(ErrorCode::NotFound, "the CMake project has no executable target");
    auto reserved = m_sharedAdmission.reserveRun();
    if (!reserved)
        return std::unexpected(std::move(reserved.error()));
    m_runAdmission = std::shared_ptr<void>(
        &m_sharedAdmission, [](void* service) { static_cast<build::BuildService*>(service)->releaseRun(); });
    m_runTarget = model->targets[*target].id;
    const auto profile = m_service->activeProfile();
    const auto arguments = profile ? std::ranges::find_if(m_argumentPreferences,
                                                          [&](const auto& item) {
                                                              return item.profile == profile->name &&
                                                                     item.target == model->targets[*target].name;
                                                          })
                                   : m_argumentPreferences.end();
    m_runArguments = arguments == m_argumentPreferences.end() ? std::vector<std::string>{}
                                                              : build::splitWindowsArguments(arguments->arguments);
    m_runTargetGeneration = m_targetGeneration;
    m_runRoot = m_service->sourceDirectory();
    m_profileGeneration = m_service->profileGeneration();
    const std::uint64_t generation = ++m_runGeneration;
    m_pending = true;
    m_stopRequested = false;
    {
        // RUN-3: the target's tab in the Run tool window (a rerun reuses it once it has finished).
        // The tab outlives this integration (the Run window is the workbench's; closing or reopening
        // a folder destroys us), so the handlers hold a QPointer and detachTab() clears them.
        ui::RunTabHandlers handlers;
        const QPointer<CMakeIntegration> self(this);
        handlers.stop = [self] {
            if (self)
                self->stop();
        };
        handlers.rerun = [self] {
            if (!self)
                return;
            if (auto again = self->rerun(); !again)
                self->console().appendOutput(build::RunOutputKind::System, again.error().message() + "\n");
        };
        handlers.input = [self](const QString& line) {
            if (self)
                self->input(line);
        };
        const std::string& name = model->targets[*target].name;
        m_console = m_runWindow.openRun(QStringLiteral("cmake:") + QString::fromStdString(name),
                                        QString::fromStdString(name), std::move(handlers));
    }
    m_window.toolWindows().show(ui::tool_windows::kRun, false);
    m_runStateChanged();
    const auto relay = m_runRelay;
    if (auto started = m_service->build(
            build::BuildAction::Build, target,
            [relay, generation](const build::CMakeBuildOutcome& outcome) {
                relay->post([generation, outcome](CMakeIntegration& self) { self.buildFinished(generation, outcome); });
            },
            model);
        !started) {
        m_pending = false;
        m_runAdmission.reset();
        m_runStateChanged();
        build::RunOutcome outcome;
        outcome.error = started.error();
        finishConsole(outcome);
        return std::unexpected(std::move(started.error()));
    }
    return build::RunId{1};
}

void CMakeIntegration::buildFinished(std::uint64_t generation, const build::CMakeBuildOutcome& outcome) {
    if (m_shutdown || generation != m_runGeneration || !m_pending)
        return;
    m_pending = false;
    auto admission = std::move(m_runAdmission);
    build::RunOutcome failed;
    const auto failDebug = [this](const Error& error) {
        m_debugConfiguration.reset();
        if (m_debugDone) {
            auto done = std::move(m_debugDone);
            m_debugDone = {};
            done(std::unexpected(error));
        }
    };
    if (m_stopRequested) {
        failDebug(Error(ErrorCode::Cancelled, "debug build cancelled"));
        failed.stopped = true;
        finishConsole(failed);
        m_runStateChanged();
        maybeRerun();
        return;
    }
    if (!outcome.succeeded()) {
        failed.error = outcome.error;
        failed.buildFailed = true;
        failDebug(outcome.error.value_or(Error(ErrorCode::IoError, "CMake build failed")));
        finishConsole(failed);
        m_runStateChanged();
        maybeRerun();
        return;
    }
    if (m_service->profileGeneration() != m_profileGeneration) {
        failed.error = Error(ErrorCode::Cancelled, "the CMake profile changed while building the target");
        failDebug(*failed.error);
        finishConsole(failed);
        m_runStateChanged();
        return;
    }
    const auto currentModel = m_controller->model();
    const auto currentTarget = currentModel ? runTarget(*currentModel) : std::nullopt;
    const auto trust = m_trust.snapshot(m_runRoot);
    if (m_targetGeneration != m_runTargetGeneration || m_service->sourceDirectory() != m_runRoot ||
        outcome.sourceDirectory != m_runRoot || !trust.trusted || trust.revision != outcome.trustRevision ||
        !currentTarget || currentModel != outcome.model || m_service->model() != outcome.model ||
        currentModel->targets[*currentTarget].id != m_runTarget) {
        failed.error = Error(ErrorCode::Cancelled, "the project, trust or target changed while building");
        failDebug(*failed.error);
        finishConsole(failed);
        m_runStateChanged();
        return;
    }
    const auto& model = outcome.model;
    const project::CMakeTarget* target = nullptr;
    if (model) {
        auto it = std::ranges::find_if(model->targets, [this](const auto& t) { return t.id == m_runTarget; });
        if (it != model->targets.end())
            target = &*it;
    }
    if (!target || target->artifacts.empty() || !m_trust.isTrusted(m_service->sourceDirectory())) {
        failed.error = Error(ErrorCode::NotFound, "the built target has no executable artifact or trust was revoked");
        failDebug(*failed.error);
        finishConsole(failed);
        m_runStateChanged();
        return;
    }
    auto program = run::cmakeTargetProgram(*target);
    if (!program) {
        failed.error = program.error();
        failDebug(*failed.error);
        finishConsole(failed);
        m_runStateChanged();
        return;
    }
    /*
     * The target is built, so the program starts through the RunManager (RUN-1) as the target's
     * cmake-target configuration (RUN-2), whose program is the built artifact and which has no
     * before-launch step left; the workbench lays the user's edits (Run/Debug Configurations, RUN-3)
     * over it. The build stays here, before the RunManager's run, because its admission, trust and
     * profile checks above belong to this integration.
     */
    run::RunConfiguration configuration;
    configuration.id = run::runConfigurationIdFor(run::kCMakeIdPrefix, target->name);
    configuration.name = target->name;
    configuration.type = run::RunType::CMakeTarget;
    configuration.target = target->name;
    configuration.program = *program;
    configuration.arguments = m_runArguments;
    configuration.workingDirectory = m_service->sourceDirectory();
    if (m_debugConfiguration) {
        const auto& frozen = *m_debugConfiguration;
        configuration.id = frozen.id;
        configuration.name = frozen.name;
        if (!frozen.arguments.empty())
            configuration.arguments = frozen.arguments;
        if (!frozen.workingDirectory.empty())
            configuration.workingDirectory = frozen.workingDirectory;
        configuration.environment = frozen.environment;
    } else if (m_launchConfigurator)
        configuration = m_launchConfigurator(std::move(configuration));
    if (m_debugDone) {
        debug::DebugTarget debugTarget;
        debugTarget.name = configuration.name;
        debugTarget.program = configuration.program;
        debugTarget.workingDirectory = configuration.workingDirectory;
        debugTarget.arguments = configuration.arguments;
        if (m_debugConfiguration &&
            std::ranges::any_of(configuration.environment, [](const auto& entry) { return !entry.value; })) {
            // DAP has no unset entry. Capture the inherited environment and send the complete
            // resulting map, preserving each frozen removal instead of silently dropping it.
            auto environment = QProcessEnvironment::systemEnvironment();
            for (const auto& entry : configuration.environment) {
                if (entry.value)
                    environment.insert(QString::fromStdString(entry.name), QString::fromStdString(*entry.value));
                else
                    environment.remove(QString::fromStdString(entry.name));
            }
            for (const auto& name : environment.keys())
                debugTarget.environment.emplace_back(name.toStdString(), environment.value(name).toStdString());
            debugTarget.inheritEnvironment = false;
        } else {
            for (const auto& entry : configuration.environment)
                if (entry.value)
                    debugTarget.environment.emplace_back(entry.name, *entry.value);
        }
        debugTarget.trustRoots = {m_service->sourceDirectory()};
        auto done = std::move(m_debugDone);
        m_debugDone = {};
        m_debugConfiguration.reset();
        m_runStateChanged();
        done(std::move(debugTarget));
        return;
    }
    auto relay = m_runRelay;
    run::RunHandlers handlers;
    handlers.onOutput = [relay](run::OutputStream stream, std::string_view text) {
        relay->post([stream, text = std::string(text)](CMakeIntegration& self) {
            if (self.m_shutdown)
                return;
            if (!self.m_console)
                return; // The tab was closed.
            self.m_console->appendOutput(stream == run::OutputStream::Stdout ? build::RunOutputKind::Stdout
                                                                             : build::RunOutputKind::Stderr,
                                         text);
        });
    };
    handlers.onFinished = [relay](const run::RunResult& result) {
        relay->post([result](CMakeIntegration& self) { self.runFinished(result); });
    };
    auto started = m_runs.start(configuration, std::move(handlers));
    if (!started) {
        failed.error = started.error();
        finishConsole(failed);
    } else {
        m_runId = *started;
        m_runAdmission = std::move(admission);
        m_running = true;
        if (m_console)
            m_console->setRunning(true, true); // stdin is a pipe (RunManager); input before the launch is dropped.
    }
    m_runStateChanged();
}

void CMakeIntegration::runFinished(const run::RunResult& result) {
    if (m_shutdown || m_runId != result.id)
        return;
    m_running = false;
    m_runId.reset();
    m_runAdmission.reset();
    build::RunOutcome finished;
    finished.error = result.error;
    finished.exitCode = result.exitCode;
    finished.stopped = result.stopped;
    finished.duration = result.duration;
    finishConsole(finished);
    m_runStateChanged();
    maybeRerun();
}

void CMakeIntegration::stop() {
    m_rerun = false;
    m_stopRequested = true;
    m_debugConfiguration.reset();
    if (m_pending)
        m_service->cancel();
    if (m_running && m_runId)
        m_runs.stop(*m_runId);
}

void CMakeIntegration::shutdown() {
    if (m_shutdown)
        return;
    m_shutdown = true;
    m_debugDone = {};
    m_debugConfiguration.reset();
    m_rerun = false;
    m_pending = false;
    ++m_runGeneration;
    const bool launched = m_running; // Read before it is cleared: the tab's last line depends on it.
    m_service->cancel();
    if (m_runId) {
        m_runs.stop(*m_runId); // Ends the whole tree; the RunManager collects the exit.
        m_runId.reset();
    }
    m_running = false;
    m_runAdmission.reset();
    detachTab(launched);
    m_runStateChanged();
}

void CMakeIntegration::detachTab(bool launched) {
    // A live m_console means the Run window (its parent) is alive too.
    if (!m_console)
        return;
    if (m_console->active()) {
        // The run's tree was just stopped and the relay drops its exit, so the tab says so itself;
        // "(running)" would claim a run that no longer exists.
        build::RunOutcome stopped;
        stopped.stopped = true;
        // Without an exit code the default line reads "Stopped before the program started", which is
        // true only while the build was pending; a launched program ran (its output is above).
        m_console->finish(stopped, launched ? QStringLiteral("Stopped: FrostIDE ended the program when the "
                                                             "project closed (its exit code was not collected)")
                                            : QString());
    }
    m_runWindow.detach(m_console);
    m_console = nullptr;
}

Result<build::RunId> CMakeIntegration::rerun() {
    if (!running())
        return run();
    stop();
    m_rerun = true;
    return build::RunId{1};
}

void CMakeIntegration::maybeRerun() {
    if (m_shutdown || !m_rerun || running() || m_service->state() != build::BuildState::Idle)
        return;
    m_rerun = false;
    if (auto started = run(); !started) {
        build::RunOutcome failed;
        failed.error = started.error();
        finishConsole(failed);
    }
}

void CMakeIntegration::input(const QString& line) {
    if (m_running && m_runId)
        (void)m_runs.sendInput(*m_runId, line.toStdString() + "\n");
}

Result<void> CMakeIntegration::open(const std::string& folder) {
    auto opened = m_service->openFolder(folder);
    if (opened) {
        refreshRunTarget();
        m_build.setBackend(m_buildBackend.get());
        m_window.toolWindows().show(kTargets, false);
        m_window.toolWindows().show(kCMake, false);
    }
    return opened;
}
} // namespace frost::app
