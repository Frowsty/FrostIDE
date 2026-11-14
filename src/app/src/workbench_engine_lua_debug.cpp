// SPDX-License-Identifier: Apache-2.0
// Private engine bootstrap, direct Lua DAP and actual owned-Play cleanup.
#include "workbench_engine_lua_debug.hpp"

#include "frost/debug/lua_debug_adapter.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"

#include "engine_integration_internal.hpp"
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <map>
#include <optional>
#include <utility>

namespace frost::app {
namespace {
bool sameOwner(const engine::EngineIdentity& a, const engine::EngineIdentity& b) {
    return a.projectPath == b.projectPath && a.instanceId == b.instanceId && a.generation == b.generation &&
           a.processId > 0 && a.processId == b.processId && a.connectionGeneration != 0 &&
           a.connectionGeneration == b.connectionGeneration && b.scriptDebugAvailable &&
           b.scriptDebugProtocolVersion == 1;
}
} // namespace

Result<EngineLuaDebugOwner> EngineIntegration::captureLuaDebugOwner() const {
    const auto& impl = *m_impl;
    if (!impl.session || !impl.session->connected() || !impl.identity || !impl.identity->scriptDebugAvailable ||
        impl.identity->scriptDebugProtocolVersion != 1)
        return fail(ErrorCode::Unsupported, "The selected engine has no authenticated Lua DAP endpoint");
    if (!impl.admitted(impl.epoch, impl.root, impl.trustRevision))
        return fail(ErrorCode::Untrusted, "Lua debugging requires current project trust");
    if (impl.policy->nativePaused.load())
        return fail(ErrorCode::Busy, "The engine is paused by its native debugger");
    auto connection = impl.session->debugConnectionStopToken(*impl.identity);
    if (!connection)
        return std::unexpected(connection.error());
    EngineLuaDebugOwner result;
    result.identity = *impl.identity;
    result.identity.schema = {};
    result.root = impl.root;
    result.epoch = impl.epoch;
    result.sessionEpoch = impl.sessionEpoch;
    result.trustRevision = impl.trustRevision;
    result.projectStop = impl.workStop.get_token();
    result.connectionStop = *connection;
    const auto expected = result.identity;
    const auto epoch = result.epoch, sessionEpoch = result.sessionEpoch, revision = result.trustRevision;
    const auto root = result.root;
    QPointer<const EngineIntegration> self(this);
    result.current = [self, expected, epoch, sessionEpoch, root, revision] {
        if (!self)
            return false;
        const auto& live = *self->m_impl;
        return live.session && live.session->connected() && live.sessionEpoch == sessionEpoch && live.identity &&
               sameOwner(expected, *live.identity) && live.admitted(epoch, root, revision);
    };
    result.nativePaused = [policy = impl.policy] { return policy->nativePaused.load(); };
    const auto policy = impl.policy;
    const auto physical = impl.physicalAdmission;
    const auto key = impl.projectKey;
    auto* trust = &impl.workbench.m_services.trust;
    auto* fs = &impl.workbench.m_services.fs;
    result.admit = [policy, physical, key, root, revision, expected, trust, fs, connection = *connection,
                    projectStop = result.projectStop](const debug::LuaDebugIdentity& identity,
                                                      std::string_view command) -> Result<void> {
        if (connection.stop_requested() || projectStop.stop_requested() || !policy->active.load())
            return fail(ErrorCode::Cancelled, "The Lua debugger owner connection ended");
        if (identity.instanceId != expected.instanceId || identity.generation != expected.generation ||
            identity.projectPath != expected.projectPath || identity.processId != expected.processId ||
            identity.connectionGeneration != expected.connectionGeneration)
            return fail(ErrorCode::PermissionDenied, "The Lua debugger belongs to another engine connection");
        const auto current = trust->snapshot(root);
        if (!current.trusted || current.revision != revision)
            return fail(ErrorCode::Untrusted, "Lua debugger project trust changed");
        if (!retainEngineProjectIdentity(*fs, root, key, physical))
            return fail(ErrorCode::PermissionDenied, "The Lua debugger physical project changed");
        if (policy->nativePaused.load() && command != "<receive>")
            return fail(ErrorCode::Busy, "The engine is paused by its native debugger");
        std::scoped_lock lock(policy->mutex);
        const auto& context = policy->context;
        if (!context.connected || !context.paired || !context.trusted ||
            context.sessionId != expected.instanceId + "/" + expected.generation || context.sessionProject != key)
            return fail(ErrorCode::PermissionDenied, "The Lua debugger session is no longer admitted");
        return {};
    };
    return result;
}

Result<engine::EngineState> EngineIntegration::currentLuaDebugState(const EngineLuaDebugOwner& owner) const {
    if (!owner.current || !owner.current() || !m_impl->state || !sameOwner(owner.identity, m_impl->state->identity))
        return fail(ErrorCode::Cancelled, "No current state belongs to this Lua debugger owner");
    return *m_impl->state;
}

Result<void> EngineIntegration::connectLuaDebug(const EngineLuaDebugOwner& owner, debug::LuaDebugAdmission admission,
                                                std::stop_token stop, debug::LuaDebugTransportCallbacks callbacks,
                                                LuaConnected completed) {
    if (!completed || !admission || !owner.current || !owner.current() || stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Lua debugger preparation is no longer current");
    QPointer<EngineIntegration> self(this);
    const auto expected = owner.identity;
    auto accepted = m_impl->session->bootstrapScriptDebug(
        expected,
        [admission, expected](const engine::EngineIdentity& live) -> Result<void> {
            if (!sameOwner(expected, live))
                return fail(ErrorCode::PermissionDenied, "Lua bootstrap owner changed");
            return admission(
                {live.projectPath, live.instanceId, live.generation, {}, live.processId, live.connectionGeneration},
                "script.debug.bootstrap");
        },
        [self, owner, admission, stop, callbacks = std::move(callbacks),
         completed = std::move(completed)](Result<engine::ScriptDebugBootstrap> result) mutable {
            // The credential remains in a move-only private type; never enters Console/EnginePush.
            auto privateResult = std::make_shared<Result<engine::ScriptDebugBootstrap>>(std::move(result));
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, owner, admission, stop, callbacks = std::move(callbacks), completed = std::move(completed),
                 privateResult]() mutable {
                    if (!self)
                        return;
                    if (!*privateResult) {
                        completed(std::unexpected(privateResult->error()));
                        return;
                    }
                    if (!owner.current() || stop.stop_requested()) {
                        completed(fail(ErrorCode::Cancelled, "Lua bootstrap owner ended before authentication"));
                        return;
                    }
                    self->m_impl->background([self, owner, admission, stop, callbacks = std::move(callbacks),
                                              completed = std::move(completed), privateResult]() mutable {
                        auto bootstrap = std::move(**privateResult);
                        debug::LuaDebugEndpoint endpoint(std::move(bootstrap).takeCredential());
                        endpoint.identity = {bootstrap.projectPath, bootstrap.instanceId,
                                             bootstrap.generation,  bootstrap.debugSessionId,
                                             bootstrap.processId,   bootstrap.connectionGeneration};
                        endpoint.port = bootstrap.port;
                        endpoint.expiresAt = bootstrap.expiresAt;
                        auto connection = std::make_shared<Result<debug::LuaDebugConnection>>(
                            debug::connectLuaDebugEndpoint(std::move(endpoint), admission,
                                                           std::chrono::steady_clock::now() + std::chrono::seconds(30),
                                                           stop, std::move(callbacks)));
                        if (!self)
                            return;
                        QMetaObject::invokeMethod(
                            self.data(),
                            [self, owner, stop, completed = std::move(completed), connection]() mutable {
                                if (!self)
                                    return;
                                if (!owner.current() || stop.stop_requested())
                                    completed(
                                        fail(ErrorCode::Cancelled, "Lua debugger owner ended during authentication"));
                                else
                                    completed(std::move(*connection));
                            },
                            Qt::QueuedConnection);
                    });
                },
                Qt::QueuedConnection);
        },
        stop);
    if (!accepted)
        return std::unexpected(accepted.error());
    return {};
}

