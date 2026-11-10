// SPDX-License-Identifier: Apache-2.0
//
// The .vcxproj.user writer (see frost/build/vcxproj_user.hpp), over pugixml 1.16 (ADR 0033),
// which frost::build links privately for this file only.

#include "frost/build/vcxproj_user.hpp"

#include "frost/io/file_system.hpp"

#include <pugixml.hpp>

#include <format>
#include <optional>

namespace frost::build {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr std::string_view kMsBuildNamespace = "http://schemas.microsoft.com/developer/msbuild/2003";
constexpr std::string_view kIndentUnit = "  ";
constexpr std::string_view kDebuggerFlavor = "DebuggerFlavor";
constexpr std::string_view kLocalDebugger = "WindowsLocalDebugger";

constexpr char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

bool startsWithIgnoringCase(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && equalsIgnoringCase(text.substr(0, prefix.size()), prefix);
}

// The text without spaces, tabs and line ends, lower-cased: how two spellings of a condition compare.
std::string conditionKey(std::string_view condition) {
    std::string key;
    for (const char c : condition) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            key.push_back(lowerAscii(c));
        }
    }
    return key;
}

// MSBuild property names: a letter or '_', then letters, digits, '_' or '-' (as the build driver checks).
bool isPropertyName(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        const bool digit = (c >= '0' && c <= '9') || c == '-';
        if (!letter && (i == 0 || !digit)) {
            return false;
        }
    }
    return true;
}

std::string_view localName(const pugi::xml_node& node) noexcept {
    const std::string_view name = node.name();
    const std::size_t colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

bool isElementNamed(const pugi::xml_node& node, std::string_view name) noexcept {
    return node.type() == pugi::node_element && equalsIgnoringCase(localName(node), name);
}

// The value of the attribute named `name` (ASCII case ignored), or nullptr.
const char* attributeValue(const pugi::xml_node& node, std::string_view name) noexcept {
    for (const pugi::xml_attribute& attribute : node.attributes()) {
        if (equalsIgnoringCase(attribute.name(), name)) {
            return attribute.value();
        }
    }
    return nullptr;
}

bool isWhitespaceText(const pugi::xml_node& node) noexcept {
    if (node.type() != pugi::node_pcdata) {
        return false;
    }
    for (const char* c = node.value(); *c != '\0'; ++c) {
        if (*c != ' ' && *c != '\t' && *c != '\n' && *c != '\r') {
            return false;
        }
    }
    return true;
}

// The indentation before `node`: what follows the last line end of the whitespace text before it.
std::optional<std::string> indentBefore(const pugi::xml_node& node) {
    const pugi::xml_node previous = node.previous_sibling();
    if (!previous || !isWhitespaceText(previous)) {
        return std::nullopt;
    }
    const std::string_view text = previous.value();
    const std::size_t newline = text.rfind('\n');
    if (newline == std::string_view::npos) {
        return std::nullopt;
    }
    return std::string(text.substr(newline + 1));
}

pugi::xml_node lastElementChild(const pugi::xml_node& parent) {
    pugi::xml_node last;
    for (const pugi::xml_node& child : parent.children()) {
        if (child.type() == pugi::node_element) {
            last = child;
        }
    }
    return last;
}

// The indentation of `parent`'s element children: an existing child's, else the parent's own
// plus one unit.
std::string childIndent(const pugi::xml_node& parent, std::string_view parentIndent) {
    for (const pugi::xml_node& child : parent.children()) {
        if (child.type() == pugi::node_element) {
            if (auto indent = indentBefore(child)) {
                return *indent;
            }
        }
    }
    return std::string(parentIndent) + std::string(kIndentUnit);
}

// Appends `element` (already a child of `parent`, or new) as `parent`'s last element child, laid
// out on its own line at `indent`, with `parentIndent` before the closing tag.
pugi::xml_node appendElement(pugi::xml_node parent, std::string_view name, std::string_view indent,
                             std::string_view parentIndent) {
    const pugi::xml_node last = lastElementChild(parent);
    pugi::xml_node element;
    if (last) {
        // After the last element, on a new line; the whitespace before the closing tag stays after it.
        pugi::xml_node space = parent.insert_child_after(pugi::node_pcdata, last);
        space.set_value(std::format("\n{}", indent).c_str());
        element = parent.insert_child_after(pugi::node_element, space);
    } else {
        // No element yet: the parent's content becomes "\n<indent><element>\n<parentIndent>".
        while (parent.first_child()) {
            parent.remove_child(parent.first_child());
        }
        parent.append_child(pugi::node_pcdata).set_value(std::format("\n{}", indent).c_str());
        element = parent.append_child(pugi::node_element);
        parent.append_child(pugi::node_pcdata).set_value(std::format("\n{}", parentIndent).c_str());
    }
    element.set_name(std::string(name).c_str());
    return element;
}

// Removes `node` and the whitespace text that indents it.
void removeWithIndent(pugi::xml_node parent, pugi::xml_node node) {
    const pugi::xml_node previous = node.previous_sibling();
    if (previous && isWhitespaceText(previous)) {
        parent.remove_child(previous);
    }
    parent.remove_child(node);
}

void setText(pugi::xml_node element, std::string_view value) {
    while (element.first_child()) {
        element.remove_child(element.first_child());
    }
    element.append_child(pugi::node_pcdata).set_value(std::string(value).c_str());
}

// The unconditioned children of `group` named `name`, in document order.
std::vector<pugi::xml_node> propertyElements(const pugi::xml_node& group, std::string_view name) {
    std::vector<pugi::xml_node> found;
    for (const pugi::xml_node& child : group.children()) {
        if (isElementNamed(child, name) && attributeValue(child, "Condition") == nullptr) {
            found.push_back(child);
        }
    }
    return found;
}

// A writer that collects pugixml's output.
struct StringWriter final : pugi::xml_writer {
    std::string text;
    void write(const void* data, std::size_t size) override { text.append(static_cast<const char*>(data), size); }
};

} // namespace

