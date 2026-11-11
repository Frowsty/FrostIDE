// SPDX-License-Identifier: Apache-2.0
// FrostEngine project coordination, bounded discovery and push-driven controls in the workbench.
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/frost_engine_lua.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/engine_console.hpp"
#include "frost/ui/toolwindows/engine_view.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "engine_integration_internal.hpp"
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <tuple>

namespace frost::app {
namespace {
QString qs(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }

sched::Task<void> engineBackgroundWork(std::function<void()> work) {
    work();
    co_return;
}
} // namespace

void EngineIntegration::Impl::setStatus(const QString& message) {
    if (status)
        status->setText(message);
}

void EngineIntegration::Impl::background(std::function<void()> work) {
    std::erase_if(tasks, [](const auto& task) { return task.ready(); });
    tasks.push_back(workbench.m_pool->spawn(sched::Lane::Background, engineBackgroundWork(std::move(work))));
}

bool EngineIntegration::Impl::admitted(std::uint64_t expectedEpoch, std::string_view expectedRoot,
                                       std::uint64_t expectedTrust) const {
    if (stopped || workbench.m_shutDown || epoch != expectedEpoch || root != expectedRoot ||
        workbench.projectRoot() != expectedRoot)
        return false;
    const auto trust = workbench.m_services.trust.snapshot(expectedRoot);
    if (!trust.trusted || trust.revision != expectedTrust)
        return false;
    return physicalAdmission->load();
}

void EngineIntegration::Impl::invalidate(bool stopOwned) {
    ++epoch;
    physicalAdmission->store(false);
    physicalAdmission = std::make_shared<std::atomic<bool>>(true);
    ++discoveryGeneration;
    ++contentGeneration;
    workStop.request_stop();
    workStop = std::stop_source{};
    if (runAdmission)
        runAdmission->active.store(false);
    if (startupWatch)
        startupWatch->stop();
    awaitingOwnedEndpoint = false;
    awaitingOwnedAuthentication = false;
    ownedProcessStarted = false;
    if (stopOwned && ownedRun && workbench.m_runManager)
        workbench.m_runManager->stop(*ownedRun);
    ownedRun.reset();
    ownedPid = 0;
    runAdmission.reset();
    clearSession(true);
    instances.clear();
    installations.clear();
    projectContent.reset();
    if (picker)
        picker->clear();
    if (projectView)
        projectView->clear();
    discoveryBusy = false;
    projectBusy = false;
    if (workbench.m_problems)
        workbench.m_problems->model()->clear(ui::DiagnosticSource::Engine);
    updateControls();
}

void EngineIntegration::Impl::projectChanged() {
    if (stopped)
        return;
    invalidate(true);
    root = workbench.projectRoot();
    auto physical =
        root.empty() ? Result<std::string>(fail(ErrorCode::NotFound)) : workbench.m_services.fs.canonicalPath(root);
    physicalRoot = physical ? *physical : std::string{};
    selectedProject = lang::isFrostEngineProjectFileName(io::fileName(workbench.m_projectIdentity))
                          ? workbench.m_projectIdentity
                          : physicalRoot;
    projectKey = physical ? lang::documentKey(*physical) : std::string{};
    physicalAdmission->store(physical.has_value());
    available = lang::isFrostEngineProjectRoot(workbench.m_services.fs, root);
    const auto trust = workbench.m_services.trust.snapshot(root);
    trustRevision = trust.revision;
    if (workbench.m_window) {
        auto& windows = workbench.m_window->toolWindows();
        if (available && !registered) {
            const QPointer<QWidget> content = panel;
            auto added = windows.add({std::string(kEngineConsoleWindow),
                                      QStringLiteral("FrostEngine"),
                                      ui::ToolWindowSide::Bottom,
                                      std::string(ui::icons::kRunRun),
                                      {},
                                      [content] { return content.data(); },
                                      260});
            registered = added.has_value();
        }
        if (available && !projectRegistered) {
            const QPointer<ui::EngineView> content = projectView;
            auto added = windows.add({std::string(kEngineProjectWindow),
                                      QStringLiteral("Engine"),
                                      ui::ToolWindowSide::Left,
                                      std::string(ui::icons::kRunRun),
                                      {},
                                      [content] { return content.data(); },
                                      260});
            projectRegistered = added.has_value();
        }
        for (const auto id : {kEngineConsoleWindow, kEngineProjectWindow}) {
            if (!available)
                windows.hide(id);
            if (auto* button = windows.stripeButton(id))
                button->setVisible(available);
        }
    }
    if (!available)
        setStatus(QStringLiteral("Open a FrostEngine project to connect an editor"));
    else if (!trust.trusted)
        setStatus(QStringLiteral("Trust the project before using FrostEngine tools"));
    else
        (void)refresh();
    updateControls();
}

Result<void> EngineIntegration::Impl::refresh() {
    if (stopped || !available)
        return fail(ErrorCode::Unsupported, "Open a FrostEngine project before discovering editors");
    const auto trust = workbench.m_services.trust.snapshot(root);
    if (!trust.trusted)
        return fail(ErrorCode::Untrusted, "Trust the project before discovering FrostEngine editors");
    if (!physicalAdmission->load())
        return fail(ErrorCode::PermissionDenied, "Reopen the project after its physical directory changes");
    if (trust.revision != trustRevision) {
        projectChanged();
        return {};
    }
    if (discoveryBusy)
        return fail(ErrorCode::Busy, "FrostEngine discovery is already running");
    discoveryBusy = true;
    const auto generation = ++discoveryGeneration;
    const auto currentEpoch = epoch;
    const auto expectedRoot = root;
    const auto revision = trustRevision;
    const auto stop = workStop.get_token();
    const auto configured = !chosenProgram.empty() ? chosenProgram : workbench.m_options.frostEditorPath;
    const auto directory = automationDirectory;
    const auto originalKey = projectKey;
    const auto admission = physicalAdmission;
    auto* fs = &workbench.m_services.fs;
    QPointer<EngineIntegration> self(&owner);
    setStatus(QStringLiteral("Looking for FrostEngine editors..."));
    updateControls();
    background([self, fs, expectedRoot, originalKey, admission, revision, currentEpoch, generation, stop, configured,
                directory] {
        if (!retainEngineProjectIdentity(*fs, expectedRoot, originalKey, admission))
            return;
        auto editors = locateEngineInstallations(*fs, expectedRoot, configured, stop);
        auto found = listEngineInstances(*fs, directory, expectedRoot, stop);
        if (!self || !retainEngineProjectIdentity(*fs, expectedRoot, originalKey, admission))
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, expectedRoot, revision, currentEpoch, generation, editors = std::move(editors),
             found = std::move(found)]() mutable {
                if (!self)
                    return;
                auto& impl = *self->m_impl;
                if (!impl.admitted(currentEpoch, expectedRoot, revision) || generation != impl.discoveryGeneration)
                    return;
                impl.discoveryBusy = false;
                impl.installations = editors ? std::move(*editors) : std::vector<EngineInstallation>{};
                impl.instances = found ? std::move(*found) : std::vector<EngineInstanceRecord>{};
                impl.picker->clear();
                for (const auto& instance : impl.instances) {
                    const QString label =
                        instance.instanceId.empty()
                            ? QStringLiteral("Legacy editor on port %1 (pairing unavailable)").arg(instance.port)
                            : QStringLiteral("Editor %1 — port %2")
                                  .arg(qs(instance.instanceId), QString::number(instance.port));
                    impl.picker->addItem(label, qs(instance.instanceId));
                }
                if (!impl.selectedInstance.empty()) {
                    const int selected = impl.picker->findData(qs(impl.selectedInstance));
                    if (selected >= 0)
                        impl.picker->setCurrentIndex(selected);
                }
                if (!found)
                    impl.setStatus(qs(found.error().message()));
                else if (!editors && impl.instances.empty())
                    impl.setStatus(qs(editors.error().message()));
                else
                    impl.setStatus(
                        QStringLiteral("%1 editor instance(s) found; select one to pair").arg(impl.instances.size()));
                if (impl.workbench.m_runController)
                    impl.workbench.m_runController->refreshStartupCombo();
                impl.refreshProjectContent();
                impl.updateControls();
            },
            Qt::QueuedConnection);
    });
    return {};
}

