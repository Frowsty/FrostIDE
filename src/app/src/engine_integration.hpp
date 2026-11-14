// SPDX-License-Identifier: Apache-2.0
// App coordinator for authenticated FrostEngine sessions, pairing, push UI and owned run types.
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/json.hpp"
#include "frost/run/run_manager.hpp"

#include <QObject>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace frost::app {
class Workbench;

// GUI-thread coordinator. Sessions and background work are stopped before the scheduler dies.
// Pairing never grants project trust or process ownership. Only RUN-11 runs are owned children.
class EngineIntegration final : public QObject {
public:
    explicit EngineIntegration(Workbench& workbench);
    ~EngineIntegration() override;
    // Cancels project/session work and owned runs; a paired external editor remains alive.
    void cancel();
    void trustChanged();
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

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::app
