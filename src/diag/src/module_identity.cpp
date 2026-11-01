// SPDX-License-Identifier: Apache-2.0
//
// ModuleTable (frost/diag/module_identity.hpp): the per-report cache of resolved modules.

#include "frost/diag/module_identity.hpp"

namespace frost::diag {

int ModuleTable::indexOf(std::uintptr_t address) {
    for (std::size_t i = 0; i < m_modules.size(); ++i) {
        const ModuleIdentity& module = m_modules[i];
        if (address >= module.base && address - module.base < module.size) {
            return static_cast<int>(i);
        }
    }
    auto module = moduleForAddress(address);
    if (!module || module->size == 0) {
        return -1;
    }
    m_modules.push_back(std::move(*module));
    return static_cast<int>(m_modules.size() - 1);
}

} // namespace frost::diag
