// SPDX-License-Identifier: Apache-2.0
//
// LSP snippets (LSP 3.17 "Snippet Syntax") parsed to plain text plus tab stops, for completion
// items whose insertTextFormat is Snippet (W2, ADR 0041). clangd sends "foo(${1:int x}, ${2:int
// y})" for a call; the editor inserts "foo(int x, int y)" and selects "int x", then "int y".
/*
 * Grammar (from the specification): $n and ${n} tab stops, ${n:placeholder} (placeholders may
 * nest), ${n|one,two|} choices (the first option is inserted), $NAME, ${NAME} and ${NAME:default}
 * variables (FrostIDE resolves none: the default is inserted, else nothing) and the transform
 * form ${NAME/regex/format/options} (dropped). '\' escapes '$', '}' and '\' (and ',' and '|' in
 * choices). Anything malformed is kept as literal text, so parsing never fails.
 *
 * Tab stops are ordered by number with $0 (the final caret) last; a number used several times
 * yields one entry per occurrence (mirrors). Offsets are bytes into `text`. Pure, O(length).
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::lang {

struct SnippetTabStop {
    std::uint32_t index = 0; // The snippet's number; 0 is the final caret.
    std::uint32_t begin = 0; // Byte range of the placeholder text in ParsedSnippet::text.
    std::uint32_t end = 0;
    friend bool operator==(const SnippetTabStop&, const SnippetTabStop&) = default;
};

struct ParsedSnippet {
    std::string text;
    std::vector<SnippetTabStop> tabStops; // By index (1, 2, ...), then $0.
    // Where the caret goes after insertion: the first tab stop's begin, else $0, else the end.
    [[nodiscard]] std::uint32_t caret() const noexcept;
};

[[nodiscard]] ParsedSnippet parseSnippet(std::string_view snippet);

} // namespace frost::lang
