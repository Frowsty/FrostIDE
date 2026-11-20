// SPDX-License-Identifier: Apache-2.0
// Governed, cancellation-aware ordinary language requester over authenticated native shader commands.
#pragma once
#include "frost/lang/lsp_client.hpp"
#include "frost/sched/resource_governor.hpp"
namespace frost::app {
[[nodiscard]] bool isEngineShaderProgram(std::string_view path);
[[nodiscard]] bool isEngineShaderSource(std::string_view path);
class EngineShaderLanguageRequester final : public lang::ILspRequester {
public:
    using Reply = std::function<void(Result<lang::Json>)>;
    using Dispatch = std::function<void(std::string, lang::Json, std::stop_token, Reply)>;
    // Scheduler outlives requests; dispatch posts to GUI and checks the actual paired/trusted engine at send.
    EngineShaderLanguageRequester(sched::IScheduler& scheduler, std::shared_ptr<sched::ResourceGovernor> governor,
                                  Dispatch dispatch)
        : m_scheduler(scheduler), m_governor(std::move(governor)), m_dispatch(std::move(dispatch)) {}
    sched::Task<Result<lang::Json>> request(std::string, lang::Json, std::stop_token = {}) override;
    lang::PositionEncoding encoding() const override { return lang::PositionEncoding::Utf8; }
    std::optional<lang::ServerCapabilities> serverCapabilities() const override;

private:
    static sched::Task<Result<lang::Json>> answer(sched::IScheduler&, std::shared_ptr<sched::ResourceGovernor>,
                                                  Dispatch, std::string, lang::Json, std::stop_token);
    sched::IScheduler& m_scheduler;
    std::shared_ptr<sched::ResourceGovernor> m_governor;
    Dispatch m_dispatch;
};
} // namespace frost::app
