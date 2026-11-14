// SPDX-License-Identifier: Apache-2.0
// Yama-safe native parent launch with exact authenticated Lua dependency and group-scoped rollback.
#include "engine_mixed_launch.hpp"

#include "frost/io/path.hpp"

#include "engine_integration.hpp"
#include <QPointer>
#include <QProcessEnvironment>
#include <QTimer>

#include <algorithm>
#include <mutex>

namespace frost::app {
namespace {
struct MixedRelay {
    std::mutex mutex;
    QObject* target = nullptr;
    void post(std::function<void()> done) {
        const std::lock_guard lock(mutex);
        if (target)
            QMetaObject::invokeMethod(target, std::move(done), Qt::QueuedConnection);
    }
};
void environment(debug::DebugTarget& target, const std::vector<run::EnvironmentEntry>& changes) {
    auto env = QProcessEnvironment::systemEnvironment();
    for (const auto& e : changes) {
        if (e.value)
            env.insert(QString::fromStdString(e.name), QString::fromStdString(*e.value));
        else
            env.remove(QString::fromStdString(e.name));
    }
    target.inheritEnvironment = false;
    for (const auto& name : env.keys())
        target.environment.emplace_back(name.toStdString(), env.value(name).toStdString());
}
bool sameHost(const debug::LuaDebugIdentity& a, const engine::EngineIdentity& b) {
    return a.projectPath == b.projectPath && a.instanceId == b.instanceId && a.generation == b.generation &&
           a.processId == b.processId && a.connectionGeneration == b.connectionGeneration;
}
} // namespace
struct EngineMixedLaunchCoordinator::Impl {
    using Cancellation = std::stop_callback<std::function<void()>>;
    struct Job {
        EngineNativeHostLaunch launch;
        run::RunConfiguration declared;
        std::stop_source stop, attemptStop;
        std::vector<std::unique_ptr<Cancellation>> cancellations;
        std::unique_ptr<Cancellation> taskCancellation;
        run::RunId preparation = 0;
        std::optional<run::LaunchPlan> plan;
        MultiSessionDebugCoordinator::Prepared prepared;
        PreparedDebugLaunch::Started started;
        QPointer<ui::DebugSessionRegistry> registry;
        ui::DebugGroupId group = 0;
        ui::DebugSessionId native = 0, lua = 0;
        std::uint64_t nativeGeneration = 0, luaGeneration = 0, attempt = 0;
        std::vector<ui::DebugSessionId> priorLua;
        std::optional<EngineLuaDebugOwner> owner;
        std::string phase = "preparing";
        bool finished = false, packageReady = false, luaConfigured = false, attemptPending = false,
             cancellingAttempt = false;
        std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
        std::chrono::steady_clock::duration activeStartup{};
        std::int64_t pid = 0;
    };
    EngineMixedLaunchCoordinator& self;
    QPointer<EngineIntegration> engine;
    QPointer<EngineLuaDebugCoordinator> lua;
    run::RunManager& tasks;
    std::vector<std::shared_ptr<Job>> jobs;
    std::shared_ptr<MixedRelay> relay = std::make_shared<MixedRelay>();
    QTimer* timer;
    lang::Json last = lang::Json::object().set("phase", "idle");
    Impl(EngineMixedLaunchCoordinator& s, EngineIntegration& e, EngineLuaDebugCoordinator& l, run::RunManager& t)
        : self(s), engine(&e), lua(&l), tasks(t), timer(new QTimer(&s)) {
        relay->target = &s;
        timer->setInterval(25);
        QObject::connect(timer, &QTimer::timeout, &self, [this] { poll(); });
    }
    ui::DebugController* native(const std::shared_ptr<Job>& j) const {
        auto* c = j->registry ? j->registry->controller(j->native) : nullptr;
        return c && c->sessionGeneration() == j->nativeGeneration ? c : nullptr;
    }
    ui::DebugController* luaChild(const std::shared_ptr<Job>& j) const {
        auto* c = j->registry ? j->registry->controller(j->lua) : nullptr;
        return c && c->sessionGeneration() == j->luaGeneration ? c : nullptr;
    }
    lang::Json describe(const std::shared_ptr<Job>& j) const {
        return lang::Json::object()
            .set("phase", j->phase)
            .set("groupId", j->group)
            .set("nativeSessionId", j->native)
            .set("luaSessionId", j->lua)
            .set("nativeGeneration", j->nativeGeneration)
            .set("luaGeneration", j->luaGeneration)
            .set("processId", j->pid)
            .set("reservationId", j->launch.token.value)
            .set("configured", j->luaConfigured)
            .set("ready", j->packageReady);
    }
    void phase(const std::shared_ptr<Job>& j, std::string value) {
        if (j->phase == value)
            return;
        j->phase = std::move(value);
        last = describe(j);
        emit self.changed();
    }
    void finish(const std::shared_ptr<Job>& j) {
        j->finished = true;
        last = describe(j);
        std::erase(jobs, j);
        if (jobs.empty())
            timer->stop();
        emit self.changed();
    }
    void abort(const std::shared_ptr<Job>& j, Error error, bool explicitStop = false) {
        if (j->finished)
            return;
        j->stop.request_stop();
        ++j->attempt;
        j->attemptStop.request_stop();
        if (j->preparation)
            tasks.stop(j->preparation);
        if (engine) {
            engine->revokeNativeHost(j->launch.token, !explicitStop);
        }
        auto prepared = std::move(j->prepared);
        auto started = std::move(j->started);
        const bool ready = j->packageReady;
        phase(j, error.code() == ErrorCode::Cancelled ? "cancelled" : "failed");
        finish(j);
        if (prepared)
            prepared(std::unexpected(error));
        else if (started)
            started(std::unexpected(error));
        else if (!ready && error.code() != ErrorCode::Cancelled)
            emit self.failed(QString::fromStdString(error.message()));
    }
    void cancelAttemptForNativePause(const std::shared_ptr<Job>& j) {
        if (j->packageReady || j->luaConfigured || j->cancellingAttempt)
            return;
        j->cancellingAttempt = true;
        if (j->lua && lua) {
            auto cancelled = lua->cancelPreparation(j->lua, j->luaGeneration, EngineLuaDebugInterruption::NativePaused);
            if (!cancelled && cancelled.error().code() == ErrorCode::Busy) {
                j->cancellingAttempt = false;
                phase(j, "native-paused-configured");
                return;
            }
        }
        // The core local preparation cancel marks stopped/no-error BEFORE closing the private peer.
        // It sends no disconnect while the host is native-stopped and never fabricates an ACK.
        ++j->attempt;
        j->attemptStop.request_stop();
        j->attemptPending = false;
        if (j->lua) {
            j->priorLua.push_back(j->lua);
            j->lua = 0;
            j->luaGeneration = 0;
        }
        j->owner.reset();
        j->cancellingAttempt = false;
        phase(j, "native-paused");
    }
    bool holdForNativePause(const std::shared_ptr<Job>& j, const Error& error) {
        if (j->luaConfigured || j->packageReady || !engine)
            return false;
        const auto code = error.code();
        if (code != ErrorCode::Cancelled && code != ErrorCode::Busy && code != ErrorCode::Timeout &&
            code != ErrorCode::IoError)
            return false;
        auto actual = engine->observeNativeHost(j->launch.token);
        if (!actual || !actual->nativeStopped)
            return false;
        cancelAttemptForNativePause(j);
        return true;
    }
    void ready(const std::shared_ptr<Job>& j, std::uint64_t attempt, const EngineLuaDebugBinding& binding) {
        if (j->finished || attempt != j->attempt || j->stop.stop_requested() || !j->owner ||
            !sameHost(binding.identity, j->owner->identity))
            return;
        if (!engine || j->owner->projectStop.stop_requested() || j->owner->connectionStop.stop_requested()) {
            abort(j, Error(ErrorCode::Cancelled, "Mixed authenticated owner ended before readiness"));
            return;
        }
        const auto actualNative = engine->observeNativeHost(j->launch.token);
        if (!actualNative) {
            abort(j, actualNative.error());
            return;
        }
        auto* c = j->registry ? j->registry->controller(binding.registrySessionId) : nullptr;
        if (!c || c->sessionGeneration() != binding.controllerGeneration || !c->configured() ||
            binding.registrySessionId != j->lua) {
            abort(j, Error(ErrorCode::PermissionDenied, "Mixed Lua configured binding changed"));
            return;
        }
        j->luaConfigured = true;
        j->attemptPending = false;
        j->packageReady = true;
        phase(j, "ready");
        auto started = std::move(j->started);
        if (started)
            started(std::vector<ui::DebugSessionId>{j->native, j->lua});
    }
    void beginLua(const std::shared_ptr<Job>& j, EngineLuaDebugOwner owner) {
        if (!lua || !engine) {
            abort(j, Error(ErrorCode::Cancelled, "Mixed engine services ended"));
            return;
        }
        j->owner = std::move(owner);
        j->attemptStop = std::stop_source{};
        j->luaConfigured = false;
        j->attemptPending = true;
        const auto attempt = ++j->attempt;
        phase(j, "lua-preparing");
        QPointer<EngineMixedLaunchCoordinator> guard(&self);
        EngineLuaDebugLifecycle lifecycle;
        lifecycle.configured = [guard, j, attempt](const EngineLuaDebugBinding& binding) {
            if (!guard || j->finished || attempt != j->attempt)
                return;
            if (!j->owner || !sameHost(binding.identity, j->owner->identity)) {
                guard->m_impl->abort(j,
                                     Error(ErrorCode::PermissionDenied, "Mixed Lua identity differs from native host"));
                return;
            }
            j->luaConfigured = true;
            j->lua = binding.registrySessionId;
            j->luaGeneration = binding.controllerGeneration;
            guard->m_impl->phase(j, "lua-configured");
        };
        lifecycle.ready = [guard, j, attempt](const EngineLuaDebugBinding& binding) {
            if (guard)
                guard->m_impl->ready(j, attempt, binding);
        };
        lifecycle.interrupted = [guard, j, attempt](EngineLuaDebugInterruption why, const Error& error) {
            if (!guard || j->finished || attempt != j->attempt)
                return;
            if (why == EngineLuaDebugInterruption::NativePaused && !j->luaConfigured) {
                guard->m_impl->cancelAttemptForNativePause(j);
                return;
            }
            if (!j->packageReady)
                guard->m_impl->abort(j, error);
        };
        auto accepted = lua->prepareForOwner(
            *j->owner, j->declared.type == run::RunType::FrostEditor, j->attemptStop.get_token(),
            [guard, j, attempt](Result<PreparedDebugLaunch> prepared) mutable {
                if (!guard || j->finished || attempt != j->attempt)
                    return;
                auto& impl = *guard->m_impl;
                if (!prepared) {
                    if (impl.holdForNativePause(j, prepared.error()))
                        return;
                    impl.abort(j, prepared.error());
                    return;
                }
                if (j->stop.stop_requested() || !j->registry || !j->owner || !j->owner->current()) {
                    const Error error(ErrorCode::Cancelled, "Mixed Lua owner ended before adoption");
                    if (impl.holdForNativePause(j, error))
                        return;
                    impl.abort(j, error);
                    return;
                }
                if (!prepared->start || prepared->startPhased) {
                    impl.abort(j, Error(ErrorCode::Corrupt, "Lua attach supplied an unexpected phased member"));
                    return;
                }
                auto started = prepared->start(*j->registry, j->group);
                if (!started) {
                    if (impl.holdForNativePause(j, started.error()))
                        return;
                    impl.abort(j, started.error());
                    return;
                }
                j->lua = *started;
                auto* c = j->registry->controller(*started);
                if (!c) {
                    impl.abort(j, Error(ErrorCode::Internal, "Mixed Lua controller missing"));
                    return;
                }
                j->luaGeneration = c->sessionGeneration();
                impl.phase(j, "lua-configuring");
                if (j->declared.type == run::RunType::FrostPlayer)
                    c->setStopHandler([guard, j, attempt]() -> Result<void> {
                        if (!guard || j->finished || attempt != j->attempt)
                            return fail(ErrorCode::Cancelled, "Mixed Player owner ended");
                        auto* c = guard->m_impl->luaChild(j);
                        if (!c || !c->session() || !guard->m_impl->engine)
                            return fail(ErrorCode::Cancelled, "Mixed Player controller ended");
                        c->session()->stop();
                        return guard->m_impl->engine->stopNativeHost(j->launch.token);
                    });
            },
            true, std::move(lifecycle));
        if (!accepted && !j->finished && attempt == j->attempt && !holdForNativePause(j, accepted.error()))
            abort(j, accepted.error());
    }
    Result<void> start(const std::shared_ptr<Job>& j, ui::DebugSessionRegistry& registry, ui::DebugGroupId group,
                       std::stop_token stop, PreparedDebugLaunch::Started done) {
        if (j->finished || j->phase != "prepared" || j->stop.stop_requested() || stop.stop_requested() || !engine)
            return fail(ErrorCode::Cancelled, "Mixed engine preparation ended");
        if (group == 0 || !done || !j->plan)
            return fail(ErrorCode::InvalidArgument, "Mixed engine needs an actual preparing group and canonical plan");
        j->cancellations.push_back(std::make_unique<Cancellation>(stop, [weak = std::weak_ptr(j)] {
            if (auto job = weak.lock())
                job->stop.request_stop();
        }));
        j->registry = &registry;
        j->group = group;
        j->started = std::move(done);
        auto output = engine->nativeHostOutput(j->launch.token);
        if (!output) {
            abort(j, output.error());
            return {};
        }
        auto id = registry.createSession(QStringLiteral("FrostEngine Native"), group);
        if (!id) {
            abort(j, id.error());
            return {};
        }
        j->native = *id;
        auto* c = registry.controller(*id);
        debug::DebugTarget target;
        target.name = j->declared.name + " (Native)";
        target.program = j->plan->program;
        target.arguments = j->plan->arguments;
        target.workingDirectory = j->plan->workingDirectory;
        target.trustRoots = {j->launch.trustRoot};
        target.stopOnEntry = false;
        environment(target, j->plan->environment);
        auto admission = [check = j->launch.admission, stop = j->stop.get_token()]() -> Result<void> {
            if (stop.stop_requested())
                return fail(ErrorCode::Cancelled, "Mixed native launch cancelled");
            return check();
        };
        auto opened = c->startNativeEngineTarget(
            target, *output, [sink = *output](std::string_view bytes) { sink("adapter-stderr", bytes); },
            std::move(admission));
        if (!opened) {
            (void)registry.remove(*id);
            abort(j, opened.error());
            return {};
        }
        j->nativeGeneration = c->sessionGeneration();
        auto bound = engine->bindNativeHost(j->launch.token, registry, *id, j->nativeGeneration, group);
        if (!bound) {
            c->stop();
            abort(j, bound.error());
            return {};
        }
        j->plan->environment.clear();
        j->launch.configuration.environment.clear();
        j->tick = std::chrono::steady_clock::now();
        phase(j, "native-starting");
        timer->start();
        poll();
        return {};
    }
    void prepared(const std::shared_ptr<Job>& j, Result<run::LaunchPlan> result) {
        if (j->finished)
            return;
        j->taskCancellation.reset();
        j->preparation = 0;
        if (!result) {
            abort(j, result.error());
            return;
        }
        if (j->stop.stop_requested()) {
            abort(j, Error(ErrorCode::Cancelled, "Mixed preparation cancelled"));
            return;
        }
        j->plan = std::move(*result);
        phase(j, "prepared");
        QPointer<EngineMixedLaunchCoordinator> guard(&self);
        auto abandoned = std::shared_ptr<int>(new int(0), [guard, j](int* p) {
            delete p;
            if (guard && !j->finished && j->phase == "prepared")
                guard->m_impl->abort(j, Error(ErrorCode::Cancelled, "Mixed engine preparation abandoned"));
        });
        auto done = std::move(j->prepared);
        PreparedDebugLaunch launch;
        launch.startPhased = [guard, j, abandoned](ui::DebugSessionRegistry& registry, ui::DebugGroupId group,
                                                   std::stop_token stop,
                                                   PreparedDebugLaunch::Started started) -> Result<void> {
            if (!guard)
                return fail(ErrorCode::Cancelled, "Mixed engine coordinator ended");
            return guard->m_impl->start(j, registry, group, stop, std::move(started));
        };
        done(std::move(launch));
    }
    void poll() {
        QPointer<EngineMixedLaunchCoordinator> alive(&self);
        const auto current = jobs;
        for (const auto& j : current) {
            if (!alive)
                return;
            if (j->finished)
                continue;
            if (j->stop.stop_requested()) {
                abort(j, Error(ErrorCode::Cancelled, "Mixed engine package cancelled"));
                continue;
            }
            if (!j->native)
                continue;
            auto* raw = j->registry ? j->registry->controller(j->native) : nullptr;
            if (!raw || raw->sessionGeneration() != j->nativeGeneration || raw->state() == debug::DebugState::Ended) {
                if (raw && raw->sessionGeneration() == j->nativeGeneration + 1 &&
                    raw->state() == debug::DebugState::Ended && raw->lastOutcome()) {
                    debug::DebugOutcome actual;
                    actual.exitCode = raw->lastOutcome()->exitCode;
                    actual.stopped = raw->lastOutcome()->stopped;
                    actual.error = raw->lastOutcome()->error;
                    if (engine)
                        engine->nativeHostEnded(j->launch.token, actual);
                } else if (engine)
                    engine->revokeNativeHost(j->launch.token, true);
                if (!j->packageReady)
                    abort(j, Error(ErrorCode::Cancelled, "Mixed native host ended before Lua was ready"));
                else {
                    j->attemptStop.request_stop();
                    phase(j, "ended");
                    finish(j);
                }
                continue;
            }
            if (!engine) {
                abort(j, Error(ErrorCode::Cancelled, "Engine integration ended"));
                continue;
            }
            auto observed = engine->observeNativeHost(j->launch.token);
            if (!observed) {
                abort(j, observed.error());
                continue;
            }
            j->pid = observed->processId;
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = now - j->tick;
            j->tick = now;
            if (observed->nativeStopped) {
                engine->pauseNativeHostBootstrap(j->launch.token);
                if (!j->packageReady && !j->luaConfigured)
                    cancelAttemptForNativePause(j);
                continue;
            }
            if (j->packageReady) {
                if (j->lua && !luaChild(j))
                    phase(j, "native-only");
                continue;
            }
            j->activeStartup += elapsed;
            if (j->activeStartup >= std::chrono::seconds(60)) {
                abort(j, Error(ErrorCode::Timeout, "Mixed native host and Lua startup did not complete while running"));
                continue;
            }
            if (j->attemptPending)
                continue;
            auto owner = engine->captureNativeLuaDebugOwner(j->launch.token);
            if (!owner) {
                if (owner.error().code() == ErrorCode::Busy)
                    phase(j, "authenticating");
                else
                    abort(j, owner.error());
                continue;
            }
            const std::string_view role = j->declared.type == run::RunType::FrostPlayer ? "player" : "editor";
            if (owner->identity.role != role || owner->identity.processId != j->pid) {
                abort(j, Error(ErrorCode::PermissionDenied, "Mixed authenticated engine role/PID mismatch"));
                continue;
            }
            beginLua(j, std::move(*owner));
        }
    }
};
EngineMixedLaunchCoordinator::EngineMixedLaunchCoordinator(EngineIntegration& engine, EngineLuaDebugCoordinator& lua,
                                                           run::RunManager& tasks, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, engine, lua, tasks)) {}
