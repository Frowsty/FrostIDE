// SPDX-License-Identifier: Apache-2.0
// SLN-18 read-only language service; source imports never execute MSBuild.
#pragma once
#include "frost/lang/cmake_language.hpp"
#include "frost/project/msbuild_source.hpp"
#include "frost/project/solution_model.hpp"
namespace frost::lang {
struct MSBuildLanguageInput {
    std::string path;
    text::Snapshot source;
    std::vector<std::pair<std::string, std::string>> inputs;
    std::vector<std::pair<std::string, text::Snapshot>> openInputs;
    std::vector<project::EvaluatedProjectItems> evaluations; // Last trusted engine results, explicitly labelled.
};
[[nodiscard]] Result<Json> queryMSBuildLanguage(const MSBuildLanguageInput& input, std::string_view method,
                                                const Json& params, const std::stop_token& stop = {});
class MSBuildLanguageService final : public ILspRequester {
public:
    using InputReader = CMakeLanguageService::InputReader;
    MSBuildLanguageService(sched::IScheduler& scheduler, InputReader reader)
        : m_scheduler(scheduler), m_reader(std::move(reader)) {}
    void setInput(MSBuildLanguageInput input);
    sched::Task<Result<Json>> request(std::string method, Json params, std::stop_token stop = {}) override;
    PositionEncoding encoding() const override { return PositionEncoding::Utf16; }
    std::optional<ServerCapabilities> serverCapabilities() const override;

private:
    static sched::Task<Result<Json>> answer(sched::IScheduler&, InputReader, MSBuildLanguageInput, std::string, Json,
                                            std::stop_token);
    sched::IScheduler& m_scheduler;
    InputReader m_reader;
    mutable Mutex m_mutex;
    MSBuildLanguageInput m_input FROST_GUARDED_BY(m_mutex);
};
} // namespace frost::lang
