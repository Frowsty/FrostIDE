// SPDX-License-Identifier: Apache-2.0
// GUI-only same authenticated engine, same actual Lua-hook pause coordinator.
#pragma once
#include "frost/ui/mixed_debug_window.hpp"

#include "engine_native_debug.hpp"
#include "workbench_engine_lua_debug.hpp"
#include <QObject>

#include <functional>
#include <memory>
#include <optional>
namespace frost::ui {
class DebugSessionRegistry;
}
namespace frost::app {
class EngineLuaDebugCoordinator;
// Suppliers/services outlive this QObject. Binding is independent of selected debugger tabs.
// Native mapping is enabled only for the product's explicitly pinned lldb-dap session; generic
// DAP thread IDs are never treated as OS IDs. Root must retain the authenticated Lua/R6 lease
// through exact actual native all-stop evidence, while hard EOF/revocation still ends it.
class EngineMixedDebugCoordinator final : public QObject {
    Q_OBJECT
public:
    using NativeSupplier = std::function<std::optional<EngineNativeDebugBinding>()>;
    using NativeMapping = std::function<bool(const EngineNativeDebugBinding&)>;
    using LuaSupplier = std::function<std::optional<EngineLuaDebugBinding>(std::uint64_t)>;
    EngineMixedDebugCoordinator(ui::DebugSessionRegistry& registry, EngineLuaDebugCoordinator& lua,
                                NativeSupplier native, NativeMapping mapping, QObject* parent = nullptr);
    EngineMixedDebugCoordinator(ui::DebugSessionRegistry& registry, LuaSupplier lua, NativeSupplier native,
                                NativeMapping mapping, QObject* parent = nullptr);
    ~EngineMixedDebugCoordinator() override;
    [[nodiscard]] const ui::MixedDebugSnapshot& snapshot() const;
    [[nodiscard]] Result<void> captureNativeStack();
    [[nodiscard]] Result<void> resumeBoth();
    // UI navigation validates all identities and pause generations again. Native frames can be
    // selected while all-stop; Lua selection waits until native actually continues.
    [[nodiscard]] Result<void> selectFrame(std::uint64_t session, std::int64_t thread, std::int64_t frame);
    void refresh();
signals:
    void changed();
    void failed(const QString& reason);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::app
