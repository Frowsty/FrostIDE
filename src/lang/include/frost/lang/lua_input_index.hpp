// SPDX-License-Identifier: Apache-2.0
// Host input invalidation index, not a filesystem cache. Keys are caller-owned documentKeys.
#pragma once
#include <set>
#include <span>
#include <string>
#include <string_view>
namespace frost::lang {
class LuaInputIndex {
public:
    void reset(std::span<const std::string> inputs) {
        m_inputs.clear();
        m_inputs.insert(inputs.begin(), inputs.end());
    }
    // Exact, ancestor and descendant invalidation. O(path depth * log(input count));
    // does not scan the whole project on every watched-file event.
    [[nodiscard]] bool touches(std::string_view key) const {
        for (auto prefix = key; !prefix.empty();) {
            if (m_inputs.contains(prefix))
                return true;
            const auto slash = prefix.find_last_of("/\\");
            if (slash == prefix.npos)
                break;
            prefix = prefix.substr(0, slash);
        }
        for (char separator : {'/', '\\'}) {
            std::string prefix(key);
            prefix += separator;
            const auto next = m_inputs.lower_bound(prefix);
            if (next != m_inputs.end() && next->starts_with(prefix))
                return true;
        }
        return false;
    }

private:
    std::set<std::string, std::less<>> m_inputs;
};
} // namespace frost::lang
