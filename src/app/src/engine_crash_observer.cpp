// SPDX-License-Identifier: Apache-2.0
// Capture while alive, observe only unexpected loss, and offer immutable locally validated bytes.
#include "engine_crash_observer.hpp"

#include "frost/lang/uri.hpp"

#include <QPointer>
#include <QTimer>

#include <chrono>
#include <stop_token>
#include <utility>

namespace frost::app {
namespace {
bool sameIdentity(const engine::EngineIdentity& a, const engine::EngineIdentity& b) {
    return a.instanceId == b.instanceId && a.generation == b.generation &&
           a.connectionGeneration == b.connectionGeneration && a.projectPath == b.projectPath &&
           a.processId == b.processId && a.crashArtifactSource == b.crashArtifactSource;
}
engine::EngineIdentity smallIdentity(engine::EngineIdentity identity) {
    identity.schema = rpc::Json{};
    identity.availableCommands.clear();
    return identity;
}
} // namespace

void EngineCrashSessionHolder::authenticated(const engine::EngineIdentity& identity) {
    auto lease = std::make_shared<EngineCrashSessionLease>();
    lease->m_identity = smallIdentity(identity);
    std::scoped_lock lock(m_mutex);
    if (m_current) {
        m_current->m_revoked.store(true);
        m_current->m_live.store(false);
    }
    m_current = std::move(lease);
}
bool EngineCrashSessionHolder::lost(const engine::EngineIdentity& identity, bool unexpected) {
    std::scoped_lock lock(m_mutex);
    if (!m_current || !sameIdentity(m_current->identity(), identity))
        return false;
    m_current->m_live.store(false);
    if (!unexpected)
        m_current->m_revoked.store(true);
    return true;
}
void EngineCrashSessionHolder::revoke() {
    std::scoped_lock lock(m_mutex);
    if (m_current) {
        m_current->m_revoked.store(true);
        m_current->m_live.store(false);
        m_current.reset();
    }
}
std::shared_ptr<const EngineCrashSessionLease> EngineCrashSessionHolder::snapshot() const {
    std::scoped_lock lock(m_mutex);
    return m_current;
}

struct EngineCrashObserver::Impl {
    struct Observation {
        EngineCrashObservationSource source;
        std::stop_source stop;
        // Exclusively accessed by serialized work batch jobs, then destroyed after their ownership ends.
        std::unique_ptr<engine::CrashArtifactObserver> observer;
        std::optional<Error> error;
        engine::CrashArtifactScan scan;
        std::atomic<bool> done{false};
        bool capturing = true, inFlight = false;
        std::atomic<bool> lost{false};
        IClock::SteadyTime deadline{}, nextPoll{};
    };
    EngineCrashObserver& owner;
    EngineCrashObserverOptions options;
    QTimer timer;
    std::shared_ptr<Observation> current;
    EngineCrashObservationStatus status;
    bool stopped = false;

    Impl(EngineCrashObserver& owner_, EngineCrashObserverOptions options_)
        : owner(owner_), options(std::move(options_)), timer(&owner_) {
        timer.setInterval(20);
        QObject::connect(&timer, &QTimer::timeout, &owner, [this] { owner.poll(); });
    }
    void fail(Error error) {
        if (current)
            current->stop.request_stop();
        status.phase = EngineCrashObservationPhase::Failed;
        status.error = std::move(error);
        timer.stop();
        if (options.notifications)
            options.notifications->notify({QStringLiteral("Crash observation unavailable"),
                                           QString::fromStdString(status.error->message()),
                                           ui::NotificationKind::Warning,
                                           std::nullopt,
                                           {}});
    }
    void submit(const std::shared_ptr<Observation>& value) {
        auto* fs = options.fs;
        auto* clock = options.clock;
        const bool capturing = value->capturing;
        auto submitted = options.work->submit([value, fs, clock, capturing] {
            const auto run = [&]() -> Result<void> {
                if (value->stop.stop_requested())
                    return frost::fail(ErrorCode::Cancelled, "Crash observation was revoked");
                if (auto admitted = value->source.admission.validatePhysical(); !admitted)
                    return admitted;
                if (capturing) {
                    if (value->lost.load() || !value->source.live())
                        return frost::fail(ErrorCode::Cancelled,
                                           "The authenticated engine ended before baseline capture");
                    auto observer = engine::CrashArtifactObserver::capture(
                        *fs, *value->source.identity.crashArtifactSource, *clock);
                    if (!observer)
                        return std::unexpected(std::move(observer.error()));
                    if (value->stop.stop_requested() || value->lost.load() || !value->source.live())
                        return frost::fail(ErrorCode::Cancelled,
                                           "The authenticated engine ended during baseline capture");
                    value->observer = std::move(*observer);
                } else {
                    auto scan = value->observer->poll(*fs, value->stop.get_token());
                    if (!scan)
                        return std::unexpected(std::move(scan.error()));
                    value->scan = std::move(*scan);
                }
                return value->source.admission.validatePhysical();
            };
            auto result = run();
            if (!result)
                value->error = std::move(result.error());
            value->done.store(true, std::memory_order_release);
        });
        if (!submitted) {
            if (submitted.error().code() != ErrorCode::Busy)
                fail(std::move(submitted.error()));
            return;
        }
        value->inFlight = true;
        if (!capturing)
            ++status.attempts;
    }
};

EngineCrashObserver::EngineCrashObserver(EngineCrashObserverOptions options, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(options))) {}
EngineCrashObserver::~EngineCrashObserver() { shutdown(); }

