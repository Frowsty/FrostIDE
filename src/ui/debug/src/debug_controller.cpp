// SPDX-License-Identifier: Apache-2.0
//
// DebugController (see frost/ui/debug_controller.hpp): session lifecycle, Debug run and attach
// hand-off, and the relay from session callbacks to the GUI thread.

#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/ui/debug_retirement.hpp"
#include "frost/ui/run_console.hpp"

#include "debug_controller_internal.hpp"
#include <QToolButton>

#include <algorithm>
#include <climits>
#include <format>
#include <utility>

namespace frost::ui {

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
    if (m_session && m_options.retirements &&
        (m_session->targetMode() == debug::DebugTargetMode::Attach || m_session->readOnly()))
        m_options.retirements->retire(std::move(m_session), kStopTimeout);
    else
        m_session.reset(); // Owned launches close their contained tree; attached batches join off-GUI.
}

void DebugController::setConsole(RunConsole* console) {
    if (m_console) {
        disconnect(m_console, nullptr, this, nullptr);
    }
    m_console = console;
    if (m_console) {
        connect(m_console, &RunConsole::stopRequested, this, [this] {
            const QPointer<DebugController> self(this);
            emit userStopRequested();
            if (self)
                self->stop();
        });
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

Result<debug::AdapterFactory> DebugController::adapterFactory(bool* nativeOsThreadIdsVerified,
                                                              debug::DapProcessOutput output) {
    if (nativeOsThreadIdsVerified)
        *nativeOsThreadIdsVerified = false;
    if (m_options.adapterFactory) {
        return m_options.adapterFactory;
    }
    if (m_options.fs == nullptr || m_options.launcher == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the debugger has no process launcher");
    }
    const auto configured = m_options.lldbDapSetting ? m_options.lldbDapSetting() : std::string{};
    auto path = debug::findLldbDap(*m_options.fs, debug::defaultLldbDapSearch(configured));
    if (!path) {
        return std::unexpected(std::move(path.error()));
    }
    if (nativeOsThreadIdsVerified)
        *nativeOsThreadIdsVerified = configured.empty();
    return debug::lldbDapAdapterFactory(*m_options.launcher, std::move(*path), std::move(output));
}

Result<void> DebugController::startTarget(const debug::DebugTarget& target) {
    return startNativeTarget(target, {}, {}, {});
}
Result<void> DebugController::startNativeEngineTarget(const debug::DebugTarget& target, PrivateOutput output,
                                                      std::function<void(std::string_view)> adapterStderr,
                                                      std::function<Result<void>()> launchAdmission) {
    if (!output || !launchAdmission || m_options.adapterFactory)
        return fail(ErrorCode::Unsupported,
                    "Private engine debugging requires the built-in pinned adapter and private admission");
    return startNativeTarget(target, std::move(output), {true, std::move(adapterStderr)},
                             debug::NativeDebugPrivacy{std::move(launchAdmission)});
}
Result<void> DebugController::startNativeTarget(const debug::DebugTarget& target, PrivateOutput output,
                                                debug::DapProcessOutput processOutput,
                                                std::optional<debug::NativeDebugPrivacy> privacy) {
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
    bool osThreadIdsVerified = false;
    auto factory = adapterFactory(&osThreadIdsVerified, std::move(processOutput));
    if (!factory)
        return std::unexpected(std::move(factory.error()));
    if (privacy && !osThreadIdsVerified)
        return fail(ErrorCode::Unsupported, "Private engine debugging requires the built-in pinned LLDB adapter");
    auto started = startSession(
        resolvedTarget.name.empty() ? std::string(io::fileName(resolvedTarget.program)) : resolvedTarget.name,
        resolvedTarget.program, "lldb-dap",
        [this, resolvedTarget, factory = std::move(*factory),
         privacy = std::move(privacy)](debug::DebugSessionCallbacks callbacks) mutable {
            return debug::startDebugSession(*m_options.trust, resolvedTarget, std::move(factory),
                                            m_breakpoints.sessionLines(), std::move(callbacks),
                                            m_breakpoints.sessionFunctions(), m_breakpoints.sessionExceptionFilters(),
                                            m_breakpoints.sessionThrowTypes(), std::move(privacy));
        },
        false, std::move(output));
    if (started) {
        m_lastCoreClrSession = false;
        m_nativeSession = true;
        m_nativeOsThreadIdsVerified = osThreadIdsVerified;
    }
    return started;
}

Result<void> DebugController::startAdapterTarget(const debug::DebugTarget& target, debug::AdapterFactory factory,
                                                 rpc::Json arguments, std::string adapterId,
                                                 debug::DebugTargetMode mode, bool readOnly,
                                                 std::function<Result<void>()> launchAdmission) {
    if (m_pendingRun || m_alternatePending || (m_hooks.mayStart && !m_hooks.mayStart()))
        return fail(ErrorCode::Busy, "Another run or debug launch is still preparing");
    if (!m_options.trust || !factory || !arguments.isObject() || adapterId.empty())
        return fail(ErrorCode::InvalidArgument, "An adapter, arguments and trusted target are required");
    if (launchAdmission) {
        if (auto admitted = launchAdmission(); !admitted)
            return admitted;
    }
    auto roots = target.trustRoots;
    if (roots.empty() && !target.program.empty())
        roots.emplace_back(io::parentPath(target.program));
    if (roots.empty())
        return fail(ErrorCode::Untrusted, "The adapter target has no trusted workspace");
    for (const auto& root : roots)
        if (!io::isAbsolutePath(root) || !m_options.trust->isTrusted(root))
            return fail(ErrorCode::Untrusted, "The adapter target workspace is not trusted");
    if (readOnly) {
        const auto dump = arguments.stringAt("dumpFile");
        const bool localVerbatim = dump.size() >= 7 && dump.starts_with("\\\\?\\") && dump[5] == ':' && dump[6] == '\\';
        if (!io::isAbsolutePath(dump) || (dump.starts_with("\\\\") && !localVerbatim) || dump.starts_with("//") ||
            dump.find('\0') != std::string_view::npos)
            return fail(ErrorCode::InvalidArgument, "A read-only adapter target requires an absolute local dump file");
    }
    if ((!readOnly || !target.program.empty()) && !io::isAbsolutePath(target.program))
        return fail(ErrorCode::InvalidArgument, "The adapter program path must be absolute");
    const auto actualMode = mode;
    const auto display = adapterId;
    auto started = startSession(
        target.name, target.program, display,
        [this, factory = std::move(factory), arguments = std::move(arguments), adapterId, actualMode, readOnly,
         launchAdmission = std::move(launchAdmission)](
            debug::DebugSessionCallbacks callbacks) mutable -> Result<std::unique_ptr<debug::DebugSession>> {
            debug::DebugSessionOptions options;
            options.adapterId = adapterId;
            options.targetMode = actualMode;
            options.readOnly = readOnly;
            options.launchAdmission = std::move(launchAdmission);
            if (!readOnly) {
                options.functionBreakpoints = m_breakpoints.sessionFunctions();
                options.exceptionFilters = m_breakpoints.sessionExceptionFilters();
            }
            return debug::DebugSession::start(std::move(factory), std::move(arguments),
                                              readOnly ? std::map<std::string, std::vector<debug::BreakpointLine>>{}
                                                       : m_breakpoints.sessionLines(),
                                              std::move(callbacks), std::move(options));
        },
        readOnly || actualMode == debug::DebugTargetMode::Attach);
    if (started) {
        m_nativeSession = !readOnly && (adapterId == "lldb-dap" || adapterId == "frost-dbgeng");
        m_lastCoreClrSession = false;
    }
    return started;
}

Result<void> DebugController::attachTarget(const debug::DebugTarget& target, debug::AdapterFactory factory,
                                           rpc::Json attachArguments, const std::function<bool()>& ownerAdmission) {
    const bool admitted = ownerAdmission ? ownerAdmission() : (!m_hooks.mayStart || m_hooks.mayStart());
    if (m_pendingRun || m_alternatePending || !admitted)
        return fail(ErrorCode::Busy, "another run or debug launch is already pending");
    const std::string_view typeView =
        attachArguments.isObject() ? attachArguments.stringAt("type") : std::string_view{};
    const std::string adapterId = !typeView.empty() ? std::string(typeView) : std::string("lldb-dap");
    bool osThreadIdsVerified = false;
    if (!factory && adapterId == "lldb-dap") {
        auto resolved = adapterFactory(&osThreadIdsVerified);
        if (!resolved)
            return std::unexpected(resolved.error());
        factory = std::move(*resolved);
    }
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
        m_nativeOsThreadIdsVerified = osThreadIdsVerified;
    }
    return started;
}

Result<void>
DebugController::attachManagedTarget(const debug::DebugTarget& target, debug::AdapterFactory factory,
                                     std::function<bool(const debug::DebugFrame&)> frameAllowed,
                                     std::vector<std::string> allowedSources,
                                     std::map<std::string, std::vector<debug::BreakpointLine>> verifiedBreakpoints,
                                     std::function<void(std::string, std::function<void(bool)>)> validateBreakpoint,
                                     const std::function<bool()>& ownerAdmission) {
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
                                           const SessionStarter& starter, bool attach, PrivateOutput privateOutput) {
    if (m_shutdown)
        return fail(ErrorCode::Cancelled, "the workbench is shutting down");
    if (m_session)
        return fail(ErrorCode::Busy, "a debug session is running already; stop it first");
    if (m_options.trust == nullptr)
        return fail(ErrorCode::InvalidArgument, "the debugger has no trust store");
    const std::uint64_t generation = ++m_generation;
    m_nativeOsThreadIdsVerified = false;
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
            [relay, generation, privateOutput](std::string_view category, std::string_view text) {
                if (privateOutput) {
                    privateOutput(category, text);
                    return;
                }
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
        .onProcess =
            [relay, generation](std::int64_t) {
                relay->post(generation, [](DebugController* self) { emit self->stateChanged(); });
            },
        .onConfigured =
            [relay, generation] {
                relay->post(generation, [](DebugController* self) {
                    self->m_configured = true;
                    emit self->configuredChanged();
                });
            },
        .onContinued =
            [relay, generation] {
                relay->post(generation, [generation](DebugController* self) {
                    self->m_adapterStopped = false;
                    ++self->m_adapterEvidenceSequence;
                    emit self->adapterContinued(generation);
                    emit self->stateChanged();
                });
            },
        .onAdapterStopped =
            [relay, generation] {
                relay->post(generation, [](DebugController* self) {
                    self->m_adapterStopped = true;
                    ++self->m_adapterEvidenceSequence;
                    emit self->stateChanged();
                });
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
    for (auto& [view, attached] : m_editors)
        if (attached->view)
            refreshMarkers(*view);
    m_rerunAllowed = !attach && !privateOutput && m_options.allowRerun;
    m_state = debug::DebugState::Starting;
    m_configured = false;
    m_adapterStopped = false;
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
    m_adapterStopped = true;
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
    } else if (m_editorPresentation && frame.hasSource() && m_hooks.openSource) {
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

void DebugController::onCapabilities(const debug::dap::Capabilities& capabilities) {
    if (!m_session)
        return; // Capabilities belong to this adapter/session, including embedded Lua adapters.
    const bool msvc = m_session->msvcExceptions();
    m_breakpoints.setAvailableExceptionFilters(capabilities.exceptionBreakpointFilters,
                                               [msvc](std::string_view filter) {
                                                   if (!msvc) {
                                                       return std::string();
                                                   }
                                                   return debug::msvcUnsupportedFilter(filter).value_or(std::string());
                                               });
    emit stateChanged(); // Actual capability changes may enable/refuse explicit engine termination.
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
    ++m_adapterEvidenceSequence;
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
    m_nativeOsThreadIdsVerified = false;
    m_configured = false;
    m_adapterStopped = false;
    m_stopHandler = {};
    m_stopHandlerGeneration = 0;
    clearLoadedSource();
    std::unique_ptr<debug::DebugSession> ended = std::move(m_session);
    ended.reset(); // Joins the client's threads (the adapter has exited or was killed).
    m_breakpoints.resetStatus(debug::BreakpointStatus::Unknown);
    showExecutionPoint();
    emit threadsChanged();
    emit framesChanged();
    emit stateChanged();
}

} // namespace frost::ui
