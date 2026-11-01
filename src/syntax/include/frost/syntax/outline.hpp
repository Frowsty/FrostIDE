// SPDX-License-Identifier: Apache-2.0
//
// frost::syntax outline (ADR 0023, D8): the declarations our outline.scm queries find (namespaces,
// types, functions, members, CMake commands and targets, JSON keys, XML elements, Markdown
// sections), nested by containment. It powers next/previous member in the editor model; the
// Structure view model is M15 (ADR 0029).
#pragma once

#include "frost/text/positions.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace frost::syntax {

// What an outline item is. The capture outline.<snake_case name> selects it (outline.enum_member).
enum class OutlineKind : std::uint8_t {
    Namespace,
    Class,
    Struct,
    Union,
    Interface,
    Enum,
    EnumMember,
    Function,
    Method,
    Constructor,
    Destructor,
    Field,
    Property,
    Event,
    Variable,
    Constant,
    Macro,
    TypeAlias,
    Command,
    Target,
    Key,
    Element,
    Heading,
};

// The number of OutlineKind values.
inline constexpr std::size_t kOutlineKindCount = 23;

// The LSP 3.17 SymbolKind value of `kind` (Namespace 3, Class 5, Method 6, ...), for merging with
// clangd's document symbols in M3. O(1).
[[nodiscard]] std::uint8_t toLspSymbolKind(OutlineKind kind) noexcept;

// The capture suffix of `kind` ("enum_member"); "" for an out-of-range value. O(1).
[[nodiscard]] std::string_view outlineCaptureSuffix(OutlineKind kind) noexcept;

// No parent: a top-level item.
inline constexpr std::uint32_t kNoParent = 0xFFFF'FFFF;

/*
 * One declaration. `range` is the whole declaration and `nameRange` its name (empty at
 * range.begin when the query captured none), both bytes of the tree's snapshot. `parent` is the
 * index of the innermost item containing it (always smaller than its own index) or kNoParent;
 * `depth` is 0 for top-level items. `name` is the name's text with whitespace runs collapsed to
 * one space, trimmed, and clipped to at most 256 bytes at a UTF-8 boundary.
 */
struct OutlineItem {
    OutlineKind kind = OutlineKind::Function;
    std::uint32_t parent = kNoParent;
    std::uint32_t depth = 0;
    text::ByteRange range;
    text::ByteRange nameRange;
    std::string name;
};

// Items in pre-order (sorted by range.begin, outer before inner). `complete` is false when a
// QueryLimits bound cut the result short.
struct Outline {
    std::vector<OutlineItem> items;
    bool complete = true;
};

} // namespace frost::syntax
