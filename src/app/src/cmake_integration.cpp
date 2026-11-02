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
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "cmake_build_backend.hpp"
#include <QMenu>
#include <QMetaObject>
#include <QTreeView>
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
                                   ui::ShellWindow& window, ui::RunConsole& console, std::string statePath,
                                   std::function<void()> runStateChanged,
                                   std::function<void(const std::string&)> openFile, std::function<void()> modelChanged)
    : m_build(buildController), m_window(window), m_launcher(launcher), m_trust(trust), m_console(console),
      m_runStateChanged(std::move(runStateChanged)), m_controller(std::make_unique<ui::cmake::CMakeController>()),
      m_runRelay(std::make_shared<RunRelay>()) {
    {
        LockGuard lock(m_runRelay->mutex);
        m_runRelay->target = this;
    }
    const auto snapshot = settingsStore.snapshot();
    build::CMakeServiceOptions options;
    options.stateFilePath = std::move(statePath);
    options.cmakePath = snapshot->get(settings::key::toolsCmakePath);
    options.ninjaPath = snapshot->get(settings::key::toolsNinjaPath);
    options.profileRecords = build::cmakeProfileRecords(snapshot->get(settings::key::cmakeProfiles));
    m_service = build::CMakeService::create(
        fs, launcher, trust, std::move(options),
        cmakeBuildCallbacks(
            m_controller->callbacks(),
            buildController.serviceCallbacks([self = QPointer<CMakeIntegration>(this)] { return !self.isNull(); })));
    m_controllerBackend = std::make_unique<ui::cmake::CMakeServiceBackend>(*m_service);
    m_buildBackend = std::make_unique<CMakeBuildBackend>(*m_service);
    m_controller->setBackend(m_controllerBackend.get());

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
    connect(tree, &QTreeView::customContextMenuRequested, this, [this](QPoint point) {
        const QModelIndex index = m_tree->indexAt(point);
        const QVariant target = index.data(ui::cmake::CMakeTreeModel::TargetIndexRole);
        if (!target.isValid() || target.toInt() < 0)
            return;
        const auto selected = static_cast<std::size_t>(target.toInt());
        QMenu menu(m_tree);
        QAction* buildAction = menu.addAction(tr("Build Target"));
        if (menu.exec(m_tree->viewport()->mapToGlobal(point)) == buildAction) {
            if (auto result = m_service->build(build::BuildAction::Build, selected); !result)
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
            [changed = std::move(modelChanged)] { changed(); });
    connect(m_controller.get(), &ui::cmake::CMakeController::busyChanged, this, [this](bool busy) {
        if (!busy)
            maybeRerun();
    });
    windows.setContent(kTargets, tree);
    auto* tool = new ui::cmake::CMakeToolWindow(*m_controller);
    windows.setContent(kCMake, tool);
}

CMakeIntegration::~CMakeIntegration() {
    {
        LockGuard lock(m_runRelay->mutex);
        m_runRelay->target = nullptr;
    }
    m_session.reset();
    // The tool window holds a controller reference, so dispose widgets before the controller.
    m_window.toolWindows().hide(kTargets);
    m_window.toolWindows().hide(kCMake);
    m_window.toolWindows().setContent(kTargets, new QWidget());
    m_window.toolWindows().setContent(kCMake, new QWidget());
    m_controller->setBackend(nullptr);
}

Result<build::RunId> CMakeIntegration::run() {
    if (m_shutdown)
        return fail(ErrorCode::Cancelled, "the workbench is shutting down");
    if (running())
        return fail(ErrorCode::Busy, "a CMake target is already running");
    const auto model = m_service->model();
    if (!model)
        return fail(ErrorCode::InvalidArgument, "configure the CMake project first");
    std::optional<std::size_t> target;
    if (m_tree && m_tree->currentIndex().isValid()) {
        const QVariant data = m_tree->currentIndex().data(ui::cmake::CMakeTreeModel::TargetIndexRole);
        const int index = data.isValid() ? data.toInt() : -1;
        if (index >= 0 && static_cast<std::size_t>(index) < model->targets.size() &&
            model->targets[static_cast<std::size_t>(index)].type == project::CMakeTargetType::Executable) {
            target = static_cast<std::size_t>(index);
        }
    }
    if (!target) {
        for (std::size_t i = 0; i < model->targets.size(); ++i) {
            if (model->targets[i].type == project::CMakeTargetType::Executable) {
                target = i;
                break;
            }
        }
    }
    if (!target)
        return fail(ErrorCode::NotFound, "the CMake project has no executable target");
    m_runTarget = model->targets[*target].id;
    m_profileGeneration = m_service->profileGeneration();
    const std::uint64_t generation = ++m_runGeneration;
    m_pending = true;
    m_stopRequested = false;
    m_console.beginRun(QString::fromStdString(model->targets[*target].name));
    m_window.toolWindows().show(ui::tool_windows::kRun, false);
    m_runStateChanged();
    const auto relay = m_runRelay;
    if (auto started = m_service->build(build::BuildAction::Build, target,
                                        [relay, generation](const build::CMakeBuildOutcome& outcome) {
                                            relay->post([generation, outcome](CMakeIntegration& self) {
                                                self.buildFinished(generation, outcome);
                                            });
                                        });
        !started) {
        m_pending = false;
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
    build::RunOutcome failed;
    if (m_stopRequested) {
        failed.stopped = true;
        m_console.finish(failed);
        m_runStateChanged();
        maybeRerun();
        return;
    }
    if (!outcome.succeeded()) {
        failed.error = outcome.error;
        failed.buildFailed = true;
        m_console.finish(failed);
        m_runStateChanged();
        maybeRerun();
        return;
    }
    if (m_service->profileGeneration() != m_profileGeneration) {
        failed.error = Error(ErrorCode::Cancelled, "the CMake profile changed while building the target");
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
            m_console.finish(failed);
            m_runStateChanged();
            return;
        }
        spec.executable = *executable;
    } else {
        spec.executable = target->artifacts.front();
    }
    spec.workingDirectory = m_service->sourceDirectory();
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
    m_rerun = false;
    m_pending = false;
    ++m_runGeneration;
    m_service->cancel();
    if (m_session) {
        m_session->stop();
        m_session.reset();
    }
    m_running = false;
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
        m_build.setBackend(m_buildBackend.get());
        m_window.toolWindows().show(kTargets, false);
        m_window.toolWindows().show(kCMake, false);
    }
    return opened;
}
} // namespace frost::app
