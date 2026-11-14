// SPDX-License-Identifier: Apache-2.0
// App coordinator for authenticated FrostEngine sessions, pairing, push UI and owned run types.
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/json.hpp"
#include "frost/run/run_manager.hpp"

#include "engine_native_host.hpp"
#include "workbench_engine_lua_debug.hpp"
#include <QObject>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::app {
class Workbench;
struct EngineUnitTestsSource;
struct EngineCrashObservationSource;
struct EngineNativeDebugBinding;

// GUI-thread coordinator. Sessions and background work are stopped before the scheduler dies.
// Pairing never grants project trust or process ownership. Only RUN-11 runs are owned children.
class EngineIntegration final : public QObject {
    Q_OBJECT
public:
    explicit EngineIntegration(Workbench& workbench);
    ~EngineIntegration() override;
    // Cancels project/session work and owned runs; a paired external editor remains alive.
    void cancel();
    void trustChanged();
    // GUI native adapter observer; publishes typed actual event evidence to the exact R6 lease.
    void updateNativeDebuggerPause();
    void shutdown();

    [[nodiscard]] Result<void> configureTools(std::string editorPath, std::string configDirectory,
                                              std::string automationDirectory, std::vector<std::string> arguments);
    [[nodiscard]] Result<void> refreshInstances();
    [[nodiscard]] Result<void> pair(std::string_view instanceId);
    void disconnect();
    [[nodiscard]] Result<std::uint64_t> command(std::string method, lang::Json params);
    [[nodiscard]] Result<build::RunId> startEditor();
    [[nodiscard]] Result<void> validateScript();
    [[nodiscard]] Result<void> cancelRequest();
    [[nodiscard]] lang::Json snapshot() const;
    // GUI-only immutable CLI/project capture; returned admission is worker-safe and revision-bound.
    [[nodiscard]] Result<EngineUnitTestsSource> captureEngineUnitTestsSource() const;
    // GUI cached capture. Returned live/historical admission closures are worker-safe and QObject-free.
    [[nodiscard]] Result<EngineCrashObservationSource> captureCrashObservationSource() const;
    // GUI-only current authenticated binding; registry selection and stale retained proofs grant no match.
    [[nodiscard]] std::optional<EngineNativeDebugBinding> nativeDebugBinding() const;
    [[nodiscard]] std::vector<run::RunConfiguration> configurations() const;
    [[nodiscard]] Result<run::RunConfiguration> prepareRun(const run::RunConfiguration& configuration);
    // GUI factory; returned immutable closure is safe on the RunManager dispatcher and outlives
    // this coordinator. It checks the captured reservation and trust revision at actual launch.
    [[nodiscard]] std::function<Result<void>()> launchAdmissionFor(const run::RunConfiguration& configuration) const;
    [[nodiscard]] std::function<Result<run::LaunchPlan>(run::LaunchPlan)>
    launchPlanFor(const run::RunConfiguration& configuration) const;
    void runAccepted(run::RunId id, const run::RunConfiguration& configuration);
    void processStarted(const run::RunStatus& run);
    void processOutput(run::RunId run, run::OutputStream stream, std::string_view bytes);
    void processFinished(const run::RunResult& result);
    void explicitRunStop(run::RunId id); // GUI user Stop/rerun, exact owned run only; revokes crash observation.

    [[nodiscard]] Result<EngineLuaDebugOwner> captureLuaDebugOwner() const;
    // GUI only: actual authenticated owner of this exact IDE-owned RUN-11 run. Busy while its
    // endpoint/authentication is pending; Cancelled after ownership changes. No external pairing.
    [[nodiscard]] Result<EngineLuaDebugOwner> captureOwnedLuaDebugOwner(run::RunId id) const;
    // GUI native-launch reservation. No host exists until a private built-in DAP launch binds its
    // actual registry session/generation; output sinks are worker-safe and discard raw UI output.
    [[nodiscard]] Result<EngineNativeHostLaunch> reserveNativeHost(const run::RunConfiguration& configuration);
    [[nodiscard]] Result<void> bindNativeHost(EngineNativeHostToken token, ui::DebugSessionRegistry& registry,
                                              ui::DebugSessionId session, std::uint64_t generation,
                                              ui::DebugGroupId group);
    [[nodiscard]] Result<EngineNativeHostObservation> observeNativeHost(EngineNativeHostToken token);
    void pauseNativeHostBootstrap(EngineNativeHostToken token);
    void revokeNativeHost(EngineNativeHostToken token, bool preserveUnexpectedLoss = false);
    [[nodiscard]] Result<EngineNativePrivateOutput> nativeHostOutput(EngineNativeHostToken token) const;
    // Poll privately on GUI: actual native process/config ACK, endpoint and full HMAC PID match.
    // Busy means pending/native-paused; errors do not grant ownership of any replacement host.
    [[nodiscard]] Result<EngineLuaDebugOwner> captureNativeLuaDebugOwner(EngineNativeHostToken token);
    [[nodiscard]] Result<void> stopNativeHost(EngineNativeHostToken token);
    void nativeHostEnded(EngineNativeHostToken token, const debug::DebugOutcome& outcome);

    using LuaConnected = std::function<void(Result<debug::LuaDebugConnection>)>;
    [[nodiscard]] Result<void> connectLuaDebug(const EngineLuaDebugOwner& owner, debug::LuaDebugAdmission admission,
                                               std::stop_token stop, debug::LuaDebugTransportCallbacks callbacks,
                                               LuaConnected completed);
    // GUI-only dispatch, typed callback on GUI; underlying Session policy and trust run at send.
    [[nodiscard]] Result<void> commandForLua(const EngineLuaDebugOwner& owner, std::string method, lang::Json params,
                                             engine::EngineSession::ObservedReply completed, std::stop_token stop);
    [[nodiscard]] Result<engine::EngineState> currentLuaDebugState(const EngineLuaDebugOwner& owner) const;
signals:
    // Actual current-identity push publication only; command replies do not synthesize state.
    void stateObserved();
    // Actual native adapter evidence changed; consumers recheck their own immutable bindings.
    void nativeDebuggerEvidenceChanged();
    // Emitted on GUI only after actual authentication/loss, or an explicit source revocation.
    void crashSourceObserved();
    void crashSourceUnexpectedLoss(const engine::EngineIdentity& identity);
    void crashSourceRevoked();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::app
