// SPDX-License-Identifier: Apache-2.0
// Exact owned host launch precedes authentication/DAP; host lifetime never follows external attach.
#include "engine_run_debug.hpp"

#include "frost/ui/run_controller.hpp"

#include "engine_integration.hpp"
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace frost::app {
namespace {
bool waitName(std::string_view name) {
    constexpr std::string_view expected = "FROST_SCRIPT_DEBUG_WAIT";
    if (name.size() != expected.size())
        return false;
    return std::ranges::equal(name, expected, [](char a, char b) {
        return (a >= 'a' && a <= 'z' ? static_cast<char>(a - 'a' + 'A') : a) == b;
    });
}
} // namespace
Result<run::RunConfiguration> engineDebugHostConfiguration(const run::RunConfiguration& configuration) {
    if (!run::isFrostEngineRunType(configuration.type))
        return fail(ErrorCode::InvalidArgument, "Debug host must be a FrostEditor or Player configuration");
    if (auto valid = run::validateRunConfiguration(configuration); !valid)
        return std::unexpected(valid.error());
    auto result = configuration;
    std::erase_if(result.environment, [](const auto& e) { return waitName(e.name); });
    result.environment.push_back({"FROST_SCRIPT_DEBUG_WAIT", configuration.type == run::RunType::FrostPlayer
                                                                 ? std::optional<std::string>("1")
                                                                 : std::nullopt});
    return result;
}
struct EngineRunDebugCoordinator::Impl {
    using Cancellation = std::stop_callback<std::function<void()>>;
    enum class Phase { WaitingHost, WaitingLua, Prepared, Adopted, Finished };
    struct Job {
        run::RunConfiguration configuration;
        run::RunId run = 0;
        std::stop_source stop;
        std::unique_ptr<Cancellation> cancellation;
        MultiSessionDebugCoordinator::Prepared completed;
        std::optional<EngineLuaDebugOwner> owner;
        std::optional<std::chrono::steady_clock::time_point> deadline;
        QPointer<ui::DebugSessionRegistry> registry;
        ui::DebugSessionId session = 0;
        std::uint64_t controllerGeneration = 0;
        Phase phase = Phase::WaitingHost;
    };
    EngineRunDebugCoordinator& self;
    ui::RunController& runs;
    run::RunManager& manager;
    EngineRunDebugHooks hooks;
    EngineRunDebugOptions options;
    QTimer* timer;
    std::vector<std::shared_ptr<Job>> jobs;
    Impl(EngineRunDebugCoordinator& s, ui::RunController& r, run::RunManager& m, EngineRunDebugHooks h,
         EngineRunDebugOptions o)
        : self(s), runs(r), manager(m), hooks(std::move(h)), options(o), timer(new QTimer(&s)) {
        timer->setInterval(25);
        QObject::connect(timer, &QTimer::timeout, &self, [this] { poll(); });
    }
    bool owns(const std::shared_ptr<Job>& job) const {
        const auto status = manager.status(job->run);
        return status && status->configuration.id == job->configuration.id &&
               status->configuration.type == job->configuration.type &&
               status->configuration.program == job->configuration.program &&
               status->configuration.engineProjectPath == job->configuration.engineProjectPath;
    }
    ui::DebugController* child(const std::shared_ptr<Job>& job) const {
        auto* c = job->registry ? job->registry->controller(job->session) : nullptr;
        return c && c->sessionGeneration() == job->controllerGeneration ? c : nullptr;
    }
    void stopHost(const std::shared_ptr<Job>& job) {
        if (owns(job))
            manager.stop(job->run);
    }
    void finish(const std::shared_ptr<Job>& job, bool revoke = true) {
        job->phase = Phase::Finished;
        if (revoke)
            job->stop.request_stop();
        job->completed = {};
        std::erase(jobs, job);
        if (jobs.empty())
            timer->stop();
    }
    void abort(const std::shared_ptr<Job>& job, Error error) {
        if (job->phase == Phase::Finished)
            return;
        const bool adopted = job->phase == Phase::Adopted;
        if (auto* c = child(job); c && c->session())
            c->session()->stop(); // Ordinary detach, never a host kill timer.
        if (!adopted || job->configuration.type == run::RunType::FrostPlayer)
            stopHost(job);
        auto completed = std::move(job->completed);
        finish(job);
        if (completed)
            completed(std::unexpected(error));
        else if (error.code() != ErrorCode::Cancelled)
            emit self.failed(QString::fromStdString(error.message()));
    }
    Result<void> stopPlayer(const std::shared_ptr<Job>& job) {
        auto* c = child(job);
        if (job->phase != Phase::Adopted || job->configuration.type != run::RunType::FrostPlayer || !c ||
            !c->session() || !owns(job))
            return fail(ErrorCode::Cancelled, "The exact owned Player debug run ended");
        // Explicit process ownership authorizes termination. DAP detach is sent first, but its ACK
        // may be interrupted by that actual owned-process termination; do not claim graceful detach.
        if (hooks.explicitStop)
            hooks.explicitStop(job->run);
        c->session()->stop();
        stopHost(job);
        return {};
    }
    void prepared(const std::shared_ptr<Job>& job, Result<PreparedDebugLaunch> result) {
        if (job->phase != Phase::WaitingLua)
            return;
        if (!result) {
            abort(job, result.error());
            return;
        }
        if (job->stop.stop_requested() || !owns(job) || !job->owner || !job->owner->current()) {
            abort(job, Error(ErrorCode::Cancelled, "Owned engine changed during Lua preparation"));
            return;
        }
        if (!result->start) {
            abort(job, Error(ErrorCode::Corrupt, "Lua preparation returned no adapter launch"));
            return;
        }
        job->phase = Phase::Prepared;
        job->deadline.reset();
        QPointer<EngineRunDebugCoordinator> guard(&self);
        // Only Prepared.start retains this guard. Dropping an unconsumed compound member stops its
        // newly launched host and revokes private DAP preparation, without touching another run.
        auto abandoned = std::shared_ptr<int>(new int(0), [guard, job](int* p) {
            delete p;
            if (guard && job->phase == Phase::Prepared)
                guard->m_impl->abort(job, Error(ErrorCode::Cancelled, "Owned engine Debug preparation was abandoned"));
        });
        auto completed = std::move(job->completed);
        completed(PreparedDebugLaunch{
            [guard, job, abandoned, inner = std::move(*result)](
                ui::DebugSessionRegistry& registry, ui::DebugGroupId group) mutable -> Result<ui::DebugSessionId> {
                if (!guard || job->phase != Phase::Prepared || job->stop.stop_requested() || !job->owner ||
                    !job->owner->current() || !guard->m_impl->owns(job))
                    return fail(ErrorCode::Cancelled, "Prepared owned engine Debug owner ended");
                auto started = inner.start(registry, group);
                if (!started) {
                    guard->m_impl->abort(job, started.error());
                    return std::unexpected(started.error());
                }
                auto* c = registry.controller(*started);
                if (!c) {
                    guard->m_impl->abort(job, Error(ErrorCode::Internal, "Lua registry controller missing"));
                    return fail(ErrorCode::Internal, "Lua registry controller missing");
                }
                job->registry = &registry;
                job->session = *started;
                job->controllerGeneration = c->sessionGeneration();
                job->phase = Phase::Adopted;
                if (job->configuration.type == run::RunType::FrostPlayer)
                    c->setStopHandler([guard, job]() -> Result<void> {
                        if (!guard)
                            return fail(ErrorCode::Cancelled, "Owned engine Debug coordinator ended");
                        return guard->m_impl->stopPlayer(job);
                    });
                return *started;
            }});
    }
    void poll() {
        const auto snapshot = jobs;
        QPointer<EngineRunDebugCoordinator> alive(&self);
        for (const auto& job : snapshot) {
            if (!alive)
                return;
            if (job->phase == Phase::Finished)
                continue;
            if (job->stop.stop_requested()) {
                abort(job, Error(ErrorCode::Cancelled, "Owned engine Debug launch cancelled"));
                continue;
            }
            const auto status = manager.status(job->run);
            if (!status || !owns(job)) {
                if (job->phase == Phase::Adopted) {
                    if (auto* c = child(job); c && c->session())
                        c->session()->stop();
                    finish(job);
                } else
                    abort(job, Error(ErrorCode::Cancelled, "Owned engine run ended before Lua Debug was ready"));
                continue;
            }
            if (job->phase == Phase::Adopted) {
                auto* c = child(job);
                if (!c || c->state() == debug::DebugState::Ended) {
                    const bool player = job->configuration.type == run::RunType::FrostPlayer;
                    if (player)
                        stopHost(job);
                    // Editor Ended can precede FER-9's detach ACK and expected-generation
                    // play.stop. Do not cancel its supplied token while that cleanup is retained.
                    finish(job, player);
                } else if (!job->owner || !job->owner->current())
                    abort(job, Error(ErrorCode::Cancelled, "Authenticated owned engine identity changed"));
                continue;
            }
            if (status->phase == run::RunPhase::Stopping) {
                abort(job, Error(ErrorCode::Cancelled, "Owned engine run is stopping"));
                continue;
            }
            if (status->phase == run::RunPhase::BeforeLaunch)
                continue; // Existing before-task deadlines/admission remain RUN-11's.
            if (job->phase == Phase::Prepared) {
                if (!job->owner || !job->owner->current())
                    abort(job, Error(ErrorCode::Cancelled, "Prepared owned engine identity changed"));
                continue;
            }
            if (!job->deadline)
                job->deadline = std::chrono::steady_clock::now() + options.startupTimeout;
            if (std::chrono::steady_clock::now() >= *job->deadline) {
                abort(job, Error(ErrorCode::Timeout, "Owned engine authentication/Lua Debug preparation timed out"));
                continue;
            }

            if (job->phase == Phase::WaitingLua && (!job->owner || !job->owner->current())) {
                abort(job, Error(ErrorCode::Cancelled,
                                 "Authenticated owned engine identity changed during DAP preparation"));
                continue;
            }
            if (job->phase != Phase::WaitingHost)
                continue;
            auto owner = hooks.capture(job->run);
            if (!alive)
                return;
            if (!owner) {
                if (owner.error().code() != ErrorCode::Busy)
                    abort(job, owner.error());
                continue;
            }
            const std::string_view role = job->configuration.type == run::RunType::FrostPlayer ? "player" : "editor";
            if (owner->identity.role != role || !owner->current || !owner->current()) {
                abort(job, Error(ErrorCode::PermissionDenied,
                                 "Authenticated engine role/owner differs from the launched Debug host"));
                continue;
            }
            job->owner = std::move(*owner);
            job->phase = Phase::WaitingLua;
            job->deadline = std::chrono::steady_clock::now() + options.startupTimeout;
            QPointer<EngineRunDebugCoordinator> guard(&self);
            auto queued = hooks.prepareLua(
                job->configuration.type == run::RunType::FrostEditor, job->stop.get_token(),
                [guard, job](Result<PreparedDebugLaunch> r) mutable {
                    if (guard)
                        guard->m_impl->prepared(job, std::move(r));
                },
                true);
            if (!alive)
                return;
            if (!queued && job->phase == Phase::WaitingLua)
                abort(job, queued.error());
        }
    }
};
EngineRunDebugCoordinator::EngineRunDebugCoordinator(ui::RunController& runs, run::RunManager& manager,
                                                     EngineIntegration& engine, EngineLuaDebugCoordinator& lua,
                                                     QObject* parent)
    : EngineRunDebugCoordinator(
          runs, manager,
          EngineRunDebugHooks{
              [guard = QPointer<EngineIntegration>(&engine)](run::RunId id) -> Result<EngineLuaDebugOwner> {
                  if (!guard)
                      return fail(ErrorCode::Cancelled, "Engine integration ended");
                  return guard->captureOwnedLuaDebugOwner(id);
              },
              [guard = QPointer<EngineLuaDebugCoordinator>(&lua)](bool play, std::stop_token stop,
                                                                  MultiSessionDebugCoordinator::Prepared done,
                                                                  bool entry) -> Result<void> {
                  if (!guard)
                      return fail(ErrorCode::Cancelled, "Lua coordinator ended");
                  return guard->prepare(play, stop, std::move(done), entry);
              },
              [guard = QPointer<EngineIntegration>(&engine)](run::RunId id) {
                  if (guard)
                      guard->explicitRunStop(id);
              }},
          {}, parent) {
    connect(&engine, &EngineIntegration::stateObserved, this, [this] { m_impl->poll(); });
}
EngineRunDebugCoordinator::EngineRunDebugCoordinator(ui::RunController& runs, run::RunManager& manager,
                                                     EngineRunDebugHooks hooks, EngineRunDebugOptions options,
                                                     QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, runs, manager, std::move(hooks), options)) {}
