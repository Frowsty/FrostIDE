// SPDX-License-Identifier: Apache-2.0
// RUN-11 R7 Debug startup: owned host run, exact authenticated owner, then FER-9 private Lua DAP.
#pragma once
#include "frost/run/run_manager.hpp"

#include "workbench_engine_lua_debug.hpp"
#include <QObject>

#include <chrono>

namespace frost::ui {
class RunController;
}
namespace frost::app {
// Pure transient configuration: Player waits before VM creation; Editor removes inherited waits.
// Never changes saved identity/arguments or adds a credential/debug command-line flag. Errors:
// InvalidArgument for a non-engine/invalid configuration. O(environment size).
[[nodiscard]] Result<run::RunConfiguration> engineDebugHostConfiguration(const run::RunConfiguration& configuration);
struct EngineRunDebugHooks {
    // GUI dependency seams. Production constructor binds actual EngineIntegration and FER-9.
    // capture must authenticate this exact owned RunId; Busy means startup is still pending.
    std::function<Result<EngineLuaDebugOwner>(run::RunId)> capture;
    std::function<Result<void>(bool, std::stop_token, MultiSessionDebugCoordinator::Prepared, bool)> prepareLua;
    std::function<void(run::RunId)> explicitStop{};
};
struct EngineRunDebugOptions {
    std::chrono::milliseconds startupTimeout{30000};
};
// GUI thread only. RunController/RunManager and hook services outlive this object. Editor Debug
// owns its Play operation through FER-9; its Editor host remains an ordinary owned Run afterwards.
// Player Debug owns an exact RunId independently from attached DAP; explicit Stop ends that owned
// run tree. External attach never goes through this launcher and never gains process ownership.
class EngineRunDebugCoordinator final : public QObject {
    Q_OBJECT
public:
    EngineRunDebugCoordinator(ui::RunController& runs, run::RunManager& manager, EngineIntegration& engine,
                              EngineLuaDebugCoordinator& lua, QObject* parent = nullptr);
    EngineRunDebugCoordinator(ui::RunController& runs, run::RunManager& manager, EngineRunDebugHooks hooks,
                              EngineRunDebugOptions options = {}, QObject* parent = nullptr);
    ~EngineRunDebugCoordinator() override;
    // Starts one owned host through the existing RUN-11 run path, then waits boundedly for matching
    // authentication before FER-9 prepare. Callback GUI, once; its Prepared.start is one-use.
    // Abandoning an unconsumed preparation cancels its DAP transport and stops only its owned host.
    // Refuses implicit rerun of an active configuration. Before-launch tasks remain RUN-11 owned.
    [[nodiscard]] Result<void> prepare(const run::RunConfiguration& configuration, std::stop_token stop,
                                       MultiSessionDebugCoordinator::Prepared completed);
    // Cancel pending preparations and owned Player debug runs; adopted Editor host remains Run-owned.
    void cancel();
signals:
    void failed(const QString& message);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::app
