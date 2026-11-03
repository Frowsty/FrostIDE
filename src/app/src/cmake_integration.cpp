// SPDX-License-Identifier: Apache-2.0
// CMake folder composition for the workbench.

#include "cmake_integration.hpp"

#include "frost/core/mutex.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/store.hpp"
#include "frost/ui/cmake/cmake_tool_window.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "cmake_build_backend.hpp"
#include <QComboBox>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
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
                                   ui::RunConsole& console, std::string statePath,
                                   std::function<void()> runStateChanged,
                                   std::function<void(const std::string&)> openFile, std::function<void()> modelChanged)
    : m_build(buildController), m_sharedAdmission(sharedAdmission), m_window(window), m_launcher(launcher),
      m_trust(trust), m_console(console), m_runStateChanged(std::move(runStateChanged)),
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
    options.profileRecords = build::cmakeProfileRecords(snapshot->get(settings::key::cmakeProfiles));
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
    if (m_runAction) {
        m_runActionVisible = m_runAction->isVisible();
        m_runAction->setVisible(false);
    }
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
        (void)windows.add(std::move(spec));
    }
    if (!windows.contains(kCMake)) {
        ui::ToolWindowSpec spec;
        spec.id = std::string(kCMake);
        spec.title = tr("CMake");
        spec.side = ui::ToolWindowSide::Bottom;
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
    targetsLayout->addWidget(tree, 1);
    connect(tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
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
                m_console.appendOutput(build::RunOutputKind::System, "a Lua debug launch is pending");
                return;
            }
            if (auto result = m_service->build(build::BuildAction::Build, selected, {}, model); !result)
                m_console.appendOutput(build::RunOutputKind::System, result.error().message());
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
    refreshRunTarget();
    refreshProfiles();
    windows.setContent(kTargets, targets);
    auto* tool = new ui::cmake::CMakeToolWindow(*m_controller);
    windows.setContent(kCMake, tool);
}

CMakeIntegration::~CMakeIntegration() {
    {
        LockGuard lock(m_runRelay->mutex);
        m_runRelay->target = nullptr;
    }
    m_session.reset();
    m_runAdmission.reset();
    auto& toolbar = m_window.mainToolbar();
    if (m_profileAction)
        toolbar.removeAction(m_profileAction);
    if (m_targetAction)
        toolbar.removeAction(m_targetAction);
    delete m_profileAction.data();
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

Result<void> CMakeIntegration::debug(std::function<void(Result<debug::DebugTarget>)> done) {
    if (running() || m_debugDone)
        return fail(ErrorCode::Busy, "a CMake target is already running");
    m_debugDone = std::move(done);
    auto started = run();
    if (!started) {
        m_debugDone = {};
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
    m_profileGeneration = m_service->profileGeneration();
    const std::uint64_t generation = ++m_runGeneration;
    m_pending = true;
    m_stopRequested = false;
    m_console.beginRun(QString::fromStdString(model->targets[*target].name));
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
        m_console.finish(outcome);
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
        if (m_debugDone) {
            auto done = std::move(m_debugDone);
            m_debugDone = {};
            done(std::unexpected(error));
        }
    };
    if (m_stopRequested) {
        failDebug(Error(ErrorCode::Cancelled, "debug build cancelled"));
        failed.stopped = true;
        m_console.finish(failed);
        m_runStateChanged();
        maybeRerun();
        return;
    }
    if (!outcome.succeeded()) {
        failed.error = outcome.error;
        failed.buildFailed = true;
        failDebug(outcome.error.value_or(Error(ErrorCode::IoError, "CMake build failed")));
        m_console.finish(failed);
        m_runStateChanged();
        maybeRerun();
        return;
    }
    if (m_service->profileGeneration() != m_profileGeneration) {
        failed.error = Error(ErrorCode::Cancelled, "the CMake profile changed while building the target");
        failDebug(*failed.error);
        m_console.finish(failed);
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
        m_console.finish(failed);
        m_runStateChanged();
        return;
    }
    io::ProcessSpec spec;
    if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
        auto executable = std::ranges::find_if(target->artifacts, [](const std::string& path) {
            std::string name = std::string(io::fileName(path));
            std::ranges::transform(name, name.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return name.size() >= 4 && name.substr(name.size() - 4) == ".exe";
        });
        if (executable == target->artifacts.end()) {
            executable = std::ranges::find_if(target->artifacts, [](const std::string& path) {
                std::string name = std::string(io::fileName(path));
                std::ranges::transform(name, name.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return !name.ends_with(".pdb");
            });
        }
        if (executable == target->artifacts.end()) {
            failed.error = Error(ErrorCode::NotFound, "the CMake target has no executable artifact");
            failDebug(*failed.error);
            m_console.finish(failed);
            m_runStateChanged();
            return;
        }
        spec.executable = *executable;
    } else {
        spec.executable = target->artifacts.front();
    }
    spec.workingDirectory = m_service->sourceDirectory();
    if (m_debugDone) {
        debug::DebugTarget debugTarget;
        debugTarget.name = target->name;
        debugTarget.program = spec.executable;
        debugTarget.workingDirectory = spec.workingDirectory;
        debugTarget.trustRoots = {m_service->sourceDirectory()};
        auto done = std::move(m_debugDone);
        m_debugDone = {};
        m_runStateChanged();
        done(std::move(debugTarget));
        return;
    }
    spec.stdinMode = io::Stdio::Pipe;
    auto relay = m_runRelay;
    build::RunSessionCallbacks callbacks;
    callbacks.onOutput = [relay](build::RunStream stream, std::string_view text) {
        relay->post([stream, text = std::string(text)](CMakeIntegration& self) {
            if (self.m_shutdown)
                return;
            self.m_console.appendOutput(
                stream == build::RunStream::Stdout ? build::RunOutputKind::Stdout : build::RunOutputKind::Stderr, text);
        });
    };
    callbacks.onExited = [relay](const build::RunExit& exit) {
        relay->post([exit](CMakeIntegration& self) {
            if (self.m_shutdown)
                return;
            self.m_running = false;
            self.m_session.reset();
            self.m_runAdmission.reset();
            build::RunOutcome finished;
            finished.exitCode = exit.exitCode;
            finished.stopped = exit.stopped;
            finished.duration = exit.duration;
            self.m_console.finish(finished);
            self.m_runStateChanged();
            self.maybeRerun();
        });
    };
    auto started = build::RunSession::start(m_launcher, spec, std::move(callbacks));
    if (!started) {
        failed.error = started.error();
        m_console.finish(failed);
    } else {
        m_session = std::move(*started);
        m_runAdmission = std::move(admission);
        m_running = true;
        m_console.setRunning(true, m_session->acceptsInput());
    }
    m_runStateChanged();
}

void CMakeIntegration::stop() {
    m_rerun = false;
    m_stopRequested = true;
    if (m_pending)
        m_service->cancel();
    if (m_running && m_session)
        m_session->stop();
}

void CMakeIntegration::shutdown() {
    if (m_shutdown)
        return;
    m_shutdown = true;
    m_debugDone = {};
    m_rerun = false;
    m_pending = false;
    ++m_runGeneration;
    m_service->cancel();
    if (m_session) {
        m_session->stop();
        m_session.reset();
    }
    m_running = false;
    m_runAdmission.reset();
    m_runStateChanged();
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
        m_console.finish(failed);
    }
}

void CMakeIntegration::input(const QString& line) {
    if (m_running && m_session)
        (void)m_session->writeInput(line.toStdString() + "\n");
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
