// SPDX-License-Identifier: Apache-2.0
//
// Fuzz harness for .natvis debugger visualizer XML, C++ template type patterns, and
// DisplayString / Expand expression and format-specifier parsing (FND-14, PLAN.md Appendix C):
//   - parses arbitrary byte sequences as .natvis XML via pugixml;
//   - parses <AutoVisualizer>, <Type>, <AlternativeType>, <Version>, <Intrinsic>,
//     <DisplayString>, <StringView>, and <Expand> (<Item>, <ArrayItems>, <IndexListItems>,
//     <LinkedListItems>, <TreeItems>, <ExpandedItem>, <Synthetic>);
//   - parses curly-brace interpolation expressions {expr,format} (handling escaped {{ / }},
//     nested braces and .natvis format specifiers like ,na, ,s8, ,x, ,view(...), ,[size]);
//   - parses C++ template type patterns (e.g. std::vector<*,*>) and matches them against
//     concrete C++ type names.

#include "fuzz/fuzz_harness.hpp"
#include "support/crash_dialogs.hpp"
#include <pugixml.hpp>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::fuzz {
namespace {

constexpr int kMaxExpandDepth = 16;
constexpr std::size_t kMaxNodesPerType = 128;

struct NatvisToken {
    bool isExpression = false;
    std::string text;
    std::string formatSpec;
};

struct NatvisTypePattern {
    std::string baseName;
    std::vector<std::string> templateArgs;
    bool valid = true;
};

std::string_view trimAscii(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return s;
}

// Splits a .natvis DisplayString / StringView template into literal text and {expr,spec} tokens.
std::vector<NatvisToken> parseDisplayTemplate(std::string_view raw) {
    std::vector<NatvisToken> tokens;
    std::string literal;
    std::size_t i = 0;
    while (i < raw.size() && tokens.size() < 64) {
        if (raw[i] == '{') {
            if (i + 1 < raw.size() && raw[i + 1] == '{') {
                literal.push_back('{');
                i += 2;
                continue;
            }
            if (!literal.empty()) {
                tokens.push_back(NatvisToken{false, std::move(literal), {}});
                literal.clear();
            }
            ++i;
            int braceDepth = 1;
            int parenDepth = 0;
            int bracketDepth = 0;
            std::optional<std::size_t> commaSplit;
            const std::size_t exprStart = i;
            while (i < raw.size() && braceDepth > 0) {
                const char c = raw[i];
                if (c == '{') {
                    ++braceDepth;
                } else if (c == '}') {
                    --braceDepth;
                    if (braceDepth == 0) {
                        break;
                    }
                } else if (c == '(') {
                    ++parenDepth;
                } else if (c == ')' && parenDepth > 0) {
                    --parenDepth;
                } else if (c == '[') {
                    ++bracketDepth;
                } else if (c == ']' && bracketDepth > 0) {
                    --bracketDepth;
                } else if (c == ',' && braceDepth == 1 && parenDepth == 0 && bracketDepth == 0) {
                    commaSplit = i;
                }
                ++i;
            }
            const std::size_t exprEnd = i;
            if (i < raw.size() && raw[i] == '}') {
                ++i;
            }
            NatvisToken tok;
            tok.isExpression = true;
            if (commaSplit.has_value() && *commaSplit >= exprStart && *commaSplit < exprEnd) {
                tok.text = std::string(trimAscii(raw.substr(exprStart, *commaSplit - exprStart)));
                tok.formatSpec = std::string(trimAscii(raw.substr(*commaSplit + 1, exprEnd - (*commaSplit + 1))));
            } else {
                tok.text = std::string(trimAscii(raw.substr(exprStart, exprEnd - exprStart)));
            }
            tokens.push_back(std::move(tok));
        } else if (raw[i] == '}' && i + 1 < raw.size() && raw[i + 1] == '}') {
            literal.push_back('}');
            i += 2;
        } else {
            literal.push_back(raw[i]);
            ++i;
        }
    }
    if (!literal.empty()) {
        tokens.push_back(NatvisToken{false, std::move(literal), {}});
    }
    return tokens;
}

// Parses a C++ type pattern from <Type Name="std::vector&lt;*,*&gt;">.
NatvisTypePattern parseTypePattern(std::string_view raw) {
    NatvisTypePattern out;
    raw = trimAscii(raw);
    const std::size_t openAngle = raw.find('<');
    if (openAngle == std::string_view::npos) {
        out.baseName = std::string(raw);
        return out;
    }
    if (!raw.ends_with('>')) {
        out.valid = false;
        return out;
    }
    out.baseName = std::string(trimAscii(raw.substr(0, openAngle)));
    const std::string_view inner = raw.substr(openAngle + 1, raw.size() - openAngle - 2);
    int angleDepth = 0;
    std::size_t argStart = 0;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '<') {
            ++angleDepth;
        } else if (inner[i] == '>') {
            if (angleDepth == 0) {
                out.valid = false;
                return out;
            }
            --angleDepth;
        } else if (inner[i] == ',' && angleDepth == 0) {
            out.templateArgs.emplace_back(trimAscii(inner.substr(argStart, i - argStart)));
            argStart = i + 1;
        }
    }
    if (angleDepth != 0) {
        out.valid = false;
        return out;
    }
    out.templateArgs.emplace_back(trimAscii(inner.substr(argStart)));
    return out;
}