void EngineIntegration::Impl::updateControls() {
    const bool trusted = !root.empty() && workbench.m_services.trust.snapshot(root).trusted;
    const bool ready = available && trusted && admitted(epoch, root, trustRevision);
    if (refreshButton)
        refreshButton->setEnabled(ready && !discoveryBusy);
    if (pairButton)
        pairButton->setEnabled(ready && !discoveryBusy && !ownedRun && picker && picker->currentIndex() >= 0 &&
                               !picker->currentData().toString().isEmpty());
    if (startButton)
        startButton->setEnabled(ready && !installations.empty() && !ownedRun && !runAdmission);
    if (disconnectButton)
        disconnectButton->setEnabled(session != nullptr);
    const bool connected =
        identity.has_value() && session && session->connected() && admitted(epoch, root, trustRevision);
    if (cancelButton)
        cancelButton->setEnabled(connected && !requests.empty());
    for (const auto& button : commandButtons)
        if (button)
            button->setEnabled(connected);
    if (console) {
        console->setConnected(connected);
        console->setPlaying(connected && state && state->playing);
    }
    if (!workbench.m_window)
        return;
    if (!available) {
        workbench.m_window->mainToolbar().setEngineState(std::nullopt);
        return;
    }
    if (identity && session && session->connected()) {
        if (nativeDebug && nativeDebug->matched() && workbench.m_debug)
            nativeBoundDebugGeneration = workbench.m_debug->sessionGeneration();
        else
            nativeBoundDebugGeneration.reset();
    }
    ui::EngineControlState controls;
    controls.connected = connected && state.has_value();
    controls.playing = state && state->playing;
    controls.paused = state && state->paused;
    controls.debuggerPaused = nativeDebug && nativeDebug->paused();
    policy->nativePaused.store(controls.debuggerPaused);
    if ((connected || controls.debuggerPaused) && indicator)
        indicator->setState(controls.debuggerPaused ? ui::EngineConnectionState::DebuggerPaused
                            : controls.paused       ? ui::EngineConnectionState::Paused
                                                    : ui::EngineConnectionState::Connected);
    const auto allowed = [this, connected](std::string_view method) {
        if (!connected)
            return false;
        std::scoped_lock lock(policy->mutex);
        auto context = policy->context;
        if (policy->nativePaused.load())
            context.state = "DebuggerPaused";
        return policy->policy && policy->policy->admit(method, lang::Json::object(), context).has_value();
    };
    controls.canPlay = allowed(state && state->paused ? "play.pause" : "play.start");
    controls.canPause = allowed("play.pause");
    controls.canStep = allowed("play.step");
    controls.canStop = controls.debuggerPaused ? nativeDebug->canTerminate() : allowed("play.stop");
    controls.canSimulate = allowed("play.simulate");
    workbench.m_window->mainToolbar().setEngineState(controls);
    workbench.m_window->refreshActions();
}

