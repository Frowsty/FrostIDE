// SPDX-License-Identifier: Apache-2.0
//
// DebugController (see frost/ui/debug_controller.hpp): the relay from the session's threads to the
// GUI thread, the Debug run hand-off, stepping, the execution point and the breakpoint gutters.

#include "frost/ui/debug_controller.hpp"

#include "frost/core/log.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/run_console.hpp"

#include <QMetaObject>
#include <QToolButton>

#include <algorithm>
#include <climits>
#include <format>
#include <limits>
#include <utility>

namespace frost::ui {

// Shared with the sessions' and the run service's callbacks: posts to the controller while it lives.
struct DebugController::Relay {
    Mutex mutex;
    DebugController* target FROST_GUARDED_BY(mutex) = nullptr;

    // Calls function(controller) on the controller's thread, queued, when session `generation` is
    // still the controller's; ~QObject drops what is still queued.
    template<typename F>
    void post(std::uint64_t generation, F&& function) {
        LockGuard lock(mutex);
        if (target != nullptr) {
            DebugController* self = target;
            QMetaObject::invokeMethod(
                self,
                [self, generation, call = std::forward<F>(function)]() mutable {
                    if (self->m_generation == generation) {
                        call(self);
                    }
                },
                Qt::QueuedConnection);
        }
    }
};

// One attached editor.
struct DebugController::Attached {
    QPointer<EditorView> view;
    std::string path; // Normalised.
    std::unique_ptr<debug::DocumentBreakpointTracker> tracker;
    std::vector<QMetaObject::Connection> connections;
};

debug::DebugTarget debugTargetFor(const build::RunOutcome& outcome, const build::DebugLaunch& launch) {
    debug::DebugTarget target;
    target.name = std::format("{} [{}]", outcome.run.projectName, outcome.run.configuration.name());
    target.program = launch.command.executable;
    target.arguments = build::splitWindowsArguments(launch.command.arguments);
    target.workingDirectory = launch.command.workingDirectory;
    target.environment = launch.command.environment;
    target.inheritEnvironment = launch.command.mergeEnvironment;
    target.trustRoots = launch.trustRoots;
    return target;
}

DebugController::DebugController(debug::BreakpointManager& breakpoints, DebugControllerOptions options,
                                 DebugControllerHooks hooks, QObject* parent)
    : QObject(parent), m_breakpoints(breakpoints), m_options(std::move(options)), m_hooks(std::move(hooks)),
      m_relay(std::make_shared<Relay>()) {
    {
        LockGuard lock(m_relay->mutex);
        m_relay->target = this;
    }
    m_rerunTimer.setInterval(50);
    connect(&m_rerunTimer, &QTimer::timeout, this, [this] {
        if (!m_rerunPending || m_shutdown) {
            m_rerunTimer.stop();
            return;
        }
        if (debugging() || (m_hooks.mayStart && !m_hooks.mayStart()))
            return;
        auto started = debug();
        if (!started && started.error().code() == ErrorCode::Busy)
            return;
        m_rerunPending = false;
        m_rerunTimer.stop();
        if (!started)
            report(std::unexpected(started.error()), QStringLiteral("Debug"));
    });
    m_killTimer.setSingleShot(true);
    m_killTimer.setInterval(kStopTimeout);
    connect(&m_killTimer, &QTimer::timeout, this, [this] {
        if (m_session) {
            log::warn("debug", "lldb-dap did not end the session in time; ending its process tree");
            m_session->kill();
        }
    });
    m_listener = m_breakpoints.subscribe(
        [this](std::string_view path, debug::BreakpointManager::Change change) { onBreakpointsChanged(path, change); });
}

DebugController::~DebugController() {
    {
        LockGuard lock(m_relay->mutex);
        m_relay->target = nullptr;
    }
    m_breakpoints.unsubscribe(m_listener);
    for (auto& [view, attached] : m_editors) {
        for (const QMetaObject::Connection& connection : attached->connections) {
            disconnect(connection);
        }
    }
    m_editors.clear();
    ++m_generation;
    m_session.reset(); // Closes lldb-dap's tree and joins the client's threads.
}

void DebugController::setConsole(RunConsole* console) {
    if (m_console) {
        disconnect(m_console, nullptr, this, nullptr);
    }
    m_console = console;
    if (m_console) {
        connect(m_console, &RunConsole::stopRequested, this, [this] { stop(); });
        connect(m_console, &RunConsole::rerunRequested, this, [this] { rerun(); });
        connect(m_console, &RunConsole::inputSubmitted, this,
                [this](const QString& line) { submitConsoleInput(line); });
    }
}

RunConsole* DebugController::console() const noexcept { return m_console.data(); }

Result<void> DebugController::setWorkspaceRoot(std::string root) {
    m_root.clear(); // Suppress persistence while the old set is cleared and the new set loaded.
    if (root.empty()) {
        m_breakpoints.clear();
        return {};
    }
    if (m_options.fs == nullptr)
        return {};
    auto loaded = m_breakpoints.load(*m_options.fs, root);
    if (loaded)
        m_root = std::move(root);
    return loaded;
}

void DebugController::save() {
    if (m_root.empty() || m_options.fs == nullptr) {
        return;
    }
    if (auto saved = m_breakpoints.save(*m_options.fs, m_root); !saved) {
        log::warn("debug", "the breakpoints could not be saved: {}", saved.error().message());
    }
}

QString DebugController::statusText() const {
    if (!m_session) {
        return {};
    }
    switch (m_state) {
    case debug::DebugState::Starting: return QStringLiteral("Starting the debugger...");
    case debug::DebugState::Running: return QStringLiteral("Running");
    case debug::DebugState::Stopped:
        return m_stop ? QString::fromStdString(m_stop->summary()) : QStringLiteral("Paused");
    case debug::DebugState::Ended: return {};
    }
    return {};
}

// --- Starting ---

Result<void> DebugController::debug(std::optional<project::ProjectId> project) {
    if (m_shutdown) {
        return fail(ErrorCode::Cancelled, "the workbench is shutting down");
    }
    if ((m_hooks.mayStart && !m_hooks.mayStart()) || m_session || m_pendingRun || m_alternatePending) {
        return fail(ErrorCode::Busy, "a debug session is running already; stop it first");
    }
    if (m_hooks.startOverride) {
        if (std::optional<Result<void>> handled = m_hooks.startOverride()) {
            emit stateChanged();
            return std::move(*handled);
        }
    }
    if (m_hooks.alternateAvailable && m_hooks.alternateAvailable() && m_hooks.startAlternate) {
        const QPointer<DebugController> self(this);
        const std::uint64_t generation = ++m_generation;
        m_alternatePending = true;
        auto started = m_hooks.startAlternate([self, generation](Result<debug::DebugTarget> target) {
            if (!self || self->m_shutdown || self->m_generation != generation || !self->m_alternatePending)
                return;
            self->m_alternatePending = false;
            if (!target) {
                self->report(std::unexpected(target.error()), QStringLiteral("Debug"));
            } else if (auto result = self->startTarget(*target); !result) {
                self->report(std::unexpected(result.error()), QStringLiteral("Debug"));
            }
            emit self->stateChanged();
        });
        if (!started)
            m_alternatePending = false;
        emit stateChanged();
        return started;
    }
    if (m_service == nullptr) {
        return fail(ErrorCode::InvalidArgument, "no solution is open to debug");
    }
    const std::shared_ptr<Relay> relay = m_relay;
    const std::uint64_t generation = m_generation;
    build::DebugRunHandlers handlers{
        .onOutput =
            [relay, generation](build::RunOutputKind kind, std::string_view text) {
                relay->post(generation, [kind, text = std::string(text)](DebugController* self) {
                    if (self->m_console) {
                        self->m_console->appendOutput(kind, text);
                    }
                });
            },
        .onFinished =
            [relay, generation](const build::RunOutcome& outcome, const std::optional<build::DebugLaunch>& launch) {
                relay->post(generation,
                            [outcome, launch](DebugController* self) { self->onRunFinished(outcome, launch); });
            },
    };
    auto started = m_service->debug(project, std::move(handlers));
    if (!started) {
        return std::unexpected(std::move(started.error()));
    }
    m_pendingRun = *started;
    m_lastCoreClrSession = false;
    if (m_console) {
        m_console->beginRun(QStringLiteral("Debug"));
    }
    if (m_hooks.showToolWindow) {
        m_hooks.showToolWindow();
    }
    emit stateChanged();
    return {};
}

void DebugController::onRunFinished(const build::RunOutcome& outcome, const std::optional<build::DebugLaunch>& launch) {
    if (!m_pendingRun || *m_pendingRun != outcome.run.id) {
        return;
    }
    m_pendingRun.reset();
    if (!launch) {
        m_lastOutcome = outcome;
        if (m_console) {
            m_console->finish(outcome);
        }
        if (outcome.error && !outcome.stopped && !outcome.buildFailed && m_hooks.notify) {
            m_hooks.notify(QStringLiteral("Debug"), QString::fromStdString(outcome.error->message()), true);
        }
        emit stateChanged();
        return;
    }
    if (auto started = startTarget(debugTargetFor(outcome, *launch)); !started) {
        m_lastOutcome = build::RunOutcome{.run = outcome.run, .error = started.error()};
        if (m_console) {
            m_console->appendOutput(build::RunOutputKind::System,
                                    std::format("The debugger could not start: {}\n", started.error().message()));
            build::RunOutcome failed;
            failed.error = started.error();
            m_console->finish(failed);
        }
        report(std::unexpected(started.error()), QStringLiteral("Debug"));
        emit stateChanged();
    }
}

Result<debug::AdapterFactory> DebugController::adapterFactory() {
    if (m_options.adapterFactory) {
        return m_options.adapterFactory;
    }
    if (m_options.fs == nullptr || m_options.launcher == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the debugger has no process launcher");
    }
    auto path = debug::findLldbDap(
        *m_options.fs, debug::defaultLldbDapSearch(m_options.lldbDapSetting ? m_options.lldbDapSetting() : ""));
    if (!path) {
        return std::unexpected(std::move(path.error()));
    }
    return debug::lldbDapAdapterFactory(*m_options.launcher, std::move(*path));
}

Result<void> DebugController::startTarget(const debug::DebugTarget& target) {
    if (m_shutdown)
        return fail(ErrorCode::Cancelled, "the workbench is shutting down");
    if (m_session) {
        return fail(ErrorCode::Busy, "a debug session is running already; stop it first");
    }
    if (m_options.trust == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the debugger has no trust store");
    }
    auto resolvedTarget = target;
    if (m_options.sourceMapSetting) {
        const auto [from, to] = m_options.sourceMapSetting();
        if (!from.empty() || !to.empty()) {
            if (!m_options.fs || from.empty() || to.empty())
                return fail(ErrorCode::InvalidArgument, "set both debug source mapping roots");
            auto roots = target.trustRoots;
            if (roots.empty())
                roots.emplace_back(io::parentPath(target.program));
            for (auto& root : roots) {
                auto canonical = m_options.fs->canonicalPath(root);
                if (!canonical || !m_options.trust->isTrusted(root))
                    return fail(ErrorCode::Untrusted, "debug project root is not trusted or does not exist");
                root = std::move(*canonical);
            }
            auto mapping = debug::makeSourceMap(*m_options.fs, *m_options.trust, from, to, roots);
            if (!mapping)
                return std::unexpected(std::move(mapping.error()));
            resolvedTarget.trustRoots = std::move(roots);
            resolvedTarget.sourceMap = std::move(*mapping);
        }
    }
    auto factory = adapterFactory();
    if (!factory)
        return std::unexpected(std::move(factory.error()));
    auto started = startSession(
        resolvedTarget.name.empty() ? std::string(io::fileName(resolvedTarget.program)) : resolvedTarget.name,
        resolvedTarget.program, "lldb-dap",
        [this, resolvedTarget, factory = std::move(*factory)](debug::DebugSessionCallbacks callbacks) mutable {
            return debug::startDebugSession(*m_options.trust, resolvedTarget, std::move(factory),
                                            m_breakpoints.sessionLines(), std::move(callbacks),
                                            m_breakpoints.sessionFunctions(), m_breakpoints.sessionExceptionFilters(),
                                            m_breakpoints.sessionThrowTypes());
        });
    if (started) {
        m_lastCoreClrSession = false;
        m_nativeSession = true;
    }
    return started;
}

Result<void> DebugController::attachTarget(const debug::DebugTarget& target, debug::AdapterFactory factory,
                                           rpc::Json attachArguments, std::function<bool()> ownerAdmission) {
    const bool admitted = ownerAdmission ? ownerAdmission() : (!m_hooks.mayStart || m_hooks.mayStart());
    if (m_pendingRun || m_alternatePending || !admitted)
        return fail(ErrorCode::Busy, "another run or debug launch is already pending");
    const std::string_view typeView =
        attachArguments.isObject() ? attachArguments.stringAt("type") : std::string_view{};
    const std::string adapterId = !typeView.empty() ? std::string(typeView) : std::string("lldb-dap");
    const std::string displayAdapter =
        adapterId == "coreclr" ? "CoreCLR" : (!typeView.empty() ? adapterId : std::string("Lua"));
    auto started = startSession(
        target.name.empty() ? std::string(io::fileName(target.program)) : target.name, target.program, displayAdapter,
        [this, target, factory = std::move(factory), arguments = std::move(attachArguments),
         adapterId](debug::DebugSessionCallbacks callbacks) mutable {
            auto session = debug::attachDebugSession(*m_options.trust, target, std::move(factory), std::move(arguments),
                                                     m_breakpoints.sessionLines(), std::move(callbacks), adapterId);
            if (session && adapterId == "lldb-dap") {
                // Recorded now and sent when the adapter is configured (DebugSession).
                (*session)->setFunctionBreakpoints(m_breakpoints.sessionFunctions());
                (*session)->setExceptionFilters(m_breakpoints.sessionExceptionFilters());
                (*session)->setThrowTypes(m_breakpoints.sessionThrowTypes());
            }
            return session;
        },
        true);
    if (started) {
        m_lastCoreClrSession = (adapterId == "coreclr");
        m_nativeSession = adapterId == "lldb-dap";
    }
    return started;
}

Result<void>
DebugController::attachManagedTarget(const debug::DebugTarget& target, debug::AdapterFactory factory,
                                     std::function<bool(const debug::DebugFrame&)> frameAllowed,
                                     std::vector<std::string> allowedSources,
                                     std::map<std::string, std::vector<debug::BreakpointLine>> verifiedBreakpoints,
                                     std::function<void(std::string, std::function<void(bool)>)> validateBreakpoint,
                                     std::function<bool()> ownerAdmission) {
    const bool admitted = ownerAdmission && ownerAdmission();
    if (m_pendingRun || m_alternatePending || !admitted)
        return fail(ErrorCode::Busy, "another run or debug launch is already pending");
    if (!factory || !frameAllowed || !validateBreakpoint || allowedSources.empty())
        return fail(ErrorCode::InvalidArgument, "verified managed source policy is required");
    if (m_session)
        return fail(ErrorCode::Busy, "a debug session is running already; stop it first");
    for (const auto& [path, lines] : verifiedBreakpoints) {
        (void)lines;
        if (std::ranges::find(allowedSources, path) == allowedSources.end())
            return fail(ErrorCode::InvalidArgument, "breakpoint source is not admitted");
    }
    m_coreClrSession = true;
    m_coreClrFrameAllowed = std::move(frameAllowed);
    m_coreClrAllowedSources = std::move(allowedSources);
    m_coreClrBreakpointValidator = std::move(validateBreakpoint);
    auto started = startSession(
        target.name.empty() ? std::string(io::fileName(target.program)) : target.name, target.program, "Mono",
        [this, target, factory = std::move(factory),
         verifiedBreakpoints = std::move(verifiedBreakpoints)](debug::DebugSessionCallbacks callbacks) mutable {
            return debug::attachDebugSession(*m_options.trust, target, std::move(factory), rpc::Json::object(),
                                             std::move(verifiedBreakpoints), std::move(callbacks), "mono");
        },
        true);
    if (!started) {
        m_coreClrSession = false;
        m_coreClrFrameAllowed = {};
        m_coreClrAllowedSources.clear();
        m_coreClrBreakpointValidator = {};
    } else {
        m_lastCoreClrSession = true;
    }
    return started;
}

Result<void> DebugController::startCoreClrTarget(
    const debug::CoreClrTarget& target, const std::vector<std::string>& trustRoots, debug::AdapterFactory factory,
    std::function<bool(const debug::DebugFrame&)> frameAllowed, std::vector<std::string> allowedSources,
    std::function<void(std::string, std::function<void(bool)>)> validateBreakpoint) {
    if (m_pendingRun || m_alternatePending || (m_hooks.mayStart && !m_hooks.mayStart()))
        return fail(ErrorCode::Busy, "another run or debug launch is already pending");
    if (!factory)
        return fail(ErrorCode::InvalidArgument, "the CoreCLR adapter factory is missing");
    if (m_session)
        return fail(ErrorCode::Busy, "a debug session is running already; stop it first");
    m_coreClrSession = true;
    m_coreClrFrameAllowed = std::move(frameAllowed);
    m_coreClrAllowedSources = allowedSources;
    m_coreClrBreakpointValidator = std::move(validateBreakpoint);
    auto started = startSession(
        std::string(io::fileName(target.assembly)), target.assembly, "CoreCLR",
        [this, target, trustRoots, factory = std::move(factory), allowedSources = std::move(allowedSources),
         restrictSources = static_cast<bool>(m_coreClrFrameAllowed)](debug::DebugSessionCallbacks callbacks) mutable {
            auto breakpoints = m_breakpoints.sessionLines();
            if (restrictSources) {
                std::erase_if(breakpoints, [&allowedSources](const auto& entry) {
                    return std::ranges::find(allowedSources, entry.first) == allowedSources.end();
                });
            }
            return debug::startCoreClrSession(*m_options.trust, target, trustRoots, std::move(factory),
                                              std::move(breakpoints), std::move(callbacks));
        });
    if (!started) {
        m_coreClrSession = false;
        m_coreClrFrameAllowed = {};
        m_coreClrAllowedSources.clear();
        m_coreClrBreakpointValidator = {};
    } else
        m_lastCoreClrSession = true;
    return started;
}

Result<void> DebugController::startSession(std::string title, std::string program, std::string adapterName,
                                           SessionStarter starter, bool attach) {
    if (m_shutdown)
        return fail(ErrorCode::Cancelled, "the workbench is shutting down");
    if (m_session)
        return fail(ErrorCode::Busy, "a debug session is running already; stop it first");
    if (m_options.trust == nullptr)
        return fail(ErrorCode::InvalidArgument, "the debugger has no trust store");
    const std::uint64_t generation = ++m_generation;
    const std::shared_ptr<Relay> relay = m_relay;
    debug::DebugSessionCallbacks callbacks{
        .onStateChanged =
            [relay, generation](debug::DebugState state) {
                relay->post(generation, [state](DebugController* self) { self->onState(state); });
            },
        .onStopped =
            [relay, generation](const debug::StopInfo& stop) {
                relay->post(generation, [stop](DebugController* self) { self->onStopped(stop); });
            },
        .onThreads =
            [relay, generation](const std::vector<debug::DebugThread>& threads) {
                relay->post(generation, [threads](DebugController* self) { self->onThreads(threads); });
            },
        .onFrames =
            [relay, generation](std::int64_t threadId, const debug::FrameList& frames) {
                relay->post(generation,
                            [threadId, frames](DebugController* self) { self->onFrames(threadId, frames); });
            },
        .onFrameSelected =
            [relay, generation](std::int64_t threadId, const debug::DebugFrame& frame) {
                relay->post(generation,
                            [threadId, frame](DebugController* self) { self->onFrameSelected(threadId, frame); });
            },
        .onOutput =
            [relay, generation](std::string_view category, std::string_view text) {
                relay->post(generation, [category = std::string(category), text = std::string(text)](
                                            DebugController* self) { self->onOutput(category, text); });
            },
        .onBreakpoints =
            [relay, generation](const std::vector<debug::BreakpointResult>& results) {
                relay->post(generation, [results](DebugController* self) { self->onBreakpoints(results); });
            },
        .onExited = {},
        .onEnded =
            [relay, generation](const debug::DebugOutcome& outcome) {
                relay->post(generation,
                            [generation, outcome](DebugController* self) { self->onEnded(generation, outcome); });
            },
        .onTerminated =
            [relay, generation] {
                relay->post(generation,
                            [generation](DebugController* self) { emit self->adapterTerminated(generation); });
            },
        .onCapabilities =
            [relay, generation](const debug::dap::Capabilities& capabilities) {
                relay->post(generation, [capabilities](DebugController* self) { self->onCapabilities(capabilities); });
            },
    };
    m_breakpoints.resetStatus(debug::BreakpointStatus::Pending);
    auto session = starter(std::move(callbacks));
    if (!session) {
        ++m_generation;
        m_breakpoints.resetStatus(debug::BreakpointStatus::Unknown);
        return std::unexpected(std::move(session.error()));
    }
    if (const auto outcome = (*session)->outcome(); outcome && outcome->error) {
        ++m_generation; // Its onEnded is queued already; drop it.
        m_breakpoints.resetStatus(debug::BreakpointStatus::Unknown);
        return std::unexpected(*outcome->error);
    }
    m_session = std::move(*session);
    m_rerunAllowed = !attach;
    m_state = debug::DebugState::Starting;
    m_title = QString::fromStdString(title);
    m_stop.reset();
    m_threads.clear();
    m_thread.reset();
    m_frames = {};
    m_frame.reset();
    if (m_console) {
        if (m_console->text().isEmpty()) {
            m_console->beginRun(m_title);
        }
        m_console->appendOutput(build::RunOutputKind::System,
                                attach ? std::format("Attached debugger to {}\n", title)
                                       : std::format("Debugging {} with {}\n", program, adapterName));
        m_console->setRunning(true, true);
        m_console->rerunButton()->setEnabled(!m_coreClrSession && m_rerunAllowed);
    }
    if (m_hooks.showToolWindow) {
        m_hooks.showToolWindow();
    }
    emit stateChanged();
    return {};
}

// --- The session's events ---

void DebugController::onState(debug::DebugState state) {
    if (state == debug::DebugState::Ended) {
        return; // onEnded follows.
    }
    m_state = state;
    if (state == debug::DebugState::Running) {
        m_stop.reset();
        m_frames = {};
        m_frame.reset();
        m_currentFrameVariables.clear();
        m_frameScopeReferences.clear();
        m_hoverCache.clear();
        clearLoadedSource();
        showExecutionPoint();
        emit framesChanged();
    }
    emit stateChanged();
}

void DebugController::onStopped(const debug::StopInfo& stop) {
    clearLoadedSource();
    m_state = debug::DebugState::Stopped;
    m_stop = stop;
    if (stop.threadId) {
        m_thread = stop.threadId;
    }
    const QString summary = QString::fromStdString(stop.summary());
    if (stop.isCrash() && m_hooks.notify) {
        m_hooks.notify(m_title.isEmpty() ? QStringLiteral("Debug") : m_title, summary, true);
    }
    emit stopped(summary, stop.isCrash());
    emit stateChanged();
}

void DebugController::onThreads(std::vector<debug::DebugThread> threads) {
    m_threads = std::move(threads);
    if (!m_thread && !m_threads.empty()) {
        m_thread = m_threads.front().id;
    }
    emit threadsChanged();
}

void DebugController::onFrames(std::int64_t threadId, debug::FrameList frames) {
    if (m_thread && *m_thread != threadId) {
        return;
    }
    m_thread = threadId;
    if (m_coreClrFrameAllowed)
        std::erase_if(frames.frames, [this](const auto& frame) { return !m_coreClrFrameAllowed(frame); });
    m_frames = std::move(frames);
    emit framesChanged();
}

void DebugController::onFrameSelected(std::int64_t threadId, const debug::DebugFrame& frame) {
    m_currentFrameVariables.clear();
    m_frameScopeReferences.clear();
    m_hoverCache.clear();
    if (m_coreClrFrameAllowed && !m_coreClrFrameAllowed(frame)) {
        m_frame.reset();
        showExecutionPoint();
        emit frameSelected();
        return;
    }
    m_thread = threadId;
    m_frame = frame;
    clearLoadedSource();
    if (frame.sourceReference != 0 && !m_coreClrSession) {
        // Never navigate to current disk bytes for an adapter-owned revision.
        const auto generation = m_generation;
        const auto sourceEpoch = m_sourceEpoch;
        const auto frameId = frame.id;
        const auto reference = frame.sourceReference;
        const auto name = frame.sourceName.empty() ? std::string("(loaded source)") : frame.sourceName;
        const auto line = frame.line;
        const std::shared_ptr<Relay> relay = m_relay;
        if (m_session && !m_sourceEpochExhausted) {
            m_session->source(reference, [relay, generation, sourceEpoch, frameId, reference, name,
                                          line](Result<std::string> source) {
                relay->post(generation, [sourceEpoch, frameId, reference, name, line,
                                         source = std::move(source)](DebugController* self) {
                    if (self->m_sourceEpochExhausted || self->m_sourceEpoch != sourceEpoch ||
                        self->m_state != debug::DebugState::Stopped || !self->m_frame || self->m_frame->id != frameId ||
                        self->m_frame->sourceReference != reference)
                        return;
                    if (!source) {
                        if (self->m_hooks.notify)
                            self->m_hooks.notify(
                                self->m_title.isEmpty() ? QStringLiteral("Debug") : self->m_title,
                                QStringLiteral(
                                    "The running source version cannot yet be verified; navigation is disabled."),
                                false);
                        return;
                    }
                    emit self->loadedSourceChanged(
                        QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())),
                        QString::fromUtf8(source->data(), static_cast<qsizetype>(source->size())),
                        static_cast<int>(std::min<std::int64_t>(line, INT_MAX)));
                });
            });
        }
    } else if (frame.hasSource() && m_hooks.openSource) {
        if (EditorView* view = m_hooks.openSource(QString::fromStdString(frame.path), static_cast<int>(frame.line))) {
            attachEditor(view);
        }
    }
    showExecutionPoint();
    emit frameSelected();
}

