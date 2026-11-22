// SPDX-License-Identifier: Apache-2.0
// Parser-backed structural queries and syntax-validated replacement proposals (REF-15).
#pragma once
#include "frost/core/error.hpp"
#include "frost/syntax/language.hpp"
#include "frost/text/snapshot.hpp"

#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
namespace frost::syntax {
struct StructuralCapture {
    text::ByteRange range;
    std::string text;
};
struct StructuralMatch {
    text::ByteRange range;
    std::map<std::string, StructuralCapture, std::less<>> captures;
};
struct StructuralResult {
    text::Snapshot before;
    std::vector<StructuralMatch> matches;
    std::optional<std::string> after; // Only for an explicit replacement, never a write.
};
/* Runs one tree-sitter S-expression pattern over a fresh, error-free parse. Exactly one @match
 * capture is required; all named captures must bind one node. Supports literal #eq?, #not-eq?,
 * #any-of? and #not-any-of? predicates; unsupported predicates/quantifiers/semantic constraints
 * refuse. Replacement uses ${capture} verbatim and $$ for a literal dollar. Nested/overlapping
 * replacements and a syntactically invalid result refuse the complete proposal. This is syntax
 * matching, never name/type resolution or a claim of compilation correctness. Thread-safe; owns
 * its snapshot, captures and output. No files/processes are touched. O(parser/query work), bounded
 * to 4 MiB input, 16 KiB pattern/template, 32 captures, 2000 matches, 8 MiB captured/output bytes
 * and a two-second parse/query deadline each; cancellation/limits return errors, never a subset.
 */
[[nodiscard]] Result<StructuralResult> structuralSearch(LanguageId language, const text::Snapshot& source,
                                                        std::string_view pattern,
                                                        std::optional<std::string_view> replacement = {},
                                                        const std::stop_token& stop = {});
} // namespace frost::syntax
