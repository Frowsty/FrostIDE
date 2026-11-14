// SPDX-License-Identifier: Apache-2.0
// FEC-13 public Workbench starts; the coordinator owns all asynchronous captures and presentation.
#include "frost/app/workbench.hpp"

#include "engine_binding_integration.hpp"

#include <algorithm>

namespace frost::app {
bool EngineBindingRetirement::ready() const {
    return std::ranges::all_of(jobs, [](const auto& job) { return job.ready(); });
}
void EngineBindingRetirement::join() {
    for (auto& job : jobs)
        job.join();
    jobs.clear();
}
void Workbench::retireEngineBindingWork(std::shared_ptr<EngineBindingRetirement> retirement) {
    std::erase_if(m_retiredEngineBindings, [](const auto& value) { return value->ready(); });
    if (retirement && !retirement->ready())
        m_retiredEngineBindings.push_back(std::move(retirement));
}
void Workbench::attachEngineBindings() { m_engineBindings = std::make_unique<EngineBindingIntegration>(*this); }
Result<void> Workbench::refreshEngineBindings() {
    return m_engineBindings ? m_engineBindings->refresh()
                            : Result<void>(fail(ErrorCode::Unsupported, "Binding integration is unavailable"));
}
Result<void> Workbench::findEngineBindingUsages() {
    return m_engineBindings ? m_engineBindings->findUsages()
                            : Result<void>(fail(ErrorCode::Unsupported, "Binding integration is unavailable"));
}
Result<void> Workbench::previewEngineBindingRename(std::string name, bool native) {
    return m_engineBindings ? m_engineBindings->previewRename(std::move(name), native)
                            : Result<void>(fail(ErrorCode::Unsupported, "Binding integration is unavailable"));
}
lang::Json Workbench::engineBindingState() const {
    return m_engineBindings ? m_engineBindings->snapshot() : lang::Json::object().set("available", false);
}
} // namespace frost::app