bool matchesTypePattern(const NatvisTypePattern& pattern, const NatvisTypePattern& concrete) {
    if (!pattern.valid || !concrete.valid || pattern.baseName != concrete.baseName) {
        return false;
    }
    if (pattern.templateArgs.size() != concrete.templateArgs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < pattern.templateArgs.size(); ++i) {
        if (pattern.templateArgs[i] != "*" && pattern.templateArgs[i] != concrete.templateArgs[i]) {
            return false;
        }
    }
    return true;
}

void parseExpandNode(pugi::xml_node expandNode, int depth) {
    if (depth > kMaxExpandDepth) {
        return;
    }
    (void)expandNode.attribute("HideRawView").as_bool(false);
    std::size_t count = 0;
    for (pugi::xml_node child : expandNode.children()) {
        if (++count > kMaxNodesPerType) {
            break;
        }
        const std::string_view tag = child.name();
        (void)child.attribute("Condition").value();
        (void)child.attribute("Optional").as_bool(false);
        (void)child.attribute("IncludeView").value();
        (void)child.attribute("ExcludeView").value();

        if (tag == "Item" || tag == "ExpandedItem") {
            (void)child.attribute("Name").value();
            (void)parseDisplayTemplate(child.text().get());
        } else if (tag == "ArrayItems") {
            (void)child.child("Direction").text().get();
            (void)child.child("Rank").text().as_int(1);
            (void)parseDisplayTemplate(child.child("Size").text().get());
            (void)parseDisplayTemplate(child.child("ValuePointer").text().get());
            (void)child.child("LowerBound").text().as_int(0);
        } else if (tag == "IndexListItems") {
            for (pugi::xml_node sz : child.children("Size")) {
                (void)sz.attribute("Condition").value();
                (void)parseDisplayTemplate(sz.text().get());
            }
            for (pugi::xml_node vn : child.children("ValueNode")) {
                (void)vn.attribute("Condition").value();
                (void)parseDisplayTemplate(vn.text().get());
            }
        } else if (tag == "LinkedListItems") {
            (void)parseDisplayTemplate(child.child("Size").text().get());
            (void)parseDisplayTemplate(child.child("HeadPointer").text().get());
            (void)parseDisplayTemplate(child.child("NextPointer").text().get());
            (void)parseDisplayTemplate(child.child("ValueNode").text().get());
        } else if (tag == "TreeItems") {
            (void)parseDisplayTemplate(child.child("Size").text().get());
            (void)parseDisplayTemplate(child.child("HeadPointer").text().get());
            (void)parseDisplayTemplate(child.child("LeftPointer").text().get());
            (void)parseDisplayTemplate(child.child("RightPointer").text().get());
            (void)parseDisplayTemplate(child.child("ValueNode").text().get());
        } else if (tag == "Synthetic") {
            (void)child.attribute("Name").value();
            (void)child.attribute("Expression").value();
            for (pugi::xml_node ds : child.children("DisplayString")) {
                (void)parseDisplayTemplate(ds.text().get());
            }
            if (pugi::xml_node nestedExpand = child.child("Expand")) {
                parseExpandNode(nestedExpand, depth + 1);
            }
        }
    }
}

void parseTypeNode(pugi::xml_node typeNode, const NatvisTypePattern& probeType) {
    const NatvisTypePattern primary = parseTypePattern(typeNode.attribute("Name").value());
    (void)matchesTypePattern(primary, probeType);
    (void)typeNode.attribute("Priority").value();
    (void)typeNode.attribute("Inheritable").as_bool(true);

    std::size_t count = 0;
    for (pugi::xml_node child : typeNode.children()) {
        if (++count > kMaxNodesPerType) {
            break;
        }
        const std::string_view tag = child.name();
        if (tag == "AlternativeType") {
            const NatvisTypePattern alt = parseTypePattern(child.attribute("Name").value());
            (void)matchesTypePattern(alt, probeType);
        } else if (tag == "Version") {
            (void)child.attribute("Name").value();
            (void)child.attribute("Min").value();
            (void)child.attribute("Max").value();
        } else if (tag == "Intrinsic") {
            (void)child.attribute("Name").value();
            (void)parseDisplayTemplate(child.attribute("Expression").value());
            for (pugi::xml_node param : child.children("Parameter")) {
                (void)param.attribute("Name").value();
                (void)param.attribute("Type").value();
            }
        } else if (tag == "DisplayString" || tag == "StringView") {
            (void)child.attribute("Condition").value();
            (void)child.attribute("Optional").as_bool(false);
            (void)parseDisplayTemplate(child.text().get());
        } else if (tag == "Expand") {
            parseExpandNode(child, 0);
        }
    }
}

} // namespace

void initialize() { frost::test::disableCrashDialogs(); }

Result<void> runInput(std::span<const std::uint8_t> data) {
    const std::string_view rawText(reinterpret_cast<const char*>(data.data()), data.size());
    (void)parseDisplayTemplate(rawText);
    (void)parseTypePattern(rawText);

    pugi::xml_document doc;
    const pugi::xml_parse_result result =
        doc.load_buffer(data.data(), data.size(), pugi::parse_default | pugi::parse_ws_pcdata);
    if (!result) {
        return {};
    }

    const NatvisTypePattern probeType = parseTypePattern("std::vector<int, std::allocator<int>>");
    const pugi::xml_node root = doc.child("AutoVisualizer");
    if (!root) {
        return {};
    }

    std::size_t typeCount = 0;
    for (pugi::xml_node typeNode : root.children("Type")) {
        if (++typeCount > kMaxNodesPerType) {
            break;
        }
        parseTypeNode(typeNode, probeType);
    }
    return {};
}

} // namespace frost::fuzz
