// SPDX-License-Identifier: Apache-2.0
// Private engine Lua DAP preparation and exact owned-Play lifetime (GUI coordinator).
#pragma once
#include "frost/debug/lua_debug_endpoint.hpp"
#include "frost/engine/session.hpp"

#include "multi_session_debug.hpp"
#include <QObject>

#include <memory>
#include <optional>
#include <stop_token>

namespace frost::app {
class EngineIntegration;
// Opaque admission closures bind physical project, trust revision and exact R6 connection. They
// neither grant process ownership nor expose bootstrap credentials. current() is GUI-thread only.
struct EngineLuaDebugOwner {
    engine::EngineIdentity identity;
    std::string root;
    std::uint64_t epoch = 0, sessionEpoch = 0, trustRevision = 0;
    std::function<bool()> current;
    std::function<bool()> nativePaused;
    debug::LuaDebugAdmission admit;
    std::stop_token projectStop, connectionStop;
};

struct EngineLuaDebugBinding {
    ui::DebugSessionId registrySessionId = 0;
    std::uint64_t controllerGeneration = 0, epoch = 0, sessionEpoch = 0, trustRevision = 0;
    debug::LuaDebugIdentity identity;
    std::string root;
    std::function<bool()> current; // GUI-only; exact immutable registry/controller/engine owner.
};

enum class EngineLuaDebugInterruption : std::uint8_t { NativePaused, Cancelled, Failed };
struct EngineLuaDebugLifecycle {
    std::function<void(const EngineLuaDebugBinding&)> configured;
    std::function<void(const EngineLuaDebugBinding&)> ready;
    std::function<void(EngineLuaDebugInterruption, const Error&)> interrupted;
};

class EngineLuaDebugCoordinator final : public QObject {
    Q_OBJECT
public:
    using Prepared = MultiSessionDebugCoordinator::Prepared;
    explicit EngineLuaDebugCoordinator(EngineIntegration& engine, QObject* parent = nullptr);
    ~EngineLuaDebugCoordinator() override;
    // Prepare against an already authenticated selected engine. The caller first starts/waits for
    // an owned RUN-11 editor when required; pairing an external editor never grants host ownership.
    // startPlay uses real play.start after configurationDone ACK. false is ordinary attach.
    [[nodiscard]] Result<void> prepare(bool startPlay, std::stop_token stop, Prepared completed,
                                       bool stopOnEntry = true, EngineLuaDebugLifecycle lifecycle = {});
    // Preserves an additional exact native-host/run owner wrapper; never recaptures broader selected authority.
    [[nodiscard]] Result<void> prepareForOwner(EngineLuaDebugOwner owner, bool startPlay, std::stop_token stop,
                                               Prepared completed, bool stopOnEntry = true,
                                               EngineLuaDebugLifecycle lifecycle = {});
    // Exact engine-selected scene only. No CLI, synthetic VM or individual runtime case selector.
    [[nodiscard]] Result<void> debugScene(ui::DebugSessionRegistry& sessions, std::string scene, std::uint64_t frames,
                                          std::stop_token stop = {}, EngineLuaDebugLifecycle lifecycle = {});
    [[nodiscard]] Result<void> startAttached(ui::DebugSessionRegistry& sessions, bool startPlay = true);
    // Cancels only this unfinished child, marking local attached preparation cancellation before
    // revoking its token. Busy means actual configuration is complete; do not cancel its owner token.
    [[nodiscard]] Result<void>
    cancelPreparation(ui::DebugSessionId id, std::uint64_t controllerGeneration,
                      EngineLuaDebugInterruption reason = EngineLuaDebugInterruption::Cancelled);
    void cancel();
    [[nodiscard]] std::optional<EngineLuaDebugBinding> binding(ui::DebugSessionId id) const;
signals:
    void failed(const QString& message);
    void bindingChanged(std::uint64_t registrySessionId);
    void playStartObserved(std::uint64_t registrySessionId, bool startedNow, const QString& playGeneration);
    // Only actual Edit after confirmed detach and the exact guarded Stop ACK.
    void ownedPlayStopped(std::uint64_t registrySessionId, const QString& playGeneration);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::app
