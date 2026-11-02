// SPDX-License-Identifier: Apache-2.0
//
// SyntaxOutlineProvider (ADR 0025's IOutlineProvider seam; ADR 0037): the members next/previous
// member (Alt+Down / Alt+Up) jump between, read from a SyntaxSession's tree-sitter outline.
#pragma once

#include "frost/syntax/outline.hpp"
#include "frost/syntax/syntax_session.hpp"
#include "frost/text/editor/outline.hpp"

#include <vector>

namespace frost::ui {

// True for the outline kinds that count as members: everything but namespaces (a namespace is a
// scope, not a member; IntelliJ's next method skips it too). O(1).
[[nodiscard]] bool isMemberKind(syntax::OutlineKind kind) noexcept;

/*
 * Members from the session's current tree. The tree is the session's view tree (edited to the
 * session's text while a reparse runs), so a stale tree still answers. Empty when the session has
 * no tree or its text is not `text` (another version). Borrows the session, which must outlive it;
 * the session's owner thread only (the GUI thread). O(outline) per call.
 */
class SyntaxOutlineProvider final : public text::IOutlineProvider {
public:
    explicit SyntaxOutlineProvider(const syntax::SyntaxSession& session) noexcept : m_session(&session) {}

    [[nodiscard]] std::vector<text::ByteRange> members(const text::Snapshot& text) const noexcept override;

private:
    const syntax::SyntaxSession* m_session;
};

} // namespace frost::ui