Result<void> EngineCrashObserver::observe(EngineCrashObservationSource source) {
    auto& impl = *m_impl;
    if (impl.stopped)
        return fail(ErrorCode::Cancelled, "Crash observation has shut down");
    revoke();
    if (!impl.options.fs || !impl.options.clock || !impl.options.work || !impl.options.dumps ||
        !source.identity.crashArtifactSource || !source.live || !source.admission.current ||
        !source.admission.validatePhysical)
        return fail(ErrorCode::Unsupported, "The authenticated engine has no usable crash observation source");
    if (source.admission.projectRoot.empty() ||
        lang::documentKey(source.admission.projectRoot) != lang::documentKey(source.identity.projectPath) ||
        source.identity.crashArtifactSource->projectPath != source.identity.projectPath)
        return fail(ErrorCode::PermissionDenied, "Crash observation belongs to a different physical project");
    if (auto admitted = source.admission.current(); !admitted)
        return admitted;
    if (!source.live())
        return fail(ErrorCode::Cancelled, "Crash baseline capture requires this authenticated live session");
    impl.current = std::make_shared<Impl::Observation>();
    impl.current->source = std::move(source);
    impl.current->deadline = impl.options.clock->steadyNow() + std::chrono::seconds(5);
    impl.status.phase = EngineCrashObservationPhase::Capturing;
    impl.timer.start();
    poll();
    return {};
}
void EngineCrashObserver::unexpectedLoss(const engine::EngineIdentity& identity) {
    auto& impl = *m_impl;
    if (!impl.current || !sameIdentity(impl.current->source.identity, identity) || impl.current->lost.load() ||
        impl.current->source.live())
        return;
    impl.current->lost.store(true);
    impl.current->deadline = impl.options.clock->steadyNow() + std::chrono::seconds(5);
    impl.current->nextPoll = impl.options.clock->steadyNow();
    impl.timer.start();
    poll();
}
void EngineCrashObserver::revoke() {
    auto& impl = *m_impl;
    if (impl.current)
        impl.current->stop.request_stop();
    impl.current.reset();
    impl.timer.stop();
    impl.status = {};
    if (impl.options.dumps)
        impl.options.dumps->revoke();
}
void EngineCrashObserver::shutdown() {
    m_impl->stopped = true;
    revoke();
}
EngineCrashObservationStatus EngineCrashObserver::status() const { return m_impl->status; }

void EngineCrashObserver::poll() {
    auto& impl = *m_impl;
    const auto value = impl.current;
    if (impl.stopped || !value || impl.status.phase == EngineCrashObservationPhase::Failed ||
        impl.status.phase == EngineCrashObservationPhase::Finished ||
        impl.status.phase == EngineCrashObservationPhase::Offered)
        return;
    if (auto admitted = value->source.admission.current(); !admitted) {
        revoke();
        return;
    }
    if ((value->capturing || value->lost.load()) && impl.options.clock->steadyNow() >= value->deadline) {
        value->stop.request_stop();
        impl.status.phase = EngineCrashObservationPhase::Finished;
        impl.status.error = Error(ErrorCode::Timeout,
                                  value->capturing ? "No live crash baseline was captured before the deadline"
                                                   : "No completed local crash dump was observed before the deadline");
        impl.timer.stop();
        return;
    }
    if (value->inFlight) {
        if (!value->done.load(std::memory_order_acquire))
            return;
        value->inFlight = false;
        if (value->error) {
            impl.fail(*value->error);
            return;
        }
        if (value->capturing) {
            value->capturing = false;
            impl.status.phase = EngineCrashObservationPhase::Armed;
        } else {
            impl.status.partialReports += value->scan.partialReports;
            impl.status.rejectedReports += value->scan.rejectedReports;
            if (value->scan.artifact) {
                QPointer<EngineCrashObserver> self(this);
                auto offered = impl.options.dumps->offer(std::move(value->scan.artifact), value->source.admission);
                if (!self || impl.current != value || value->stop.stop_requested())
                    return;
                if (!offered) {
                    impl.fail(std::move(offered.error()));
                    return;
                }
                impl.status.phase = EngineCrashObservationPhase::Offered;
                impl.timer.stop();
                return;
            }
            value->nextPoll = impl.options.clock->steadyNow() + std::chrono::milliseconds(200);
        }
        value->scan = {};
        value->done.store(false, std::memory_order_release);
    }
    if (!value->capturing && !value->lost.load()) {
        impl.timer.stop();
        return;
    }
    const auto now = impl.options.clock->steadyNow();
    if (impl.status.attempts >= 32) {
        value->stop.request_stop();
        impl.status.phase = EngineCrashObservationPhase::Finished;
        impl.status.error = Error(ErrorCode::Timeout, "Crash observation exhausted its bounded scan attempts");
        impl.timer.stop();
        return;
    }
    if (!value->capturing) {
        impl.status.phase = EngineCrashObservationPhase::Observing;
        if (now < value->nextPoll)
            return;
    }
    impl.submit(value);
}

} // namespace frost::app
