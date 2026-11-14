// SPDX-License-Identifier: Apache-2.0
// LUA9: immutable proposals from the actual pinned Lua AST. Never writes source.
#pragma once
#include "frost/syntax/syntax_tree.hpp"
#include <string>
#include <vector>
namespace frost::lang {
struct LuaSourceEdit { text::ByteRange range; std::string replacement; };
struct LuaRefactoringProposal {
    text::Snapshot original;
    std::string title;
    std::vector<LuaSourceEdit> edits; // Ascending, non-overlapping, against original.
};
// Inline a selected local scalar literal and its lexically resolved read references.
// Refuses writes, attributes, dynamic environments, closure/loop bindings and multi-values.
[[nodiscard]] Result<LuaRefactoringProposal> planLuaInlineVariable(
    const syntax::SyntaxTree&, ByteOffset selected, std::stop_token = {});
// Extract complete straight-line statements in one lexical block into a local closure.
// Captures existing bindings by reference; selected locals used afterward are explicit returns.
// Refuses control transfers, closures, varargs, attributes and multi-return declarations.
[[nodiscard]] Result<LuaRefactoringProposal> planLuaExtractFunction(
    const syntax::SyntaxTree&, text::ByteRange selected, std::string name, std::stop_token = {});
}