std::string userFilePath(std::string_view projectPath) { return std::string(projectPath) + ".user"; }

std::string userFileCondition(std::string_view configuration, std::string_view platform) {
    return std::format("'$(Configuration)|$(Platform)'=='{}|{}'", configuration, platform);
}

Result<std::string> updateUserFileProperties(std::string_view existing, std::string_view configuration,
                                             std::string_view platform, const std::vector<UserFileProperty>& updates) {
    if (configuration.empty() || platform.empty()) {
        return fail(ErrorCode::InvalidArgument, "a .vcxproj.user edit needs a configuration and a platform");
    }
    for (const UserFileProperty& update : updates) {
        if (!isPropertyName(update.name)) {
            return fail(ErrorCode::InvalidArgument, std::format("'{}' is not an MSBuild property name", update.name));
        }
    }

    const bool fresh = existing.find_first_not_of(" \t\r\n") == std::string_view::npos;
    const bool bom = fresh || existing.starts_with(kBom);
    const bool crlf = fresh || existing.find("\r\n") != std::string_view::npos;
    pugi::xml_document document;
    if (fresh) {
        pugi::xml_node declaration = document.append_child(pugi::node_declaration);
        declaration.append_attribute("version").set_value("1.0");
        declaration.append_attribute("encoding").set_value("utf-8");
        pugi::xml_node root = document.append_child("Project");
        root.append_attribute("ToolsVersion").set_value("Current");
        root.append_attribute("xmlns").set_value(std::string(kMsBuildNamespace).c_str());
    } else {
        const unsigned options = pugi::parse_default | pugi::parse_declaration | pugi::parse_comments | pugi::parse_pi |
                                 pugi::parse_doctype | pugi::parse_ws_pcdata;
        const pugi::xml_parse_result parsed = document.load_buffer(existing.data(), existing.size(), options);
        if (!parsed) {
            return fail(ErrorCode::Corrupt, std::format("the .vcxproj.user file is not well-formed XML ({} at byte {})",
                                                        parsed.description(), parsed.offset));
        }
    }
    pugi::xml_node root = document.document_element();
    if (!isElementNamed(root, "Project")) {
        return fail(ErrorCode::Corrupt, "the .vcxproj.user file's root element is not <Project>");
    }

    // The configuration's group, or a new one after the last element of <Project>.
    const std::string wanted = conditionKey(userFileCondition(configuration, platform));
    pugi::xml_node group;
    for (const pugi::xml_node& child : root.children()) {
        if (isElementNamed(child, "PropertyGroup")) {
            const char* condition = attributeValue(child, "Condition");
            if (condition != nullptr && conditionKey(condition) == wanted) {
                group = child;
            }
        }
    }
    const std::string groupIndent = childIndent(root, "");
    bool changed = false;
    if (!group) {
        bool anyValue = false;
        for (const UserFileProperty& update : updates) {
            anyValue = anyValue || !update.value.empty();
        }
        if (!anyValue) {
            return std::string(existing); // Nothing to set and nothing to remove.
        }
        group = appendElement(root, "PropertyGroup", groupIndent, "");
        group.append_attribute("Condition").set_value(userFileCondition(configuration, platform).c_str());
        changed = true;
    }
    const std::string indent = childIndent(group, indentBefore(group).value_or(groupIndent));
    const std::string closingIndent = indentBefore(group).value_or(groupIndent);

    bool debuggerValue = false;
    for (const UserFileProperty& update : updates) {
        std::vector<pugi::xml_node> elements = propertyElements(group, update.name);
        if (update.value.empty()) {
            for (const pugi::xml_node& element : elements) {
                removeWithIndent(group, element);
                changed = true;
            }
            continue;
        }
        debuggerValue = debuggerValue || startsWithIgnoringCase(update.name, "LocalDebugger");
        if (elements.empty()) {
            setText(appendElement(group, update.name, indent, closingIndent), update.value);
            changed = true;
            continue;
        }
        if (elements.front().text().get() != update.value || elements.size() > 1 ||
            elements.front().first_child() != elements.front().last_child()) {
            setText(elements.front(), update.value);
            for (std::size_t i = 1; i < elements.size(); ++i) {
                removeWithIndent(group, elements[i]);
            }
            changed = true;
        }
    }
    if (debuggerValue && propertyElements(group, kDebuggerFlavor).empty()) {
        setText(appendElement(group, kDebuggerFlavor, indent, closingIndent), kLocalDebugger);
        changed = true;
    }
    if (!changed) {
        return std::string(existing);
    }

    // The top-level nodes one per line (pugixml keeps no whitespace outside the root), each printed
    // raw so the whitespace inside the root is exactly what the file had plus the new lines.
    StringWriter writer;
    bool first = true;
    for (const pugi::xml_node& node : document.children()) {
        if (!first) {
            writer.text.push_back('\n');
        }
        first = false;
        node.print(writer, "", pugi::format_raw | pugi::format_no_declaration, pugi::encoding_utf8);
    }
    if (fresh) {
        writer.text.push_back('\n');
    } else if (existing.ends_with('\n')) {
        writer.text.push_back('\n');
    }
    std::string result(bom ? kBom : std::string_view());
    for (const char c : writer.text) {
        if (c == '\n' && crlf) {
            result += "\r\n";
        } else {
            result.push_back(c);
        }
    }
    return result;
}

Result<void> writeUserFileProperties(io::IFileSystem& fs, std::string_view projectPath, std::string_view configuration,
                                     std::string_view platform, const std::vector<UserFileProperty>& updates) {
    const std::string path = userFilePath(projectPath);
    std::string existing;
    if (auto read = fs.readFile(path); read) {
        existing = std::move(*read);
    } else if (read.error().code() != ErrorCode::NotFound) {
        return std::unexpected(std::move(read.error()));
    }
    auto updated = updateUserFileProperties(existing, configuration, platform, updates);
    if (!updated) {
        return std::unexpected(std::move(updated.error()).withContext(path));
    }
    if (*updated == existing) {
        return {};
    }
    return fs.writeFileAtomic(path, *updated);
}

} // namespace frost::build