Result<void> EngineIntegration::commandForLua(const EngineLuaDebugOwner& owner, std::string method, lang::Json params,
                                              engine::EngineSession::ObservedReply completed, std::stop_token stop) {
    if (!owner.current || !owner.current() || stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Lua command owner is no longer current");
    if (owner.nativePaused && owner.nativePaused())
        return fail(ErrorCode::Busy, "The engine is paused by its native debugger");
    QPointer<EngineIntegration> self(this);
    auto accepted = m_impl->session->requestObserved(
        std::move(method), std::move(params),
        [self, owner, completed = std::move(completed)](Result<engine::EngineCommandObservation> result) mutable {
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, owner, completed = std::move(completed), result = std::move(result)]() mutable {
                    if (!self || !completed)
                        return;
                    if (!owner.current())
                        completed(fail(ErrorCode::Cancelled, "Lua command owner changed"));
                    else
                        completed(std::move(result));
                },
                Qt::QueuedConnection);
        },
        stop);
    if (!accepted)
        return std::unexpected(accepted.error());
    return {};
}

struct EngineLuaDebugCoordinator::Impl {
    using Cancellation = std::stop_callback<std::function<void()>>;
    struct Job {
        EngineLuaDebugOwner owner;
        EngineLuaDebugLifecycle lifecycle;
        bool lifecycleReady = false;
        EngineLuaDebugInterruption interruption = EngineLuaDebugInterruption::Cancelled;
        std::optional<Error> interruptionError;
        debug::LuaDebugIdentity luaIdentity;
        std::stop_source stop;
        std::vector<std::unique_ptr<Cancellation>> cancellations;
        // Only the GUI writes binding; worker only reads atomics. Registry IDs never transfer.
        std::atomic<std::uint64_t> registryId{0}, controllerGeneration{0};
        std::atomic<bool> live{true};
        QPointer<ui::DebugSessionRegistry> registry;
        ui::DebugGroupId group = 0;
        bool startPlay = false, configured = false, stopping = false, detached = false, stopAcknowledged = false;
        bool playPending = false, stopOnEntry = true, attached = false, guardedStopSent = false,
             launchDispatched = false;
        std::string playGeneration, operationId;
        std::optional<std::string> scene;
        std::uint64_t frames = 1, launchSequence = 0;
        QTimer* deadline = nullptr;
        std::shared_ptr<void> launchLifetime;
    };
    EngineLuaDebugCoordinator& self;
    QPointer<EngineIntegration> engine;
    std::vector<std::shared_ptr<Job>> jobs;
    explicit Impl(EngineLuaDebugCoordinator& owner, EngineIntegration& integration)
        : self(owner), engine(&integration) {}
    bool current(const std::shared_ptr<Job>& job) const {
        return engine && job->live.load() && !job->stop.stop_requested() && job->owner.current();
    }
    ui::DebugController* controller(const std::shared_ptr<Job>& job) const {
        if (!job->registry)
            return nullptr;
        auto* result = job->registry->controller(job->registryId.load());
        return result && result->sessionGeneration() == job->controllerGeneration.load() ? result : nullptr;
    }
    void report(const Error& error) { emit self.failed(QString::fromStdString(error.message())); }
    void finish(const std::shared_ptr<Job>& job) {
        if (!job->live.exchange(false))
            return;
        job->stop.request_stop();
        if (job->deadline) {
            job->deadline->stop();
            job->deadline->deleteLater();
            job->deadline = nullptr;
        }
        std::erase(jobs, job);
        if (!job->lifecycleReady && job->lifecycle.interrupted) {
            const auto error = job->interruptionError.value_or(Error(
                job->interruption == EngineLuaDebugInterruption::NativePaused ? ErrorCode::Busy : ErrorCode::Cancelled,
                job->interruption == EngineLuaDebugInterruption::NativePaused
                    ? "Native debugger paused during Lua preparation"
                    : "Lua debugger preparation ended before readiness"));
            job->lifecycle.interrupted(job->interruption, error);
        }
        if (job->registryId.load() != 0)
            emit self.bindingChanged(job->registryId.load());
    }
    void guardedStop(const std::shared_ptr<Job>& job) {
        if (!current(job) || !job->stopping || !job->detached || job->playPending || job->guardedStopSent)
            return;
        if (job->operationId.empty()) {
            finish(job);
            return;
        } // Already-playing attach owns nothing.
        auto params = lang::Json::object().set("expectedOperationId", job->operationId);
        if (!job->playGeneration.empty())
            params.set("expectedPlayGeneration", job->playGeneration);
        job->guardedStopSent = true;
        auto accepted = engine->commandForLua(
            job->owner, "play.stop", std::move(params),
            [guard = QPointer<EngineLuaDebugCoordinator>(&self),
             job](Result<engine::EngineCommandObservation> response) {
                if (!guard)
                    return;
                auto& impl = *guard->m_impl;
                if (!impl.current(job)) {
                    impl.finish(job);
                    return;
                }
                if (!response) {
                    impl.report(response.error());
                    impl.finish(job);
                    return;
                }
                job->stopAcknowledged = true;
                impl.observed(job);
            },
            job->stop.get_token());
        if (!accepted) {
            report(accepted.error());
            finish(job);
        }
    }
    void detachObserved(const std::shared_ptr<Job>& job, Result<void> result) {
        if (!job->stopping || !current(job))
            return;
        if (!result) {
            report(result.error());
            finish(job);
            return;
        }
        job->detached = true;
        guardedStop(job);
    }
    void observed(const std::shared_ptr<Job>& job) {
        if (!current(job)) {
            finish(job);
            return;
        }
        auto state = engine->currentLuaDebugState(job->owner);
        if (!state)
            return;
        if (!job->operationId.empty() && state->operationId == job->operationId &&
            state->sequence > job->launchSequence) {
            if (state->phase == "playing" && state->playing && !state->playGeneration.empty()) {
                if (!job->playGeneration.empty() && job->playGeneration != state->playGeneration) {
                    finish(job);
                    return;
                }
                job->playGeneration = state->playGeneration; // Promote only from actual matching Play.
            }
            const bool terminal = state->state == "Edit" && (state->phase == "cancelled" ||
                                                             state->phase == "completed" || state->phase == "failed");
            if (terminal && state->phase == "failed") {
                job->interruption = EngineLuaDebugInterruption::Failed;
                job->interruptionError =
                    Error(ErrorCode::IoError,
                          state->operationError.empty() ? "The engine Play operation failed" : state->operationError);
                report(*job->interruptionError);
            }
            if (terminal && job->stopping && job->detached && job->stopAcknowledged) {
                emit self.ownedPlayStopped(job->registryId.load(), QString::fromStdString(job->playGeneration));
                finish(job);
                return;
            }
            if (terminal && !job->stopping) {
                finish(job);
                return;
            }
        }
        if (job->stopping && job->detached && !job->guardedStopSent)
            guardedStop(job);
        // A delayed pre-reply state cannot replace this operation. A newer authoritative operation,
        // local Play or VM replacement cannot inherit this exact accepted operation's ownership.
        if (!job->operationId.empty() && state->sequence > job->launchSequence &&
            state->operationId != job->operationId)
            finish(job);
    }
    Result<void> stopOwned(const std::shared_ptr<Job>& job) {
        if (job->owner.nativePaused && job->owner.nativePaused())
            return fail(ErrorCode::Busy, "Resume the exact native debugger before stopping owned Lua Play");
        if (!current(job) || (!job->startPlay && job->playGeneration.empty()))
            return fail(ErrorCode::PermissionDenied, "This Lua session has no current owned Play lease");
        if (job->stopping)
            return {};
        auto state = engine->currentLuaDebugState(job->owner);
        if (!state || (!job->operationId.empty() && state->sequence > job->launchSequence &&
                       state->operationId != job->operationId))
            return fail(ErrorCode::PermissionDenied, "The engine Play generation changed");
        auto* child = controller(job);
        if (!child || !child->session())
            return fail(ErrorCode::Cancelled, "The owning Lua debugger ended");
        job->stopping = true;
        // Attached session stop is standard disconnect(terminateDebuggee=false), no host kill timer.
        // The I/O observer invokes detachObserved only after the actual matching disconnect ACK.
        child->session()->stop();
        armDeadline(job, 10000);
        return {};
    }
    void armDeadline(const std::shared_ptr<Job>& job, int milliseconds) {
        if (!job->deadline) {
            job->deadline = new QTimer(&self);
            job->deadline->setSingleShot(true);
            QObject::connect(job->deadline, &QTimer::timeout, &self, [this, job] {
                report(Error(ErrorCode::Timeout, job->stopping ? "Lua detach/owned Play Stop was not confirmed"
                                                               : "Lua DAP configuration was not acknowledged"));
                finish(job);
            });
        }
        job->deadline->start(milliseconds);
    }
    Result<void> launch(ui::DebugSessionRegistry& registry, bool startPlay, std::optional<std::string> scene,
                        std::uint64_t frames, std::stop_token externalStop, EngineLuaDebugLifecycle lifecycle = {}) {
        const auto group = registry.createGroup();
        if (group == 0)
            return fail(ErrorCode::Busy, "The Lua debugger group could not be reserved");
        struct PendingLaunch {
            std::stop_source stop;
            std::unique_ptr<Cancellation> external;
            QMetaObject::Connection groupStop, registryDestroyed;
            ~PendingLaunch() {
                QObject::disconnect(groupStop);
                QObject::disconnect(registryDestroyed);
            }
        };
        auto pending = std::make_shared<PendingLaunch>();
        pending->external = std::make_unique<Cancellation>(externalStop, [weak = std::weak_ptr(pending)] {
            if (const auto pending = weak.lock())
                pending->stop.request_stop();
        });
        QPointer<EngineLuaDebugCoordinator> guard(&self);
        QPointer<ui::DebugSessionRegistry> sessions(&registry);
        pending->groupStop = QObject::connect(&registry, &ui::DebugSessionRegistry::groupStopRequested, &self,
                                              [weak = std::weak_ptr(pending), group](ui::DebugGroupId stopped) {
                                                  if (stopped == group)
                                                      if (const auto pending = weak.lock())
                                                          pending->stop.request_stop();
                                              });
        pending->registryDestroyed =
            QObject::connect(&registry, &QObject::destroyed, &self, [weak = std::weak_ptr(pending)] {
                if (const auto pending = weak.lock())
                    pending->stop.request_stop();
            });
        auto accepted = self.prepare(
            startPlay, pending->stop.get_token(),
            [guard, sessions, group, pending](Result<PreparedDebugLaunch> prepared) {
                if (!guard || !sessions)
                    return;
                QObject::disconnect(pending->groupStop); // An adopted group's Stop routes through its controller.
                if (pending->stop.stop_requested()) {
                    sessions->finishPreparing(group);
                    return;
                }
                if (!prepared) {
                    guard->m_impl->report(prepared.error());
                    sessions->rollbackGroup(group);
                    sessions->finishPreparing(group);
                    return;
                }
                auto started = prepared->start(*sessions, group);
                if (!started) {
                    guard->m_impl->report(started.error());
                    sessions->rollbackGroup(group);
                }
                sessions->finishPreparing(group);
            },
            true, std::move(lifecycle));
        if (!accepted) {
            pending->stop.request_stop();
            QObject::disconnect(pending->groupStop);
            QObject::disconnect(pending->registryDestroyed);
            registry.rollbackGroup(group);
            registry.finishPreparing(group);
            return accepted;
        }
        // prepare appends synchronously; its private network completion always returns through GUI.
        auto& job = jobs.back();
        job->scene = std::move(scene);
        job->frames = frames;
        job->launchLifetime = pending;
        return {};
    }
    void configured(const std::shared_ptr<Job>& job) {
        auto* child = controller(job);
        if (!current(job) || !child || !child->configured() || !job->attached)
            return;
        if (!child->session() || !child->session()->capabilities().supportsConfigurationDoneRequest) {
            report(Error(ErrorCode::Unsupported, "Lua debugging requires an actual configurationDone acknowledgement"));
            if (child->session())
                child->session()->stop();
            finish(job);
            return;
        }
        const bool nativePausedAtEntry = job->owner.nativePaused && job->owner.nativePaused();
        if (!job->configured) {
            job->configured = true;
            if (const auto binding = self.binding(job->registryId.load())) {
                if (job->lifecycle.configured)
                    job->lifecycle.configured(*binding);
                if (!current(job)) {
                    finish(job);
                    return;
                } // A lifecycle consumer may revoke this owner.
                if (!job->startPlay) {
                    job->lifecycleReady = true;
                    if (job->lifecycle.ready)
                        job->lifecycle.ready(*binding);
                }
            }
        }
        if (job->deadline)
            job->deadline->stop();
        if (job->stopping)
            return;
        if (!job->startPlay && !job->scene)
            return;
        if (job->launchDispatched || nativePausedAtEntry)
            return; // Wait for actual native Continued.
        job->launchDispatched = true;
        job->playPending = job->startPlay;
        const std::string method = job->scene ? "test.run" : "play.start";
        auto params = lang::Json::object();
        if (job->scene)
            params.set("scenes", lang::Json::array().push(*job->scene))
                .set("frames", job->frames)
                .set("mode", "headless");
        auto admitted = engine->commandForLua(
            job->owner, method, std::move(params),
            [guard = QPointer<EngineLuaDebugCoordinator>(&self),
             job](Result<engine::EngineCommandObservation> observedResult) {
                if (!guard)
                    return;
                auto& impl = *guard->m_impl;
                if (!impl.current(job)) {
                    impl.finish(job);
                    return;
                }
                job->playPending = false;
                Result<lang::Json> result = observedResult
                                                ? Result<lang::Json>(std::move(observedResult->data))
                                                : Result<lang::Json>(std::unexpected(observedResult.error()));
                if (observedResult && !job->scene)
                    job->launchSequence = observedResult->stateSequenceAtReply;
                if (!result) {
                    job->interruption = EngineLuaDebugInterruption::Failed;
                    job->interruptionError = result.error();
                    impl.report(result.error());
                    if (auto* child = impl.controller(job))
                        child->stop();
                    impl.finish(job);
                    return;
                }
                if (!job->scene) {
                    const auto accepted = result->boolAt("accepted"), pending = result->boolAt("pending");
                    const auto operation = result->stringAt("operationId");
                    const bool validOperation = operation.size() == 32 && std::ranges::all_of(operation, [](char c) {
                                                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                                                });
                    if (!accepted || !pending || result->boolAt("startedNow") != false || (*accepted != *pending) ||
                        (*accepted && (!validOperation || result->stringAt("phase") != "pending" ||
                                       !result->stringAt("playGeneration").empty()))) {
                        impl.report(
                            Error(ErrorCode::Corrupt, "The engine omitted an actual remote Play operation receipt"));
                        if (auto* child = impl.controller(job))
                            child->session()->stop();
                        impl.finish(job);
                        return;
                    }
                    if (*accepted)
                        job->operationId = operation; // Pending cleanup authority, never invented Play.
                    impl.observed(job);
                }
                if (!job->scene && job->live.load()) {
                    job->lifecycleReady = true;
                    if (const auto binding = guard->binding(job->registryId.load()); binding && job->lifecycle.ready)
                        job->lifecycle.ready(*binding);
                }
                if (!job->scene)
                    emit guard->playStartObserved(
                        job->registryId.load(), result->boolAt("startedNow") == true,
                        QString::fromUtf8(result->stringAt("playGeneration").data(),
                                          static_cast<qsizetype>(result->stringAt("playGeneration").size())));
                if (job->stopping && job->detached)
                    impl.detachObserved(job, {});
            },
            job->stop.get_token());
        if (!admitted) {
            job->interruption = EngineLuaDebugInterruption::Failed;
            job->interruptionError = admitted.error();
            report(admitted.error());
            child->session()->stop();
            finish(job);
        }
    }
};

