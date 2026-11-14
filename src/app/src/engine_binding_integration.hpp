// SPDX-License-Identifier: Apache-2.0
// FEC-13 window composition for actual schema/AST/LSP binding queries and read-only previews.
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/json.hpp"
#include "frost/sched/scheduler.hpp"

#include <QObject>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace frost::app {
class Workbench;
struct EngineBindingRetirement {
    std::vector<sched::JoinHandle<void>> jobs;
    [[nodiscard]] bool ready() const;
    // Final Workbench destruction only, after interactive UI/provider withdrawal.
    void join();
};
class EngineBindingIntegration;
// Worker posting and GUI withdrawal share this lock. A QPointer alone does not pin a
// QObject between checking it and invoking its queue during concurrent destruction.
struct EngineBindingPostRelay {
    std::mutex mutex;
    EngineBindingIntegration* target = nullptr;
    void revoke();
    void post(std::function<void(EngineBindingIntegration&)> callback);
};
class EngineBindingIntegration final : public QObject {
public:
    explicit EngineBindingIntegration(Workbench& workbench, std::shared_ptr<EngineBindingRetirement> retirement = {},
                                      std::shared_ptr<EngineBindingPostRelay> relay = {});
    ~EngineBindingIntegration() override;
    // GUI thread. Immediately invalidates pending captures/results; workers observe stop and join
    // at the final Workbench drain, before providers/fs/launcher/pool die. Safe repeatedly and during project close.
    void cancel();
    void shutdown();
    [[nodiscard]] std::shared_ptr<EngineBindingRetirement> takeRetirement();
    [[nodiscard]] Result<void> refresh();
    [[nodiscard]] Result<void> findUsages();
    [[nodiscard]] Result<void> previewRename(std::string newLuaName, bool renameNative = false);
    // Standard Find Usages hook. False leaves the stock provider route unchanged; guarded async
    // fallback avoids recursing through this hook for symbols without a semantic binding edge.
    [[nodiscard]] bool tryFindUsages();
    [[nodiscard]] lang::Json snapshot() const;
    [[nodiscard]] bool protectsGeneratedResource(std::string_view path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::app
