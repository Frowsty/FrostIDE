// SPDX-License-Identifier: Apache-2.0
//
// Private XML helpers of frost::project over pugixml (xml.hpp).
#include "xml.hpp"

#include "text_util.hpp"

#include <algorithm>
#include <cstddef>
#include <format>

namespace frost::project::detail {

Result<void> parseXml(pugi::xml_document& doc, std::string_view content, std::string_view path,
                      std::string_view rootName) {
    // parse_default keeps escapes and normalises line ends; DOCTYPE and processing instructions
    // are skipped (pugixml never fetches external entities).
    const pugi::xml_parse_result parsed =
        doc.load_buffer(content.data(), content.size(), pugi::parse_default, pugi::encoding_auto);
    if (!parsed) {
        // The offset counts characters of the converted buffer: exact for UTF-8 input.
        const auto offset = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(parsed.offset, 0, std::ssize(content)));
        const auto line = 1 + std::count(content.begin(), content.begin() + static_cast<std::ptrdiff_t>(offset), '\n');
        return fail(ErrorCode::Corrupt,
                    std::format("{}: malformed XML at line {}: {}", path, line, parsed.description()));
    }
    const pugi::xml_node root = doc.document_element();
    if (!isElement(root, rootName)) {
        return fail(ErrorCode::Corrupt, std::format("{}: the root element is <{}>, expected <{}>", path,
                                                    root ? root.name() : "", rootName));
    }
    return {};
}

std::string_view localName(const pugi::xml_node& node) noexcept {
    std::string_view name = node.name();
    const std::size_t colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

bool isElement(const pugi::xml_node& node, std::string_view name) noexcept {
    return node.type() == pugi::node_element && iequals(localName(node), name);
}

pugi::xml_node childElement(const pugi::xml_node& node, std::string_view name) noexcept {
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
        if (isElement(child, name)) {
            return child;
        }
    }
    return {};
}

std::string_view attribute(const pugi::xml_node& node, std::string_view name) noexcept {
    for (pugi::xml_attribute attr = node.first_attribute(); attr; attr = attr.next_attribute()) {
        if (iequals(attr.name(), name)) {
            return attr.value();
        }
    }
    return {};
}

bool hasAttribute(const pugi::xml_node& node, std::string_view name) noexcept {
    for (pugi::xml_attribute attr = node.first_attribute(); attr; attr = attr.next_attribute()) {
        if (iequals(attr.name(), name)) {
            return true;
        }
    }
    return false;
}

std::string_view elementText(const pugi::xml_node& node) noexcept { return trim(node.child_value()); }

} // namespace frost::project::detail