EngineLuaDebugCoordinator::EngineLuaDebugCoordinator(EngineIntegration& engine, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, engine)) {
    connect(&engine, &EngineIntegration::nativeDebuggerEvidenceChanged, this, [this] {
        const auto jobs = m_impl->jobs;
        for (const auto& job : jobs) {
            if (!job->owner.nativePaused)
                continue;
            if (!job->owner.nativePaused()) {
                if (job->configured)
                    m_impl->configured(job);
                continue;
            }
            if (job->configured)
                continue;
            if (job->registryId.load() != 0) {
                (void)cancelPreparation(job->registryId.load(), job->controllerGeneration.load(),
                                        EngineLuaDebugInterruption::NativePaused);
            } else {
                job->interruption = EngineLuaDebugInterruption::NativePaused;
                m_impl->finish(job);
            }
        }
    });
    connect(&engine, &EngineIntegration::stateObserved, this, [this] {
        const auto jobs = m_impl->jobs;
        for (const auto& job : jobs)
            m_impl->observed(job);
    });
}
EngineLuaDebugCoordinator::~EngineLuaDebugCoordinator() { cancel(); }
Result<void> EngineLuaDebugCoordinator::cancelPreparation(ui::DebugSessionId id, std::uint64_t generation,
                                                          EngineLuaDebugInterruption reason) {
    const auto found = std::ranges::find_if(m_impl->jobs, [id, generation](const auto& job) {
        return job->registryId.load() == id && job->controllerGeneration.load() == generation;
    });
    if (found == m_impl->jobs.end())
        return fail(ErrorCode::NotFound, "This Lua preparation is no longer current");
    const auto job = *found;
    if (job->configured)
        return fail(ErrorCode::Busy, "This Lua session is already configured");
    auto* child = m_impl->controller(job);
    if (!child || !child->session())
        return fail(ErrorCode::Cancelled, "This Lua controller ended");
    auto cancelled = child->session()->cancelAttachedPreparation();
    if (!cancelled)
        return cancelled; // Busy preserves a wire-configured session whose GUI callback is pending.
    job->interruption = reason;
    m_impl->finish(job);
    return {};
}
void EngineLuaDebugCoordinator::cancel() {
    const auto jobs = m_impl->jobs;
    for (const auto& job : jobs) {
        if (auto* child = m_impl->controller(job); child && child->session())
            child->session()->stop();
        m_impl->finish(job);
    }
}

