// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/lang/lsp_client.hpp"
#include "frost/core/mutex.hpp"
#include "frost/project/cmake_file_api.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/sched/scheduler.hpp"
#include <memory>
#include <vector>
namespace frost::lang {
// Immutable request input, captured by the UI before a request leaves the GUI thread.
struct CMakeLanguageInput {
    std::string path;
    text::Snapshot source;
    std::vector<std::pair<std::string, std::string>> inputs;
    std::vector<std::pair<std::string, text::Snapshot>> openInputs;
    std::shared_ptr<const project::CMakeProject> model;
};
[[nodiscard]] bool isCMakeSource(std::string_view path);
// Source analysis never configures a project or evaluates CMake code. Values explicitly distinguish
// source declarations from the last configured cache. Unsupported dynamic declarations stay opaque.
[[nodiscard]] Result<Json> queryCMakeLanguage(const CMakeLanguageInput& input, std::string_view method,
                                             const Json& params, std::stop_token stop = {});
class CMakeLanguageService final : public ILspRequester {
public:
    // Reader must enforce maxBytes during reading, not after allocating the whole file.
    // Scheduler and the reader's backing filesystem outlive pending request tasks. Request
    // frames own the reader and immutable input, never the service or its GUI bridge.
    using InputReader = std::function<Result<std::string>(std::string_view, std::size_t maxBytes)>;
    CMakeLanguageService(sched::IScheduler& scheduler, InputReader reader)
        : m_scheduler(scheduler), m_reader(std::move(reader)) {}
    static constexpr std::size_t kMaxInputBytes = 256 * 1024;
    static constexpr std::size_t kMaxTotalInputBytes = 4 * 1024 * 1024;
    static constexpr std::size_t kMaxInputs = 256;
    void setInput(CMakeLanguageInput input);
    sched::Task<Result<Json>> request(std::string method, Json params, std::stop_token stop = {}) override;
    PositionEncoding encoding() const override { return PositionEncoding::Utf16; }
    std::optional<ServerCapabilities> serverCapabilities() const override;
private:
    static sched::Task<Result<Json>> answer(sched::IScheduler& scheduler, InputReader reader,
                                            CMakeLanguageInput input, std::string method, Json params,
                                            std::stop_token stop);
    sched::IScheduler& m_scheduler;
    InputReader m_reader;
    mutable Mutex m_mutex;
    CMakeLanguageInput m_input FROST_GUARDED_BY(m_mutex);
};
} // namespace frost::lang
