// SPDX-License-Identifier: Apache-2.0
//
// SyntaxOutlineProvider (frost/ui/editor/syntax_outline.hpp): member ranges from the tree-sitter
// outline of the session's tree.

#include "frost/ui/editor/syntax_outline.hpp"

#include "frost/syntax/syntax_tree.hpp"

#include <chrono>

namespace frost::ui {

bool isMemberKind(syntax::OutlineKind kind) noexcept { return kind != syntax::OutlineKind::Namespace; }

std::vector<text::ByteRange> SyntaxOutlineProvider::members(const text::Snapshot& text) const noexcept {
    const syntax::SyntaxTree tree = m_session->tree();
    if (tree.empty() || tree.version() != text.version()) {
        return {};
    }
    // A user-initiated jump: allow more than the 1 ms paint-time default.
    syntax::QueryLimits limits;
    limits.budget = std::chrono::milliseconds(50);
    const syntax::Outline outline = tree.outline(limits);
    std::vector<text::ByteRange> ranges;
    ranges.reserve(outline.items.size());
    for (const syntax::OutlineItem& item : outline.items) {
        if (isMemberKind(item.kind)) {
            ranges.push_back(item.range);
        }
    }
    return ranges;
}

} // namespace frost::ui