EngineIntegration::EngineIntegration(Workbench& workbench)
    : QObject(&workbench), m_impl(std::make_unique<Impl>(*this, workbench)) {
    auto& impl = *m_impl;
    impl.automationDirectory = workbench.m_options.engineAutomationDirectory;
    if (impl.automationDirectory.empty()) {
        QString directory = qEnvironmentVariable("FROST_ENGINE_AUTOMATION_DIR");
        if (directory.isEmpty()) {
            QString config = qEnvironmentVariable("FROST_EDITOR_CONFIG_DIR");
            if (config.isEmpty()) {
#ifdef _WIN32
                config = QDir(qEnvironmentVariable("APPDATA")).filePath(QStringLiteral("FrostEngine/Editor"));
#else
                QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
                if (!QDir::isAbsolutePath(base))
                    base = QDir::home().filePath(QStringLiteral(".config"));
                config = QDir(base).filePath(QStringLiteral("FrostEngine/Editor"));
#endif
            }
            directory = QDir(config).filePath(QStringLiteral("Automation"));
        }
        impl.automationDirectory = directory.toUtf8().toStdString();
    }
    auto* panel = new QWidget(workbench.m_window);
    panel->setObjectName(QStringLiteral("frostEnginePanel"));
    panel->hide();
    impl.panel = panel;
    auto* layout = new QVBoxLayout(panel);
    auto* row = new QHBoxLayout;
    impl.picker = new QComboBox(panel);
    impl.picker->setObjectName(QStringLiteral("engineInstancePicker"));
    row->addWidget(impl.picker, 1);
    const auto button = [&](const QString& title, const char* name, auto handler) {
        auto* result = new QPushButton(title, panel);
        result->setObjectName(QLatin1String(name));
        row->addWidget(result);
        connect(result, &QPushButton::clicked, this, handler);
        return result;
    };
    impl.refreshButton = button(QStringLiteral("Refresh"), "engineRefreshButton", [this] {
        if (auto result = refreshInstances(); !result)
            m_impl->setStatus(qs(result.error().message()));
    });
    impl.pairButton = button(QStringLiteral("Pair"), "enginePairButton", [this] {
        auto result = pair(m_impl->picker->currentData().toString().toUtf8().toStdString());
        if (!result)
            m_impl->setStatus(qs(result.error().message()));
    });
    impl.startButton = button(QStringLiteral("Start Editor"), "engineStartEditorButton", [this] {
        if (auto result = startEditor(); !result)
            m_impl->setStatus(qs(result.error().message()));
    });
    impl.disconnectButton = button(QStringLiteral("Disconnect"), "engineDisconnectButton", [this] { disconnect(); });
    button(QStringLiteral("Locate Editor..."), "engineLocateEditorButton", [this] {
        const QString file = QFileDialog::getOpenFileName(m_impl->panel, QStringLiteral("Locate FrostEditor"));
        if (!file.isEmpty()) {
            m_impl->chosenProgram = file.toUtf8().toStdString();
            (void)refreshInstances();
        }
    });
    layout->addLayout(row);
    auto* commandRow = new QHBoxLayout;
    const auto commandButton = [&](const QString& title, const char* name, auto handler) {
        auto* result = new QPushButton(title, panel);
        result->setObjectName(QLatin1String(name));
        commandRow->addWidget(result);
        connect(result, &QPushButton::clicked, this, handler);
        impl.commandButtons.emplace_back(result);
        return result;
    };
    commandButton(QStringLiteral("Validate Script"), "engineValidateScriptButton", [this] {
        if (auto result = validateScript(); !result)
            m_impl->setStatus(qs(result.error().message()));
    });
    commandButton(QStringLiteral("Run Tests"), "engineRunTestsButton", [this] {
        if (auto result = command("test.run", lang::Json::object()); !result)
            m_impl->setStatus(qs(result.error().message()));
    });
    commandButton(QStringLiteral("Focus Editor"), "engineFocusEditorButton", [this] {
        if (auto result = command("editor.focus", lang::Json::object()); !result)
            m_impl->setStatus(qs(result.error().message()));
    });
    impl.cancelButton = new QPushButton(QStringLiteral("Cancel Request"), panel);
    impl.cancelButton->setObjectName(QStringLiteral("engineCancelRequestButton"));
    commandRow->addWidget(impl.cancelButton);
    connect(impl.cancelButton, &QPushButton::clicked, this, [this] {
        if (auto result = cancelRequest(); !result)
            m_impl->setStatus(qs(result.error().message()));
    });
    commandRow->addStretch(1);
    layout->addLayout(commandRow);
    impl.indicator = new ui::EngineConnectionIndicator(panel);
    layout->addWidget(impl.indicator);
    impl.status = new QLabel(panel);
    impl.status->setWordWrap(true);
    impl.status->setTextFormat(Qt::PlainText);
    impl.status->setObjectName(QStringLiteral("engineStatusLabel"));
    layout->addWidget(impl.status);
    impl.console = new ui::EngineConsole(panel);
    layout->addWidget(impl.console, 1);
    impl.console->setCommandHandler([this](const QString& method, const QString& params) {
        auto parsed = lang::parseJson(params.toUtf8().toStdString());
        if (!parsed || !parsed->isObject()) {
            m_impl->setStatus(QStringLiteral("Command parameters must be a JSON object"));
            return;
        }
        if (auto sent = command(method.toUtf8().toStdString(), std::move(*parsed)); !sent)
            m_impl->setStatus(qs(sent.error().message()));
    });
    connect(impl.console, &ui::EngineConsole::openLocation, this, [this](const QString& path, int line, int column) {
        if (!m_impl->admitted(m_impl->epoch, m_impl->root, m_impl->trustRevision))
            return;
        // Revalidate physical containment again on activation, not only at event arrival.
        m_impl->openLocation(path.toUtf8().toStdString(), line, column);
    });
    impl.projectView = new ui::EngineView(workbench.m_window);
    impl.projectView->hide();
    connect(impl.projectView, &ui::EngineView::refreshRequested, this, [this] { m_impl->refreshProjectContent(); });
    connect(impl.projectView, &ui::EngineView::openAsset, this, [this](const QString& path, const QString& handle) {
        if (!m_impl->projectContent)
            return;
        const std::string relative = path.toUtf8().toStdString();
        const auto found = std::ranges::find(m_impl->projectContent->assets, relative, &engine::ProjectAsset::path);
        if (found != m_impl->projectContent->assets.end() &&
            std::to_string(found->handle.value) == handle.toUtf8().toStdString())
            m_impl->openAsset(relative);
    });
    for (const auto& [id, name, method] : {std::tuple{"engine.play", "Play", "play.start"},
                                           {"engine.pause", "Pause", "play.pause"},
                                           {"engine.step", "Step", "play.step"},
                                           {"engine.stop", "Stop", "play.stop"},
                                           {"engine.simulate", "Simulate", "play.simulate"}}) {
        input::ActionSpec action;
        action.id = id;
        action.name = name;
        action.group = "FrostEngine";
        action.context.none = input::ContextKeys{input::ContextKey::DialogFocus};
        const QPointer<EngineIntegration> self(this);
        action.handler = [self, method](const input::ActionContext&, const input::InvokeInfo&) {
            if (!self)
                return;
            if (std::string_view(method) == "play.stop" && self->m_impl->nativeDebug &&
                self->m_impl->nativeDebug->paused()) {
                auto stopped = self->m_impl->nativeDebug->terminate([self](Result<void> result) {
                    if (self)
                        self->m_impl->setStatus(result ? QStringLiteral("The debugger accepted the termination request")
                                                       : qs(result.error().message()));
                });
                if (!stopped)
                    self->m_impl->setStatus(qs(stopped.error().message()));
                return;
            }
            auto params = lang::Json::object();
            std::string actualMethod(method);
            if (std::string_view(method) == "play.start" && self->m_impl->state && self->m_impl->state->paused) {
                actualMethod = "play.pause";
                params.set("paused", false);
            } else if (std::string_view(method) == "play.pause") {
                params.set("paused", true);
            }
            if (std::string_view(method) == "play.step")
                params.set("frames", self->m_impl->workbench.m_window->mainToolbar().engineStepFrames());
            if (auto result = self->command(std::move(actualMethod), std::move(params)); !result)
                self->m_impl->setStatus(qs(result.error().message()));
        };
        action.enabled = [self, method](const input::ActionContext&) {
            if (!self)
                return false;
            auto& current = *self->m_impl;
            if (!current.admitted(current.epoch, current.root, current.trustRevision))
                return false;
            if (std::string_view(method) == "play.stop" && current.nativeDebug && current.nativeDebug->paused())
                return current.nativeDebug->canTerminate();
            if (!current.session || !current.state)
                return false;
            std::scoped_lock lock(current.policy->mutex);
            const std::string_view actualMethod = std::string_view(method) == "play.start" && current.state->paused
                                                      ? std::string_view("play.pause")
                                                      : std::string_view(method);
            auto context = current.policy->context;
            if (current.nativeDebug && current.nativeDebug->paused())
                context.state = "DebuggerPaused";
            return current.policy->policy &&
                   current.policy->policy->admit(actualMethod, lang::Json::object(), context).has_value();
        };
        if (auto added = workbench.m_registry.add(std::move(action)); !added)
            log::warn("engine", "Cannot register engine action: {}", added.error().describe());
    }
    impl.startupWatch = new QTimer(this);
    impl.startupWatch->setInterval(100);
    connect(impl.startupWatch, &QTimer::timeout, this, [this] { m_impl->checkOwnedStartup(); });
    connect(impl.picker, &QComboBox::currentIndexChanged, this, [this] { m_impl->updateControls(); });
    connect(&workbench, &Workbench::projectChanged, this, [this](const QString&) { m_impl->projectChanged(); });
    if (workbench.m_debug) {
        impl.nativeDebug = std::make_unique<EngineNativeDebugAdapter>(
            *workbench.m_debug,
            [this]() -> std::optional<EngineNativeDebugTarget> {
                auto& current = *m_impl;
                if (!current.nativeTarget || !current.admitted(current.epoch, current.root, current.trustRevision))
                    return std::nullopt;
                if (current.identity && current.session && current.session->connected())
                    return current.nativeTarget;
                // A native all-stop can suspend the heartbeat thread. Retain only the DAP session
                // that was already stopped on this authenticated PID when transport was lost.
                if (!current.retainedDebugGeneration || !current.workbench.m_debug ||
                    current.workbench.m_debug->sessionGeneration() != *current.retainedDebugGeneration ||
                    !current.workbench.m_debug->nativeSession())
                    return std::nullopt;
                const auto* debugSession = current.workbench.m_debug->session();
                if (!debugSession || debugSession->processId() != current.nativeTarget->pid ||
                    (debugSession->state() != debug::DebugState::Stopped &&
                     debugSession->state() != debug::DebugState::Running))
                    return std::nullopt;
                return current.nativeTarget;
            },
            [this] {
                if (!m_impl->stopped)
                    m_impl->updateControls();
            },
            this);
    }
    impl.projectChanged();
}

