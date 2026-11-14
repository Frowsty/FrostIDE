// SPDX-License-Identifier: Apache-2.0
// Compound debug admission/preparation transaction; registry retains actual per-session outcomes.
#pragma once
#include "frost/run/run_configuration.hpp"
#include "frost/ui/debug_session_registry.hpp"

#include <QObject>

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>
namespace frost::app {
struct PreparedDebugLaunch {
    // A prepared executable/endpoint, never a fabricated session/result. Invoked only after every
    // member prepared successfully and current workspace/trust admission was rechecked.
    std::function<Result<ui::DebugSessionId>(ui::DebugSessionRegistry&, ui::DebugGroupId)> start{};
    // An explicit dependent package (native host -> authenticated Lua), never inferred from names.
    // Exactly one start form is supplied. Every prerequisite/task still prepares before this runs.
    // GUI callback, at most once; all child IDs belong to group. Cancellation must revoke private
    // endpoint work; coordinator rolls back all partially created group children as well.
    using Started = std::function<void(Result<std::vector<ui::DebugSessionId>>)>;
    std::function<Result<void>(ui::DebugSessionRegistry&, ui::DebugGroupId, std::stop_token, Started)> startPhased{};
};
// GUI thread only; registry and captured preparation services outlive coordinator. All before-launch
// steps resolve serially before any adapter launch. A failed launch stops this group's sessions only;
// ordinary attach Stop still detaches. Cancellation revokes late callbacks and requests owned work stop.
class MultiSessionDebugCoordinator final : public QObject {
    Q_OBJECT
public:
    using Prepared = std::function<void(Result<PreparedDebugLaunch>)>;
    using Prepare = std::function<Result<void>(const run::RunConfiguration&, std::stop_token, Prepared)>;
    using AdditionalPrepare =
        std::function<std::optional<Result<void>>(const run::RunConfiguration&, std::stop_token, Prepared)>;
    // Optional engine adapter provider. nullopt delegates to the workspace's native preparation.
    // Frozen per launch; changing the provider affects only later launches. GUI thread callbacks.
    void setAdditionalPrepare(AdditionalPrepare prepare) { m_additional = std::move(prepare); }
    MultiSessionDebugCoordinator(ui::DebugSessionRegistry& sessions, QObject* parent = nullptr);
    ~MultiSessionDebugCoordinator() override;
    [[nodiscard]] Result<ui::DebugGroupId> start(std::vector<run::RunConfiguration> members,
                                                 std::function<bool()> admitted, Prepare prepare);
    [[nodiscard]] bool busy() const noexcept { return m_job != nullptr; }
    void cancel();
signals:
    void stateChanged();
    void failed(const QString& message);

private:
    struct Job;
    void advance(const std::shared_ptr<Job>& job);
    void startNext(const std::shared_ptr<Job>& job);
    void abort(const std::shared_ptr<Job>& job, const Error& error);
    ui::DebugSessionRegistry& m_sessions;
    std::shared_ptr<Job> m_job;
    AdditionalPrepare m_additional;
};
} // namespace frost::app