void DebugController::onOutput(const std::string& category, const std::string& text) {
    if (!m_console) {
        return;
    }
    const build::RunOutputKind kind = category == "stdout"   ? build::RunOutputKind::Stdout
                                      : category == "stderr" ? build::RunOutputKind::Stderr
                                                             : build::RunOutputKind::System;
    m_console->appendOutput(kind, text);
}

void DebugController::onBreakpoints(const std::vector<debug::BreakpointResult>& results) {
    for (const debug::BreakpointResult& result : results) {
        m_breakpoints.setStatus(result.id, result.verified, result.message);
    }
}

void DebugController::onEnded(std::uint64_t endingGeneration, const debug::DebugOutcome& outcome) {
    build::RunOutcome finished;
    finished.exitCode = outcome.exitCode;
    finished.stopped = outcome.stopped;
    if (outcome.error && !outcome.exitCode) {
        finished.error = outcome.error;
    }
    if (!finished.exitCode && !finished.error && !finished.stopped) {
        finished.error = Error(ErrorCode::IoError, "the debug session ended");
    }
    if (m_generation == endingGeneration)
        m_lastOutcome = finished;
    const QString summary = outcome.error ? QStringLiteral("Debug session ended with an error")
                            : finished.stopped && !finished.exitCode ? QStringLiteral("Process stopped")
                                                                     : runFinishedLine(finished);
    if (m_console) {
        m_console->finish(finished, summary);
        if (m_coreClrSession || !m_rerunAllowed)
            m_console->rerunButton()->setEnabled(false);
    }
    if (outcome.error && m_hooks.notify) {
        m_hooks.notify(QStringLiteral("Debug"), QString::fromStdString(outcome.error->message()), true);
    }
    const bool endedHere = m_generation == endingGeneration;
    if (endedHere)
        endSession();
    // The legacy untagged summary must not overwrite a reentrantly started session's UI.
    if (endedHere && m_generation == endingGeneration + 1)
        emit sessionEnded(summary);
}

