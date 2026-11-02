// SPDX-License-Identifier: Apache-2.0
//
// ShellLayout helpers and the in-memory shell settings store (frost/ui/shell/shell_settings.hpp).

#include "frost/ui/shell/shell_settings.hpp"

#include <algorithm>

namespace frost::ui {

const ToolWindowLayout* ShellLayout::find(std::string_view id) const noexcept {
    const auto it = std::ranges::find(toolWindows, id, &ToolWindowLayout::id);
    return it == toolWindows.end() ? nullptr : &*it;
}

ToolWindowLayout& ShellLayout::entry(std::string_view id) {
    const auto it = std::ranges::find(toolWindows, id, &ToolWindowLayout::id);
    if (it != toolWindows.end()) {
        return *it;
    }
    toolWindows.push_back(ToolWindowLayout{std::string(id), false, 0});
    return toolWindows.back();
}

Result<ShellLayout> InMemoryShellSettings::load() const {
    if (m_fault) {
        const ErrorCode code = *m_fault;
        m_fault.reset();
        return fail(code, "injected shell settings load failure");
    }
    return m_layout;
}

Result<void> InMemoryShellSettings::store(const ShellLayout& layout) {
    if (m_fault || m_storeFault) {
        const ErrorCode code = m_fault ? *m_fault : *m_storeFault;
        (m_fault ? m_fault : m_storeFault).reset();
        return fail(code, "injected shell settings store failure");
    }
    m_layout = layout;
    ++m_stores;
    return {};
}

} // namespace frost::ui
