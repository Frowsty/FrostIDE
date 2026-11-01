// SPDX-License-Identifier: Apache-2.0
//
// tree_state.hpp: the immutable state a SyntaxTree shares (ADR 0023, D5 and D7): a tree-sitter
// tree, its language, the snapshot whose coordinates it is in, and the version actually parsed;
// plus the iterative tree dumper behind dumpForTesting.
#pragma once

#include "frost/syntax/syntax_tree.hpp"
#include "frost/text/snapshot.hpp"

#include "ts.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace frost::syntax {

class Language;

namespace detail {

// Never modified after publication; `tree` is owned and freed with the state, on whichever thread
// drops the last SyntaxTree. TODO(M1): ADR 0023 D9 moves the release of big trees to a Background
// task (B21); the hardening phase adds it.
struct TreeState {
    TreePtr tree;
    const Language* language = nullptr;
    text::Snapshot text;
    std::uint64_t parsedVersion = 0;
};

// Publishes `tree` as an immutable SyntaxTree.
[[nodiscard]] SyntaxTree makeTree(TreePtr tree, const Language* language, text::Snapshot text,
                                  std::uint64_t parsedVersion) noexcept;

// The dumpForTesting format for the subtree at `root`: every node (named and anonymous) on its own
// line, indented two spaces per depth, as `type [start-end] (row,col)-(row,col)` with ranges.
// Iterative (TSTreeCursor), so deep trees cannot overflow the stack. O(nodes).
[[nodiscard]] std::string dumpNode(TSNode root, bool withRanges);

} // namespace detail

} // namespace frost::syntax
