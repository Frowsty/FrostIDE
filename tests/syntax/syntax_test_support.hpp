// SPDX-License-Identifier: Apache-2.0
//
// Shared helpers for frost::syntax's tests (ADR 0023): the sample document of every grammar, a
// fresh-parse oracle that bypasses our TSInput, edits and session, and span lookups.
#pragma once

#include "frost/syntax/highlight.hpp"
#include "frost/syntax/language.hpp"
#include "frost/syntax/syntax_tree.hpp"

#include <string>
#include <string_view>

namespace frost::syntax::testing {

// A small document exercising the main constructs of `language` (LF line ends).
[[nodiscard]] std::string_view sample(LanguageId language) noexcept;

// `text` with every lone CR replaced by LF: the independent model of ADR 0023 D3.
[[nodiscard]] std::string normaliseLoneCr(std::string_view text);

// dumpForTesting(true) of a fresh tree-sitter parse of normaliseLoneCr(text) through
// ts_parser_parse_string: no snapshot, TSInput, edits or session involved.
[[nodiscard]] std::string freshDump(LanguageId language, std::string_view text);

// The capture name (captureName) of the span covering all of the first occurrence of `needle`
// at or after byte `from` of the tree's text; "<none>" when no single span covers it and
// "<absent>" when the needle does not occur. Highlights the whole document without a deadline.
[[nodiscard]] std::string kindAt(const SyntaxTree& tree, std::string_view needle, std::size_t from = 0);

// The outline as indented lines "<kind> <name>" (kind is outlineCaptureSuffix), two spaces per
// depth level, for whole-outline comparisons.
[[nodiscard]] std::string outlineText(const SyntaxTree& tree);

// Parses `text` as `language` through a SyntaxSession on a SimExecutor (a synchronous parse) and
// returns the Current tree, which outlives the session; an empty tree if the session did not
// reach Current.
[[nodiscard]] SyntaxTree parseNow(LanguageId language, std::string_view text);

// QueryLimits without a deadline, for deterministic tests in Debug builds.
[[nodiscard]] QueryLimits unlimited() noexcept;

} // namespace frost::syntax::testing