void DebugController::endSession() {
    m_killTimer.stop();
    ++m_generation;
    m_state = debug::DebugState::Ended;
    m_stop.reset();
    m_threads.clear();
    m_thread.reset();
    m_frames = {};
    m_frame.reset();
    m_currentFrameVariables.clear();
    m_frameScopeReferences.clear();
    m_hoverCache.clear();
    m_coreClrFrameAllowed = {};
    m_coreClrAllowedSources.clear();
    m_coreClrBreakpointValidator = {};
    m_coreClrPathRequests.clear();
    ++m_coreClrValidationEpoch;
    m_coreClrSession = false;
    m_nativeSession = false;
    clearLoadedSource();
    std::unique_ptr<debug::DebugSession> ended = std::move(m_session);
    ended.reset(); // Joins the client's threads (the adapter has exited or was killed).
    m_breakpoints.resetStatus(debug::BreakpointStatus::Unknown);
    showExecutionPoint();
    emit threadsChanged();
    emit framesChanged();
    emit stateChanged();
}

// --- Commands ---

bool DebugController::report(Result<void> result, const QString& what) {
    if (result) {
        return true;
    }
    log::warn("debug", "{}: {}", what.toStdString(), result.error().message());
    if (m_hooks.notify) {
        m_hooks.notify(what, QString::fromStdString(result.error().message()), true);
    }
    return false;
}