EngineRunDebugCoordinator::~EngineRunDebugCoordinator() { cancel(); }
Result<void> EngineRunDebugCoordinator::prepare(const run::RunConfiguration& configuration, std::stop_token stop,
                                                MultiSessionDebugCoordinator::Prepared completed) {
    if (configuration.engineDebugMode != run::EngineDebugMode::Lua)
        return fail(ErrorCode::InvalidArgument, "Native + Lua requires the phased native engine launcher");
    if (!completed || !m_impl->hooks.capture || !m_impl->hooks.prepareLua ||
        m_impl->options.startupTimeout.count() <= 0)
        return fail(ErrorCode::InvalidArgument,
                    "Owned engine Debug requires complete services and a positive deadline");
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Owned engine Debug launch cancelled");
    if (m_impl->jobs.size() >= 32)
        return fail(ErrorCode::Busy, "Too many owned engine Debug preparations");
    auto transient = engineDebugHostConfiguration(configuration);
    if (!transient)
        return std::unexpected(transient.error());
    auto job = std::make_shared<Impl::Job>();
    job->configuration = configuration;
    job->completed = std::move(completed);
    job->cancellation = std::make_unique<Impl::Cancellation>(stop, [weak = std::weak_ptr(job)] {
        if (auto j = weak.lock())
            j->stop.request_stop();
    });
    auto started = m_impl->runs.runEngineConfiguration(*transient);
    if (!started)
        return std::unexpected(started.error());
    job->run = *started;
    m_impl->jobs.push_back(job);
    m_impl->timer->start();
    m_impl->poll();
    return {};
}
void EngineRunDebugCoordinator::cancel() {
    const auto jobs = m_impl->jobs;
    for (const auto& job : jobs)
        m_impl->abort(job, Error(ErrorCode::Cancelled, "Owned engine Debug coordinator cancelled"));
}
} // namespace frost::app
