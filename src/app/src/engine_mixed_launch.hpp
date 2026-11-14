// SPDX-License-Identifier: Apache-2.0
// Explicit native-parent engine package: prepare all tasks, then launch native host and dependent Lua.
#pragma once
#include "engine_native_host.hpp"
#include "workbench_engine_lua_debug.hpp"
#include <QObject>
namespace frost::app {
class EngineMixedLaunchCoordinator final : public QObject {
    Q_OBJECT
public:
    EngineMixedLaunchCoordinator(EngineIntegration& engine, EngineLuaDebugCoordinator& lua, run::RunManager& tasks,
                                 QObject* parent = nullptr);
    ~EngineMixedLaunchCoordinator() override;
    // GUI only, exact NativeAndLua typed member. Prepares before-tasks/canonical plan on RunManager's
    // worker without launching the engine. Its phased start owns the native host, waits actual PID,
    // config ACK/private mutual authentication, then waits Lua configuration/actual start receipt.
    // All services outlive this coordinator; raw startup data never appears in snapshot/diagnostics.
    [[nodiscard]] Result<void> prepare(const run::RunConfiguration& configuration, std::stop_token stop,
                                       MultiSessionDebugCoordinator::Prepared done);
    void cancel();                             // Only its groups/native host leases; external attaches remain unowned.
    [[nodiscard]] lang::Json snapshot() const; // Actual phase/IDs/PID only, no invented frames/results.
signals:
    void changed();
    void failed(const QString& message);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::app