bool DebugController::resume() {
    const bool resumed = m_session && report(m_session->continueRun(), QStringLiteral("Resume"));
    if (resumed)
        clearLoadedSource();
    return resumed;
}

bool DebugController::pause() { return m_session && report(m_session->pause(), QStringLiteral("Pause")); }

bool DebugController::stepOver() {
    const bool resumed = m_session && report(m_session->next(), QStringLiteral("Step Over"));
    if (resumed)
        clearLoadedSource();
    return resumed;
}

bool DebugController::stepInto() {
    const bool resumed = m_session && report(m_session->stepIn(), QStringLiteral("Step Into"));
    if (resumed)
        clearLoadedSource();
    return resumed;
}

bool DebugController::stepOut() {
    const bool resumed = m_session && report(m_session->stepOut(), QStringLiteral("Step Out"));
    if (resumed)
        clearLoadedSource();
    return resumed;
}

bool DebugController::validateCoreClrPath(std::string path, std::function<void(DebugController&)> approved) {
    if (!m_coreClrBreakpointValidator ||
        std::ranges::find(m_coreClrAllowedSources, path) == m_coreClrAllowedSources.end())
        return false;
    const auto sequence = ++m_coreClrPathRequests[path];
    const auto epoch = m_coreClrValidationEpoch;
    const auto generation = m_generation;
    const auto relay = m_relay;
    m_coreClrBreakpointValidator(
        path, [relay, generation, epoch, sequence, path, approved = std::move(approved)](bool ok) {
            relay->post(generation, [ok, epoch, sequence, path, approved](DebugController* self) {
                const auto it = self->m_coreClrPathRequests.find(path);
                if (ok && self->m_session && self->m_coreClrSession && self->m_coreClrValidationEpoch == epoch &&
                    it != self->m_coreClrPathRequests.end() && it->second == sequence)
                    approved(*self);
            });
        });
    return true;
}

