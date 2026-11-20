// SPDX-License-Identifier: Apache-2.0
// Project-bound Unity editor sessions and acknowledged play controls.
#pragma once
#include "frost/core/error.hpp"
#include "frost/lang/json.hpp"

#include <QObject>

#include <memory>
#include <functional>
#include <atomic>
#include <optional>
namespace frost::app {
class Workbench;
// Discovery labels are not process authentication; the debugger independently holds/verifies the chosen PID.
struct UnityEditorIdentity {
    std::string root, instance, session;
    int generation = 0, reportedPid = 0;
    std::shared_ptr<const std::atomic_uint64_t> epoch;
    std::uint64_t revision = 0;
    bool tests = false;
};
class UnityControlIntegration final : public QObject {
public:
    explicit UnityControlIntegration(Workbench& workbench);
    ~UnityControlIntegration() override;
    [[nodiscard]] Result<void> announce(const lang::Json& message);
    [[nodiscard]] std::optional<UnityEditorIdentity> selectedEditor() const;
    // Entire encoded payload is authenticated by the existing one-shot root/session/epoch channel.
    void requestTests(const UnityEditorIdentity& expected,const lang::Json& payload,std::function<void(Result<lang::Json>)> done);
    void cancel();
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::app