EngineMixedLaunchCoordinator::~EngineMixedLaunchCoordinator() {
    cancel();
    const std::lock_guard lock(m_impl->relay->mutex);
    m_impl->relay->target = nullptr;
}
Result<void> EngineMixedLaunchCoordinator::prepare(const run::RunConfiguration& configuration, std::stop_token stop,
                                                   MultiSessionDebugCoordinator::Prepared done) {
    if (!done || stop.stop_requested() || !m_impl->engine)
        return fail(ErrorCode::Cancelled, "Mixed engine preparation unavailable/cancelled");
    if (configuration.engineDebugMode != run::EngineDebugMode::NativeAndLua ||
        !run::isFrostEngineRunType(configuration.type))
        return fail(ErrorCode::InvalidArgument, "Choose a typed Native + Lua engine configuration");
    auto reservation = m_impl->engine->reserveNativeHost(configuration);
    if (!reservation)
        return std::unexpected(reservation.error());
    auto j = std::make_shared<Impl::Job>();
    j->launch = std::move(*reservation);
    j->declared = configuration;
    j->prepared = std::move(done);
    for (auto source : {stop, j->launch.projectStop})
        j->cancellations.push_back(std::make_unique<Impl::Cancellation>(source, [weak = std::weak_ptr(j)] {
            if (auto job = weak.lock())
                job->stop.request_stop();
        }));
    m_impl->jobs.push_back(j);
    const auto relay = m_impl->relay;
    run::RunHandlers handlers;
    handlers.launchAdmission = [j]() -> Result<void> {
        if (j->stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Mixed before-task preparation cancelled");
        return j->launch.admission();
    };
    handlers.onFinished = [this, relay, j](const run::RunResult& result) {
        Result<run::LaunchPlan> plan = fail(ErrorCode::Corrupt, "Mixed preparation returned no actual launch plan");
        if (result.error)
            plan = std::unexpected(*result.error);
        else if (result.stopped || j->stop.stop_requested())
            plan = fail(ErrorCode::Cancelled, "Mixed before-tasks stopped");
        else if (result.plan) {
            auto admitted = j->launch.admission();
            if (!admitted)
                plan = std::unexpected(admitted.error());
            else
                plan = j->launch.resolve(*result.plan);
            if (plan) {
                auto still = j->launch.admission();
                if (!still)
                    plan = std::unexpected(still.error());
            }
        }
        relay->post([this, j, plan = std::move(plan)]() mutable { m_impl->prepared(j, std::move(plan)); });
    };
    auto started = m_impl->tasks.start(j->launch.configuration, std::move(handlers), run::RunMode::Prepare);
    if (!started) {
        m_impl->abort(j, started.error());
        return {};
    }
    j->preparation = *started;
    j->taskCancellation = std::make_unique<Impl::Cancellation>(
        j->stop.get_token(), [manager = &m_impl->tasks, id = *started] { manager->stop(id); });
    m_impl->timer->start();
    return {};
}
void EngineMixedLaunchCoordinator::cancel() {
    const auto jobs = m_impl->jobs;
    for (const auto& j : jobs)
        m_impl->abort(j, Error(ErrorCode::Cancelled, "Mixed engine coordinator cancelled"), true);
}
lang::Json EngineMixedLaunchCoordinator::snapshot() const {
    return m_impl->jobs.empty() ? m_impl->last : m_impl->describe(m_impl->jobs.back());
}
} // namespace frost::app