bool DebugController::runToCursor() {
    EditorView* view = m_hooks.currentEditor ? m_hooks.currentEditor() : nullptr;
    if (!m_session || view == nullptr) {
        return false;
    }
    const LineIndex line = view->editor().text().lineAt(view->editor().state().primary().head);
    auto path = io::normalizePath(view->path());
    if (!path) {
        return false;
    }
    if (m_coreClrSession) {
        const auto targetLine = static_cast<std::int64_t>(line.value()) + 1;
        return validateCoreClrPath(*path, [path = *path, targetLine](DebugController& self) {
            static_cast<void>(
                self.report(self.m_session->runToLine(path, targetLine), QStringLiteral("Run to Cursor")));
        });
    }
    const bool resumed = report(m_session->runToLine(*path, static_cast<std::int64_t>(line.value()) + 1),
                                QStringLiteral("Run to Cursor"));
    if (resumed)
        clearLoadedSource();
    return resumed;
}

bool DebugController::setNextStatement() {
    EditorView* view = m_hooks.currentEditor ? m_hooks.currentEditor() : nullptr;
    if (!m_session || !paused() || view == nullptr) {
        return false;
    }
    const LineIndex line = view->editor().text().lineAt(view->editor().state().primary().head);
    auto path = io::normalizePath(view->path());
    if (!path) {
        return false;
    }
    const std::int64_t threadId = m_thread.value_or(m_threads.empty() ? 1 : m_threads.front().id);
    const std::int64_t targetLine = static_cast<std::int64_t>(line.value()) + 1;
    const std::shared_ptr<Relay> relay = m_relay;
    const std::uint64_t generation = m_generation;
    m_session->gotoLine(threadId, *path, targetLine, [relay, generation](Result<void> res) {
        relay->post(generation, [res = std::move(res)](DebugController* self) mutable {
            static_cast<void>(self->report(std::move(res), QStringLiteral("Set Next Statement")));
        });
    });
    return true;
}

void DebugController::shutdown() {
    if (m_shutdown)
        return;
    m_shutdown = true;
    m_rerunPending = false;
    m_rerunTimer.stop();
    ++m_generation;
    if (m_pendingRun && m_service)
        m_service->stop(*m_pendingRun);
    m_pendingRun.reset();
    if (m_alternatePending && m_hooks.stopAlternate)
        m_hooks.stopAlternate();
    m_alternatePending = false;
    m_killTimer.stop();
    m_session.reset();
    m_state = debug::DebugState::Ended;
    m_stop.reset();
    m_frame.reset();
    m_currentFrameVariables.clear();
    m_frameScopeReferences.clear();
    m_hoverCache.clear();
    clearLoadedSource();
    showExecutionPoint();
    emit stateChanged();
}

