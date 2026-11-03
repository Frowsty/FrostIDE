// SPDX-License-Identifier: Apache-2.0
//
// Worker-prepared visibility and adjacency for the File System speed search.
#pragma once

#include "frost/search/fuzzy_path.hpp"
#include "frost/ui/nav/project_files.hpp"

#include <limits>
#include <stop_token>
#include <string_view>
#include <vector>

namespace frost::ui::workbench_detail {

struct FileTreeFilter {
    std::vector<std::size_t> offsets;
    std::vector<std::size_t> children;
    std::vector<std::size_t> rows;
    std::vector<bool> visible;
    std::vector<std::size_t> expand;
    std::size_t first = std::numeric_limits<std::size_t>::max();
    int matches = 0;
};

// `query` is nonempty and lower case. The caller owns files for the duration.
// O(nodes + edges + matching cost); checks cancellation between nodes.
inline std::optional<FileTreeFilter> filterFileTree(const nav::ProjectFiles& files, std::string_view query,
                                                    std::stop_token token) {
    FileTreeFilter result;
    const auto& nodes = files.nodes();
    std::vector<bool> self(nodes.size(), false);
    result.visible.resize(nodes.size(), false);
    result.rows.resize(nodes.size(), 0);
    for (std::size_t i = nodes.size(); i-- > 0;) {
        if (token.stop_requested())
            return std::nullopt;
        self[i] = search::scoreWord(query, nodes[i].name).has_value();
        if (self[i])
            ++result.matches;
        result.visible[i] = result.visible[i] || self[i];
        if (result.visible[i] && nodes[i].parent)
            result.visible[*nodes[i].parent] = true;
    }
    result.offsets.reserve(nodes.size() + 1);
    for (const auto& node : nodes) {
        if (token.stop_requested())
            return std::nullopt;
        result.offsets.push_back(result.children.size());
        std::size_t row = 0;
        for (auto child : node.children) {
            if (result.visible[child]) {
                result.rows[child] = row++;
                result.children.push_back(child);
            }
        }
    }
    result.offsets.push_back(result.children.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (token.stop_requested())
            return std::nullopt;
        if (result.offsets[i] != result.offsets[i + 1])
            result.expand.push_back(i);
    }
    if (!nodes.empty()) {
        std::vector<std::size_t> stack{0};
        while (!stack.empty()) {
            if (token.stop_requested())
                return std::nullopt;
            const auto node = stack.back();
            stack.pop_back();
            if (self[node]) {
                result.first = node;
                break;
            }
            for (std::size_t i = result.offsets[node + 1]; i-- > result.offsets[node];) {
                stack.push_back(result.children[i]);
            }
        }
    }
    return result;
}

} // namespace frost::ui::workbench_detail
