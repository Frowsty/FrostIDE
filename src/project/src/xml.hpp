// SPDX-License-Identifier: Apache-2.0
//
// Private XML helpers of frost::project over pugixml 1.16 (ADR 0033): parsing with frost errors
// and case-insensitive element and attribute lookup, as MSBuild and the .slnx readers need.
// Only the project library's own sources include this header, so pugixml stays private.
#pragma once

#include "frost/core/error.hpp"

#include <pugixml.hpp>

#include <string_view>

namespace frost::project::detail {

// Parses `content` (any encoding pugixml detects, BOMs included) into `doc` and checks that the
// root element is named `rootName` (ASCII case ignored). Errors: Corrupt naming `path` and the
// line of the first problem.
[[nodiscard]] Result<void> parseXml(pugi::xml_document& doc, std::string_view content, std::string_view path,
                                    std::string_view rootName);

// The element's local name: "ms:Project" -> "Project".
[[nodiscard]] std::string_view localName(const pugi::xml_node& node) noexcept;

// True when `node` is an element whose local name equals `name` (ASCII case ignored).
[[nodiscard]] bool isElement(const pugi::xml_node& node, std::string_view name) noexcept;

// The first child element named `name` (ASCII case ignored), or an empty node.
[[nodiscard]] pugi::xml_node childElement(const pugi::xml_node& node, std::string_view name) noexcept;

// The value of the attribute named `name` (ASCII case ignored); "" when absent.
[[nodiscard]] std::string_view attribute(const pugi::xml_node& node, std::string_view name) noexcept;

// True when the attribute exists (ASCII case ignored).
[[nodiscard]] bool hasAttribute(const pugi::xml_node& node, std::string_view name) noexcept;

// The element's text content, trimmed.
[[nodiscard]] std::string_view elementText(const pugi::xml_node& node) noexcept;

} // namespace frost::project::detail