void DebugController::cancelRerun() {
    m_rerunPending = false;
    m_rerunTimer.stop();
}

void DebugController::rerun() {
    if (m_coreClrSession || m_lastCoreClrSession) {
        if (m_hooks.notify)
            m_hooks.notify(QStringLiteral("Debug .NET"), QStringLiteral("Start the .NET project again to rebuild it."),
                           false);
        return;
    }
    if (!m_rerunAllowed) {
        if (m_hooks.notify)
            m_hooks.notify(QStringLiteral("Debug"), QStringLiteral("Rerun is not available for an attached session"),
                           false);
        return;
    }
    stop();
    m_rerunPending = true;
    m_rerunTimer.start();
}

void DebugController::stop() {
    ++m_coreClrValidationEpoch;
    m_rerunPending = false;
    m_rerunTimer.stop();
    if (m_alternatePending) {
        ++m_generation;
        m_alternatePending = false;
        if (m_hooks.stopAlternate)
            m_hooks.stopAlternate();
        emit stateChanged();
    }
    if (m_pendingRun) {
        const auto pending = *m_pendingRun;
        m_pendingRun.reset();
        ++m_generation;
        if (m_service)
            m_service->stop(pending);
        emit stateChanged();
    }
    if (!m_session) {
        return;
    }
    m_session->stop();
    if (!m_killTimer.isActive()) {
        m_killTimer.start();
    }
}

void DebugController::selectThread(std::int64_t threadId) {
    if (!m_session) {
        return;
    }
    m_thread = threadId;
    m_frames = m_session->frames(threadId);
    if (m_coreClrFrameAllowed)
        std::erase_if(m_frames.frames, [this](const auto& frame) { return !m_coreClrFrameAllowed(frame); });
    m_frame.reset();
    emit framesChanged();
    static_cast<void>(report(m_session->selectThread(threadId), QStringLiteral("Select Thread")));
}

void DebugController::selectFrame(std::int64_t frameId) {
    if (!m_session || !m_thread) {
        return;
    }
    static_cast<void>(report(m_session->selectFrame(*m_thread, frameId), QStringLiteral("Select Frame")));
}

void DebugController::loadMoreFrames() {
    if (m_session && m_thread) {
        m_session->loadMoreFrames(*m_thread);
    }
}

void DebugController::scopes(std::int64_t frameId, ScopesDone done) {
    if (!m_session) {
        return;
    }
    const std::shared_ptr<Relay> relay = m_relay;
    const std::uint64_t generation = m_generation;
    m_session->scopes(frameId, [relay, generation, frameId, done = std::move(done)](Result<debug::ScopeList> list) {
        relay->post(generation, [done, frameId, list = std::move(list)](DebugController* self) {
            if (list && self->m_frame && self->m_frame->id == frameId) {
                // Inline values and hovers use these scopes' own variables only (refreshInlineDebuggerValues).
                self->m_frameScopeReferences.clear();
                for (const debug::dap::Scope& scope : *list) {
                    if (!scope.expensive && scope.variablesReference > 0) {
                        self->m_frameScopeReferences.push_back(scope.variablesReference);
                    }
                }
            }
            done(list);
        });
    });
}

void DebugController::variables(std::int64_t reference, std::int64_t start, std::int64_t count, VariablesDone done) {
    if (!m_session) {
        return;
    }
    const std::shared_ptr<Relay> relay = m_relay;
    const std::uint64_t generation = m_generation;
    m_session->variables(
        reference, start, count,
        [relay, generation, reference, done = std::move(done)](Result<debug::VariableList> list) {
            relay->post(generation, [done, reference, list = std::move(list)](DebugController* self) {
                if (list && self->paused() && std::ranges::contains(self->m_frameScopeReferences, reference)) {
                    for (const debug::dap::Variable& var : *list) {
                        if (var.name.empty() || var.value.empty()) {
                            continue;
                        }
                        const auto existing =
                            std::ranges::find(self->m_currentFrameVariables, var.name, &debug::dap::Variable::name);
                        if (existing == self->m_currentFrameVariables.end()) {
                            self->m_currentFrameVariables.push_back(var);
                        } else {
                            *existing = var;
                        }
                    }
                    self->refreshInlineDebuggerValues();
                }
                done(list);
            });
        });
}

void DebugController::evaluate(const QString& expression, const QString& context, EvaluateDone done) {
    if (!m_session) {
        return;
    }
    const std::shared_ptr<Relay> relay = m_relay;
    const std::uint64_t generation = m_generation;
    const std::optional<std::int64_t> frame = m_frame ? std::optional<std::int64_t>(m_frame->id) : std::nullopt;
    m_session->evaluate(expression.toStdString(), frame, context.toStdString(),
                        [relay, generation, done = std::move(done)](Result<debug::dap::Variable> result) {
                            relay->post(generation,
                                        [done, result = std::move(result)](DebugController*) { done(result); });
                        });
}

void DebugController::submitConsoleInput(const QString& line) {
    if (!m_session) {
        return;
    }
    const QString trimmed = line.trimmed();
    if (m_console) {
        m_console->appendOutput(build::RunOutputKind::System, std::format("> {}\n", trimmed.toStdString()));
    }
    if (trimmed.isEmpty()) {
        return;
    }
    evaluate(trimmed, QStringLiteral("repl"), [this](Result<debug::dap::Variable> res) {
        if (!m_console) {
            return;
        }
        if (res) {
            if (!res->value.empty()) {
                m_console->appendOutput(build::RunOutputKind::Stdout, res->value + "\n");
            }
        } else {
            m_console->appendOutput(build::RunOutputKind::Stderr, res.error().message() + "\n");
        }
    });
}

void DebugController::setVariableValue(std::int64_t variablesReference, const QString& name, const QString& value,
                                       EvaluateDone done) {
    if (!m_session) {
        return;
    }
    const std::shared_ptr<Relay> relay = m_relay;
    const std::uint64_t generation = m_generation;
    m_session->setVariable(
        variablesReference, name.toStdString(), value.toStdString(),
        [relay, generation, done = std::move(done)](Result<debug::dap::Variable> res) {
            relay->post(generation, [done, res = std::move(res)](DebugController* self) mutable {
                if (!res) {
                    static_cast<void>(self->report(std::unexpected(res.error()), QStringLiteral("Set Variable Value")));
                }
                if (done) {
                    done(std::move(res));
                }
            });
        });
}

bool DebugController::setExceptionFilterEnabled(std::string_view filter, bool enabled) {
    return m_breakpoints.setExceptionFilterEnabled(filter, enabled);
}