Result<void> EngineLuaDebugCoordinator::prepare(bool startPlay, std::stop_token stop, Prepared completed,
                                                bool stopOnEntry, EngineLuaDebugLifecycle lifecycle) {
    if (!m_impl->engine)
        return fail(ErrorCode::Cancelled, "The engine integration ended");
    auto owner = m_impl->engine->captureLuaDebugOwner();
    if (!owner)
        return std::unexpected(owner.error());
    return prepareForOwner(std::move(*owner), startPlay, stop, std::move(completed), stopOnEntry, std::move(lifecycle));
}
Result<void> EngineLuaDebugCoordinator::prepareForOwner(EngineLuaDebugOwner owner, bool startPlay, std::stop_token stop,
                                                        Prepared completed, bool stopOnEntry,
                                                        EngineLuaDebugLifecycle lifecycle) {
    if (m_impl->jobs.size() >= 128)
        return fail(ErrorCode::Busy, "Too many retained Lua preparations");
    if (!m_impl->engine || !completed || stop.stop_requested() || !owner.current || !owner.current() || !owner.admit)
        return fail(ErrorCode::Cancelled, "Lua debugger preparation owner was cancelled or revoked");
    auto job = std::make_shared<Impl::Job>();
    job->owner = std::move(owner);
    job->lifecycle = std::move(lifecycle);
    job->startPlay = startPlay;
    job->stopOnEntry = stopOnEntry;
    for (const auto& source : {stop, job->owner.projectStop, job->owner.connectionStop})
        job->cancellations.push_back(std::make_unique<Impl::Cancellation>(source, [weak = std::weak_ptr(job)] {
            if (const auto job = weak.lock())
                job->stop.request_stop();
        }));
    m_impl->jobs.push_back(job);
    QPointer<EngineLuaDebugCoordinator> self(this);
    auto admission = [job](const debug::LuaDebugIdentity& identity, std::string_view command) -> Result<void> {
        if (!job->live.load() || job->stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Lua debugger session ended");
        const bool setup = command == "script.debug.bootstrap" || command == "frost/authChallenge" ||
                           command == "frost/authProve" || command == "<adopt>";
        if (!setup && (job->registryId.load() == 0 || job->controllerGeneration.load() == 0))
            return fail(ErrorCode::PermissionDenied, "Lua debugger has no bound registry controller");
        return job->owner.admit(identity, command);
    };
    debug::LuaDebugTransportCallbacks callbacks;
    callbacks.onAttached = [self, weak = std::weak_ptr(job)](Result<void> result) {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, weak, result = std::move(result)]() mutable {
                if (!self)
                    return;
                const auto job = weak.lock();
                if (!job || !self->m_impl->current(job))
                    return;
                if (!result) {
                    self->m_impl->report(result.error());
                    self->m_impl->finish(job);
                    return;
                }
                job->attached = true;
                self->m_impl->configured(job);
            },
            Qt::QueuedConnection);
    };
    callbacks.onDetached = [self, weak = std::weak_ptr(job)](Result<void> result) {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, weak, result = std::move(result)]() mutable {
                if (self)
                    if (const auto job = weak.lock())
                        self->m_impl->detachObserved(job, std::move(result));
            },
            Qt::QueuedConnection);
    };
    auto started = m_impl->engine->connectLuaDebug(
        job->owner, std::move(admission), job->stop.get_token(), std::move(callbacks),
        [self, job, completed = std::move(completed)](Result<debug::LuaDebugConnection> connection) mutable {
            if (!self)
                return;
            if (!connection || !self->m_impl->current(job)) {
                if (!connection && job->live.load()) {
                    job->interruption = EngineLuaDebugInterruption::Failed;
                    job->interruptionError = connection.error();
                }
                completed(connection ? Result<PreparedDebugLaunch>(fail(ErrorCode::Cancelled, "Lua owner ended"))
                                     : Result<PreparedDebugLaunch>(std::unexpected(connection.error())));
                self->m_impl->finish(job);
                return;
            }
            job->luaIdentity = connection->identity();
            auto factory = debug::luaDebugAdapterFactory(std::move(*connection));
            // Dropping an unconsumed prepared launch closes/revokes its lease immediately. Jobs do
            // not retain this guard, so abandonment cannot leak a slot or a credential-bearing peer.
            auto guard = std::shared_ptr<int>(new int(0), [self, weak = std::weak_ptr(job)](int* value) {
                delete value;
                const auto job = weak.lock();
                if (!job || job->registryId.load() != 0)
                    return;
                job->stop.request_stop();
                if (self)
                    QMetaObject::invokeMethod(
                        self.data(),
                        [self, job] {
                            if (self)
                                self->m_impl->finish(job);
                        },
                        Qt::QueuedConnection);
            });
            completed(PreparedDebugLaunch{[self, job, guard, factory = std::move(factory)](
                                              ui::DebugSessionRegistry& registry,
                                              ui::DebugGroupId group) mutable -> Result<ui::DebugSessionId> {
                if (!self || !self->m_impl->current(job))
                    return fail(ErrorCode::Cancelled, "Prepared Lua owner ended");
                if (job->registryId.load() != 0)
                    return fail(ErrorCode::Busy, "Lua preparation was already consumed");
                auto id = registry.createSession(QStringLiteral("FrostEngine Lua"), group);
                if (!id) {
                    self->m_impl->finish(job);
                    return std::unexpected(id.error());
                }
                job->registry = &registry;
                job->group = group;
                job->registryId.store(*id);
                auto* child = registry.controller(*id);
                debug::DebugTarget target;
                target.name = "FrostEngine Lua";
                target.program = job->owner.identity.projectPath;
                target.workingDirectory = job->owner.root;
                target.trustRoots = {job->owner.root};
                auto boundFactory =
                    [job, child = QPointer<ui::DebugController>(child), factory = std::move(factory)](
                        debug::DapHandlers handlers) mutable -> Result<std::unique_ptr<debug::IDebugAdapterClient>> {
                    if (!child || !job->owner.current() || job->stop.stop_requested() ||
                        (job->owner.nativePaused && job->owner.nativePaused()))
                        return fail(ErrorCode::Cancelled, "Lua controller adoption is no longer admitted");
                    job->controllerGeneration.store(child->sessionGeneration());
                    return factory(std::move(handlers));
                };
                auto result = child->startAdapterTarget(target, std::move(boundFactory),
                                                        lang::Json::object().set("stopOnEntry", job->stopOnEntry),
                                                        "frost-lua", debug::DebugTargetMode::Attach);
                if (!result) {
                    self->m_impl->finish(job);
                    (void)registry.remove(*id);
                    return std::unexpected(result.error());
                }
                if (job->startPlay)
                    child->setStopHandler([self, job]() -> Result<void> {
                        if (!self)
                            return fail(ErrorCode::Cancelled, "Lua coordinator ended");
                        return self->m_impl->stopOwned(job);
                    });
                QObject::connect(child, &ui::DebugController::configuredChanged, self.data(), [self, job] {
                    if (self)
                        self->m_impl->configured(job);
                });
                QObject::connect(child, &ui::DebugController::stateChanged, self.data(), [self, job] {
                    if (!self)
                        return;
                    auto* child = self->m_impl->controller(job);
                    if (!child || child->state() == debug::DebugState::Ended) {
                        // Owned cleanup is retained independently across the detach ACK/session end.
                        if (!job->stopping && !job->playPending)
                            self->m_impl->finish(job);
                        else if (job->playPending) {
                            job->stopping = true;
                            self->m_impl->armDeadline(job, 10000);
                        }
                    }
                });
                self->m_impl->armDeadline(job, 30000);
                self->m_impl->configured(job);
                (void)registry.select(*id);
                emit self->bindingChanged(*id);
                return *id;
            }});
        });
    if (!started) {
        m_impl->finish(job);
        return started;
    }
    return {};
}
std::optional<EngineLuaDebugBinding> EngineLuaDebugCoordinator::binding(ui::DebugSessionId id) const {
    const auto found =
        std::ranges::find_if(m_impl->jobs, [id](const auto& job) { return job->registryId.load() == id; });
    if (found == m_impl->jobs.end() || !m_impl->current(*found))
        return std::nullopt;
    const auto& job = *found;
    const auto* child = m_impl->controller(job);
    if (!child || !child->session() || child->session()->adapterId() != "frost-lua")
        return std::nullopt;
    EngineLuaDebugBinding result;
    result.registrySessionId = id;
    result.controllerGeneration = job->controllerGeneration.load();
    result.identity = job->luaIdentity;
    result.root = job->owner.root;
    result.epoch = job->owner.epoch;
    result.sessionEpoch = job->owner.sessionEpoch;
    result.trustRevision = job->owner.trustRevision;
    QPointer<const EngineLuaDebugCoordinator> self(this);
    result.current = [self, weak = std::weak_ptr(job), id, generation = result.controllerGeneration] {
        if (!self)
            return false;
        const auto job = weak.lock();
        const auto* child = job ? self->m_impl->controller(job) : nullptr;
        return job && self->m_impl->current(job) && job->registryId.load() == id &&
               job->controllerGeneration.load() == generation && child && child->session() &&
               child->session()->adapterId() == "frost-lua";
    };
    return result;
}

Result<void> EngineLuaDebugCoordinator::startAttached(ui::DebugSessionRegistry& sessions, bool startPlay) {
    return m_impl->launch(sessions, startPlay, std::nullopt, 1, {});
}
Result<void> EngineLuaDebugCoordinator::debugScene(ui::DebugSessionRegistry& sessions, std::string scene,
                                                   std::uint64_t frames, std::stop_token stop,
                                                   EngineLuaDebugLifecycle lifecycle) {
    if (scene.empty() || io::isAbsolutePath(scene) || scene.find("..") != std::string::npos ||
        !scene.starts_with("Assets/") || !scene.ends_with(".fscene") || frames == 0)
        return fail(ErrorCode::InvalidArgument, "Choose an exact engine-listed scene and positive frame count");
    return m_impl->launch(sessions, false, std::move(scene), frames, stop, std::move(lifecycle));
}
} // namespace frost::app
