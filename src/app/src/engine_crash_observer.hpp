// SPDX-License-Identifier: Apache-2.0
// FER-16 live baseline capture and bounded local crash observation after authenticated loss.
#pragma once

#include "frost/engine/session.hpp"

#include "engine_dump_controller.hpp"
#include <QObject>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace frost::app {

// Immutable authenticated identity with independent live and historical admission. Authentication
// replaces the lease. Unexpected loss clears live only; explicit revocation clears both forever.
// Methods are thread-safe, bounded by the captured identity size, and perform no filesystem operations.
class EngineCrashSessionLease final {
public:
    [[nodiscard]] const engine::EngineIdentity& identity() const noexcept { return m_identity; }
    [[nodiscard]] bool live() const noexcept { return m_live.load() && !m_revoked.load(); }
    [[nodiscard]] bool admitted() const noexcept { return !m_revoked.load(); }

private:
    friend class EngineCrashSessionHolder;
    engine::EngineIdentity m_identity;
    std::atomic<bool> m_live{true}, m_revoked{false};
};

// Called from actual EngineSession callbacks and GUI explicit-lifecycle actions. No QObject or
// EngineSession pointer escapes into a worker admission closure. Old leases never revive.
class EngineCrashSessionHolder final {
public:
    void authenticated(const engine::EngineIdentity& identity);
    [[nodiscard]] bool lost(const engine::EngineIdentity& identity, bool unexpected);
    void revoke();
    [[nodiscard]] std::shared_ptr<const EngineCrashSessionLease> snapshot() const;

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<EngineCrashSessionLease> m_current;
};

// Pure cleanup decision, not a crash verdict. Retains only the same admitted historical loss,
// or permits observation of an actual unexpected exit. Explicit Stop revokes the holder first.
[[nodiscard]] bool preserveEngineCrashObservation(const std::shared_ptr<const EngineCrashSessionLease>& lease,
                                                  std::int64_t expectedPid, bool stopped,
                                                  std::optional<int> exitCode) noexcept;

struct EngineCrashObservationSource {
    engine::EngineIdentity identity;
    EngineDumpAdmission admission;
    // Worker-safe atomic/logical check of this exact still-live authentication. No QObject access.
    std::function<bool()> live;
};

enum class EngineCrashObservationPhase : std::uint8_t { Idle, Capturing, Armed, Observing, Offered, Finished, Failed };
struct EngineCrashObservationStatus {
    EngineCrashObservationPhase phase = EngineCrashObservationPhase::Idle;
    std::uint32_t attempts = 0, partialReports = 0, rejectedReports = 0;
    std::optional<Error> error;
};
struct EngineCrashObserverOptions {
    io::IFileSystem* fs = nullptr;
    IClock* clock = nullptr;
    std::shared_ptr<EngineDumpWorkBatch> work;
    EngineDumpController* dumps = nullptr;
    ui::NotificationCenter* notifications = nullptr;
};

// GUI coordinator with one exclusively worker-owned observer. observe() is a cached admission
// operation; all baseline, metadata and dump reads run on the shared dump work batch. Poll retries
// end after five steady seconds or 32 attempts. Borrowed services outlive WorkBatch::drain(),
// which the host calls after GUI/controller teardown. shutdown/revoke never join or perform I/O.
class EngineCrashObserver final : public QObject {
public:
    explicit EngineCrashObserver(EngineCrashObserverOptions options, QObject* parent = nullptr);
    ~EngineCrashObserver() override;
    // Replaces prior observation/offers even when the new source cannot be admitted.
    [[nodiscard]] Result<void> observe(EngineCrashObservationSource source);
    void unexpectedLoss(const engine::EngineIdentity& identity);
    void revoke();
    void shutdown();
    // GUI-only bounded completion collection/scheduling; also driven by a Qt timer.
    void poll();
    [[nodiscard]] EngineCrashObservationStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace frost::app