EngineIntegration::~EngineIntegration() {
    shutdown();
    if (!m_impl->registered)
        delete m_impl->panel.data();
    if (!m_impl->projectRegistered)
        delete m_impl->projectView.data();
}
void EngineIntegration::cancel() { m_impl->invalidate(true); }
void EngineIntegration::trustChanged() {
    auto& impl = *m_impl;
    const auto trust = impl.workbench.m_services.trust.snapshot(impl.root);
    if (!impl.physicalAdmission->load() && impl.available) {
        impl.invalidate(true);
        impl.physicalAdmission->store(false);
        impl.available = false;
        impl.setStatus(QStringLiteral("The project's physical directory changed; reopen it before using engine tools"));
        impl.projectView->setUnavailable(QStringLiteral("The selected physical project is no longer available"));
        impl.updateControls();
    } else if (trust.revision != impl.trustRevision) {
        impl.projectChanged();
    }
}
void EngineIntegration::shutdown() {
    auto& impl = *m_impl;
    if (impl.stopped)
        return;
    impl.invalidate(true);
    impl.stopped = true;
    if (impl.console)
        impl.console->setCommandHandler({});
    impl.policy->active.store(false);
    for (auto& task : impl.tasks)
        task.join();
    impl.tasks.clear();
}
Result<void> EngineIntegration::refreshInstances() { return m_impl->refresh(); }
void EngineIntegration::disconnect() { m_impl->clearSession(false); }
Result<std::uint64_t> EngineIntegration::command(std::string method, lang::Json params) {
    return m_impl->send(std::move(method), std::move(params));
}
std::vector<run::RunConfiguration> EngineIntegration::configurations() const { return m_impl->generated(); }

} // namespace frost::app