Result<debug::BreakpointId> DebugController::addFunctionBreakpoint(const QString& name) {
    return m_breakpoints.addFunction(name.trimmed().toStdString());
}

void DebugController::onCapabilities(const debug::dap::Capabilities& capabilities) {
    if (!m_session || !m_nativeSession) {
        return; // The list is lldb-dap's (debug::BreakpointManager, "Function breakpoints and exception filters").
    }
    const bool msvc = m_session->msvcExceptions();
    m_breakpoints.setAvailableExceptionFilters(capabilities.exceptionBreakpointFilters,
                                               [msvc](std::string_view filter) {
                                                   if (!msvc) {
                                                       return std::string();
                                                   }
                                                   return debug::msvcUnsupportedFilter(filter).value_or(std::string());
                                               });
}

void DebugController::disassemble(const QString& memoryReference, int instructionCount, DisassembleDone done) {
    if (!m_session) {
        return;
    }
    const std::shared_ptr<Relay> relay = m_relay;
    const std::uint64_t generation = m_generation;
    m_session->disassemble(
        memoryReference.trimmed().toStdString(), instructionCount,
        [relay, generation, done = std::move(done)](Result<std::vector<debug::dap::DisassembledInstruction>> res) {
            relay->post(generation, [done, res = std::move(res)](DebugController*) mutable {
                if (done) {
                    done(std::move(res));
                }
            });
        });
}

void DebugController::readMemory(const QString& memoryReference, int count, ReadMemoryDone done) {
    if (!m_session) {
        return;
    }
    const std::shared_ptr<Relay> relay = m_relay;
    const std::uint64_t generation = m_generation;
    m_session->readMemory(memoryReference.trimmed().toStdString(), count,
                          [relay, generation, done = std::move(done)](Result<debug::dap::ReadMemoryResult> res) {
                              relay->post(generation, [done, res = std::move(res)](DebugController*) mutable {
                                  if (done) {
                                      done(std::move(res));
                                  }
                              });
                          });
}

// --- Breakpoints ---

void DebugController::toggleBreakpoint(const std::string& path, int line) {
    auto normal = io::normalizePath(path);
    if (!normal) {
        return;
    }
    if (auto toggled = m_breakpoints.toggle(*normal, static_cast<std::int64_t>(line) + 1); !toggled) {
        log::warn("debug", "Toggle Breakpoint: {}", toggled.error().message());
    }
}

bool DebugController::toggleBreakpointAtCaret() {
    EditorView* view = m_hooks.currentEditor ? m_hooks.currentEditor() : nullptr;
    if (view == nullptr) {
        return false;
    }
    attachEditor(view);
    if (const auto it = m_editors.find(view); it != m_editors.end() && it->second->tracker) {
        it->second->tracker->sync();
    }
    const LineIndex line = view->editor().text().lineAt(view->editor().state().primary().head);
    toggleBreakpoint(view->path(), static_cast<int>(line.value()));
    return true;
}

void DebugController::setMutedBreakpoints(bool muted) { m_breakpoints.setMuted(muted); }

void DebugController::toggleMuteBreakpoints() { m_breakpoints.setMuted(!m_breakpoints.muted()); }

void DebugController::requestEditBreakpoint() {
    debug::BreakpointId targetId = 0;
    if (EditorView* view = m_hooks.currentEditor ? m_hooks.currentEditor() : nullptr) {
        attachEditor(view);
        if (const auto it = m_editors.find(view); it != m_editors.end() && it->second->tracker) {
            it->second->tracker->sync();
        }
        const std::int64_t line =
            static_cast<std::int64_t>(view->editor().text().lineAt(view->editor().state().primary().head).value()) + 1;
        if (auto norm = io::normalizePath(view->path())) {
            if (auto existing = m_breakpoints.at(*norm, line)) {
                targetId = existing->id;
            } else if (auto added = m_breakpoints.add(*norm, line)) {
                targetId = *added;
            }
        }
    }
    if (m_hooks.showToolWindow) {
        m_hooks.showToolWindow();
    }
    emit editBreakpointRequested(targetId);
}

void DebugController::requestEvaluate() {
    QString expression;
    if (EditorView* view = m_hooks.currentEditor ? m_hooks.currentEditor() : nullptr) {
        const text::Selection primary = view->editor().state().primary();
        if (!primary.empty() && primary.range().size() <= 4096) {
            expression = QString::fromStdString(view->editor().text().text(primary.range())).trimmed();
        }
    }
    emit evaluateRequested(expression);
}

void DebugController::attachEditor(EditorView* view) {
    if (view == nullptr || m_editors.contains(view)) {
        return;
    }
    auto path = io::normalizePath(view->path());
    if (!path) {
        return; // An untitled buffer has no breakpoints.
    }
    auto attached = std::make_unique<Attached>();
    attached->view = view;
    attached->path = *path;
    attached->tracker = std::make_unique<debug::DocumentBreakpointTracker>(m_breakpoints, view->document(), *path);
    Attached* raw = attached.get();
    attached->connections.push_back(connect(view, &EditorView::breakpointToggleRequested, this, [this, raw](int line) {
        if (raw->tracker) {
            raw->tracker->sync();
        }
        toggleBreakpoint(raw->path, line);
    }));
    attached->connections.push_back(connect(view, &EditorView::textChanged, this, [raw] {
        if (raw->tracker) {
            raw->tracker->sync();
        }
    }));
    attached->connections.push_back(connect(view, &QObject::destroyed, this, [this, view] { m_editors.erase(view); }));
    view->setBreakpointGutterEnabled(true);
    view->setDebugHoverProvider([this](std::string_view expr, LineIndex line) { return debugHoverFor(expr, line); });
    m_editors.emplace(view, std::move(attached));
    refreshMarkers(*view);
    showExecutionPoint();
}

void DebugController::detachEditor(EditorView* view) {
    const auto it = m_editors.find(view);
    if (it == m_editors.end()) {
        return;
    }
    for (const QMetaObject::Connection& connection : it->second->connections) {
        disconnect(connection);
    }
    if (it->second->view) {
        it->second->view->setBreakpointMarkers({});
        it->second->view->setExecutionPoint(std::nullopt);
        it->second->view->setInlineDebuggerValues({});
        it->second->view->setDebugHoverProvider({});
        it->second->view->setBreakpointGutterEnabled(false);
    }
    m_editors.erase(it);
}

void DebugController::refreshMarkers(EditorView& view) {
    const auto it = m_editors.find(&view);
    if (it == m_editors.end()) {
        return;
    }
    std::vector<BreakpointMarker> markers;
    for (const debug::LineBreakpoint& breakpoint : m_breakpoints.inFile(it->second->path)) {
        const bool unverified = breakpoint.status == debug::BreakpointStatus::Unverified;
        markers.push_back(BreakpointMarker{
            .line = LineIndex(static_cast<std::uint32_t>(breakpoint.line - 1)),
            .kind = unverified ? BreakpointKind::Unverified : BreakpointKind::Verified,
            .message = unverified ? (breakpoint.message.empty() ? std::string("The breakpoint is not bound to code")
                                                                : breakpoint.message)
                                  : std::string()});
    }
    view.setBreakpointMarkers(std::move(markers));
}

