// SPDX-License-Identifier: Apache-2.0
//
// frost::syntax highlight vocabulary (ADR 0023, D8): the HighlightKind set our highlights.scm
// queries capture, their capture names, and the span list SyntaxTree::highlights returns for the
// editor view to paint. Theme colours for the kinds come from the editor view and theme layers.
#pragma once

#include "frost/text/positions.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace frost::syntax {

// What a highlighted span is. Each kind has one capture name (captureName); a query capture with
// any other name is rejected when the queries are compiled. Adding a kind is an API change.
enum class HighlightKind : std::uint8_t {
    Comment,      // comment
    DocComment,   // comment.doc
    Keyword,      // keyword
    Directive,    // keyword.directive (preprocessor, #region)
    Operator,     // operator
    Punctuation,  // punctuation
    Bracket,      // punctuation.bracket
    String,       // string
    StringEscape, // string.escape
    Character,    // character
    Number,       // number
    Boolean,      // boolean
    Constant,     // constant
    Type,         // type
    BuiltinType,  // type.builtin
    Namespace,    // namespace
    Function,     // function (declarations and definitions)
    FunctionCall, // function.call
    Macro,        // macro
    Variable,     // variable
    Parameter,    // parameter
    Property,     // property (fields, members, JSON keys)
    Attribute,    // attribute
    Label,        // label
    Tag,          // tag (XML element names)
    TagAttribute, // tag.attribute
    Heading,      // markup.heading
    Link,         // markup.link
    Raw,          // markup.raw (code spans and blocks)
    ListMarker,   // markup.list
    Quote,        // markup.quote
};

// The number of HighlightKind values.
inline constexpr std::size_t kHighlightKindCount = 31;

// The capture name of `kind` ("comment.doc"); "" for an out-of-range value. O(1).
[[nodiscard]] std::string_view captureName(HighlightKind kind) noexcept;

// The kind whose capture name is `name`, or nullopt. O(kinds).
[[nodiscard]] std::optional<HighlightKind> highlightKindFromCapture(std::string_view name) noexcept;

// One painted span: `range` (bytes of the tree's snapshot) has `kind`.
struct HighlightSpan {
    text::ByteRange range;
    HighlightKind kind;
    friend constexpr bool operator==(const HighlightSpan&, const HighlightSpan&) noexcept = default;
};

// Spans sorted by range.begin, non-empty, non-overlapping, clipped to the requested range, with
// adjacent spans of one kind merged. `complete` is false when a limit (QueryLimits, or queries not
// prepared yet) cut the result short; the spans present are still valid.
struct HighlightResult {
    std::vector<HighlightSpan> spans;
    bool complete = true;
};

} // namespace frost::syntax