void DebugController::onBreakpointsChanged(std::string_view path, debug::BreakpointManager::Change change) {
    for (auto& [view, attached] : m_editors) {
        if (attached->view && m_breakpoints.samePath(attached->path, path)) {
            refreshMarkers(*attached->view);
        }
    }
    if (change == debug::BreakpointManager::Change::Lines) {
        if (m_session && m_coreClrSession) {
            const std::string file(path);
            static_cast<void>(validateCoreClrPath(file, [file](DebugController& self) {
                self.m_session->setBreakpoints(file, self.m_breakpoints.sessionLines(file));
            }));
        } else if (m_session) {
            m_session->setBreakpoints(std::string(path), m_breakpoints.sessionLines(path));
        }
        save();
    } else if (change == debug::BreakpointManager::Change::Functions) {
        if (m_session && m_nativeSession) {
            m_session->setFunctionBreakpoints(m_breakpoints.sessionFunctions());
        }
        save();
    } else if (change == debug::BreakpointManager::Change::Exceptions) {
        if (m_session && m_nativeSession) {
            m_session->setExceptionFilters(m_breakpoints.sessionExceptionFilters());
            m_session->setThrowTypes(m_breakpoints.sessionThrowTypes());
        }
        save();
    }
    emit breakpointsChanged();
}

void DebugController::clearLoadedSource() {
    if (m_sourceEpoch == std::numeric_limits<std::uint64_t>::max())
        m_sourceEpochExhausted = true;
    else
        ++m_sourceEpoch;
    emit loadedSourceChanged({}, {}, 0);
}

void DebugController::showExecutionPoint() {
    for (auto& [view, attached] : m_editors) {
        if (!attached->view) {
            continue;
        }
        if (!m_coreClrFrameAllowed && m_frame && (m_coreClrSession || m_frame->sourceReference == 0) &&
            m_frame->hasSource() && m_breakpoints.samePath(attached->path, m_frame->path)) {
            attached->view->setExecutionPoint(LineIndex(static_cast<std::uint32_t>(m_frame->line - 1)));
        } else {
            attached->view->setExecutionPoint(std::nullopt);
        }
    }
    refreshInlineDebuggerValues();
}

namespace {

[[nodiscard]] bool isDebugIdentByte(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x80 || (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || c == '_';
}

[[nodiscard]] bool lineContainsVariable(std::string_view lineText, std::string_view varName) noexcept {
    if (varName.empty() || lineText.size() < varName.size()) {
        return false;
    }
    std::size_t pos = 0;
    while ((pos = lineText.find(varName, pos)) != std::string_view::npos) {
        const bool beforeOk = (pos == 0) || !isDebugIdentByte(lineText[pos - 1]);
        const bool afterOk =
            (pos + varName.size() >= lineText.size()) || !isDebugIdentByte(lineText[pos + varName.size()]);
        if (beforeOk && afterOk) {
            return true;
        }
        pos += varName.size();
    }
    return false;
}

} // namespace

void DebugController::refreshInlineDebuggerValues() {
    const bool active = paused() && m_frame && m_frame->hasSource() && !m_currentFrameVariables.empty() &&
                        (m_coreClrSession || m_frame->sourceReference == 0);
    for (auto& [view, attached] : m_editors) {
        if (!attached->view) {
            continue;
        }
        if (!active || !m_breakpoints.samePath(attached->path, m_frame->path)) {
            attached->view->setInlineDebuggerValues({});
            continue;
        }
        const text::Snapshot& snapshot = attached->view->editor().text();
        const std::uint32_t lineCount = snapshot.lineCount();
        if (lineCount == 0) {
            attached->view->setInlineDebuggerValues({});
            continue;
        }
        const std::uint32_t stoppedLine = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(std::max<std::int64_t>(1, m_frame->line) - 1), lineCount - 1);
        const std::uint32_t firstLine = stoppedLine > 40 ? stoppedLine - 40 : 0;
        const std::uint32_t lastLine = std::min<std::uint32_t>(stoppedLine + 2, lineCount - 1);
        std::vector<EditorInlineValue> inlineValues;
        for (std::uint32_t l = firstLine; l <= lastLine; ++l) {
            const auto info = snapshot.lineInfo(LineIndex{l});
            if (!info || info->content.empty()) {
                continue;
            }
            const std::string lineText = snapshot.text(info->content);
            std::string combined;
            std::size_t countOnLine = 0;
            for (const debug::dap::Variable& var : m_currentFrameVariables) {
                if (lineContainsVariable(lineText, var.name)) {
                    if (!combined.empty()) {
                        combined += "  ";
                    }
                    std::string val = var.value;
                    if (val.size() > 48) {
                        val = val.substr(0, 45) + "...";
                    }
                    combined += var.name + " = " + val;
                    if (++countOnLine >= 4) {
                        break;
                    }
                }
            }
            if (!combined.empty()) {
                inlineValues.push_back(EditorInlineValue{
                    .line = LineIndex{l},
                    .text = std::move(combined),
                    .tooltip = "Inline debugger variable values",
                });
            }
        }
        attached->view->setInlineDebuggerValues(std::move(inlineValues));
    }
}

QString DebugController::debugHoverFor(std::string_view expression, LineIndex) {
    if (!paused() || !m_session) {
        return {};
    }
    while (!expression.empty() && (expression.front() == ' ' || expression.front() == '\t')) {
        expression.remove_prefix(1);
    }
    while (!expression.empty() && (expression.back() == ' ' || expression.back() == '\t')) {
        expression.remove_suffix(1);
    }
    if (expression.empty()) {
        return {};
    }
    for (const debug::dap::Variable& var : m_currentFrameVariables) {
        if (var.name == expression) {
            if (!var.type.empty()) {
                return QString::fromStdString(std::format("{} ({}) = {}", var.name, var.type, var.value));
            }
            return QString::fromStdString(std::format("{} = {}", var.name, var.value));
        }
    }
    if (const auto it = m_hoverCache.find(expression); it != m_hoverCache.end()) {
        return QString::fromStdString(it->second);
    }
    const std::string exprStr(expression);
    evaluate(QString::fromStdString(exprStr), QStringLiteral("hover"),
             [this, exprStr](Result<debug::dap::Variable> res) {
                 if (!res || res->value.empty()) {
                     return;
                 }
                 const std::string formatted = !res->type.empty()
                                                   ? std::format("{} ({}) = {}", exprStr, res->type, res->value)
                                                   : std::format("{} = {}", exprStr, res->value);
                 m_hoverCache[exprStr] = formatted;
             });
    return {};
}

} // namespace frost::ui
