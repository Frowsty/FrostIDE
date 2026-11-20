// SPDX-License-Identifier: Apache-2.0
// Original lexical implementation from public HLSL and Unity ShaderLab documentation.
// Token vocabulary: https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-appendix-keywords
// Block/include profile:
// https://docs.unity.com/en-us/engine/6000.0/manual/materials-and-shaders/shaders/reference/sl-reference/sl-sub-shader-pass/shader-shaderlab-code-blocks
#include "frost/syntax/shader_source.hpp"

#include <algorithm>
namespace frost::syntax {
namespace {
bool head(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool tail(unsigned char c) { return head(c) || (c >= '0' && c <= '9'); }
bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
text::ByteRange range(std::size_t a, std::size_t b) { return {ByteOffset{a}, ByteOffset{b}}; }
bool oneOf(std::string_view word, std::initializer_list<std::string_view> words) {
    return std::ranges::find(words, word) != words.end();
}
bool builtinType(std::string_view word) {
    for (auto type :
         {"float", "half", "double", "int", "uint", "bool", "min16float", "min10float", "min16int", "min16uint"}) {
        const std::string_view base(type);
        if (!word.starts_with(base))
            continue;
        const auto suffix = word.substr(base.size());
        if (suffix.empty() || (suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '4') ||
            (suffix.size() == 3 && suffix[0] >= '1' && suffix[0] <= '4' && suffix[1] == 'x' && suffix[2] >= '1' &&
             suffix[2] <= '4'))
            return true;
    }
    return oneOf(word, {"void", "vector", "matrix", "Texture1D", "Texture2D", "Texture3D", "TextureCube",
                        "Texture2DArray", "RWTexture2D", "Buffer", "RWBuffer", "StructuredBuffer", "RWStructuredBuffer",
                        "ByteAddressBuffer", "RWByteAddressBuffer", "SamplerState", "SamplerComparisonState"});
}
} // namespace
ShaderSourceKind shaderSourceKind(std::string_view path) noexcept {
    const auto dot = path.find_last_of('.');
    if (dot == path.npos)
        return ShaderSourceKind::None;
    auto ext = path.substr(dot);
    char lower[12];
    if (ext.size() > sizeof(lower))
        return ShaderSourceKind::None;
    for (std::size_t i = 0; i < ext.size(); ++i)
        lower[i] = ext[i] >= 'A' && ext[i] <= 'Z' ? static_cast<char>(ext[i] - 'A' + 'a') : ext[i];
    ext = std::string_view(lower, ext.size());
    if (ext == ".shader")
        return ShaderSourceKind::ShaderLab;
    return oneOf(ext, {".hlsl", ".hlsli", ".fx", ".cginc", ".compute"}) ? ShaderSourceKind::Hlsl
                                                                        : ShaderSourceKind::None;
}
ShaderSourceScan scanShaderSource(std::string_view s, ShaderSourceKind kind, std::stop_token stop) {
    ShaderSourceScan out;
    if (kind == ShaderSourceKind::None)
        return out;
    if (s.size() > kShaderMaxSourceBytes) {
        out.complete = false;
        return out;
    }
    bool program = kind == ShaderSourceKind::Hlsl, linePrefix = true;
    unsigned conditional = 0;
    auto add = [&](std::size_t a, std::size_t b, HighlightKind k) {
        if (a < b)
            out.spans.push_back({range(a, b), k});
    };
    for (std::size_t i = 0; i < s.size();) {
        if (stop.stop_requested()) {
            out.complete = false;
            break;
        }
        if (out.spans.size() >= kShaderMaxSpans || out.includes.size() >= kShaderMaxIncludes) {
            out.complete = false;
            break;
        }
        const auto start = i;
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size() &&
            (s[i + 1] == '\n' || (s[i + 1] == '\r' && i + 2 < s.size() && s[i + 2] == '\n'))) {
            i += s[i + 1] == '\n' ? 2 : 3;
            continue;
        }
        if (c == '\n') {
            ++i;
            linePrefix = true;
            continue;
        }
        if (space(c)) {
            ++i;
            continue;
        }
        if (s.substr(i, 2) == "//") {
            i += 2;
            while (i < s.size()) {
                if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\n') {
                    i += 2;
                    continue;
                }
                if (s[i] == '\\' && i + 2 < s.size() && s[i + 1] == '\r' && s[i + 2] == '\n') {
                    i += 3;
                    continue;
                }
                if (s[i] == '\n')
                    break;
                ++i;
            }
            add(start, i, HighlightKind::Comment);
            continue;
        }
        if (s.substr(i, 2) == "/*") {
            auto end = s.find("*/", i + 2);
            i = end == s.npos ? s.size() : end + 2;
            if (s.substr(start, i - start).find('\n') != s.npos)
                linePrefix = true;
            add(start, i, HighlightKind::Comment);
            continue;
        }
        if (c == '#' && linePrefix && program) {
            ++i;
            while (i < s.size() && space(s[i]))
                ++i;
            const auto wordStart = i;
            while (i < s.size() && tail(static_cast<unsigned char>(s[i])))
                ++i;
            const auto directive = s.substr(wordStart, i - wordStart);
            add(start, i, HighlightKind::Directive);
            if (oneOf(directive, {"if", "ifdef", "ifndef"}))
                ++conditional;
            else if (directive == "endif" && conditional)
                --conditional;
            if (directive == "include" || directive == "include_with_pragmas") {
                while (i < s.size() && space(s[i]))
                    ++i;
                ShaderInclude include;
                include.directive = directive;
                include.conditional = conditional != 0;
                if (i < s.size() && (s[i] == '"' || s[i] == '<')) {
                    include.quoted = s[i] == '"';
                    const char end = include.quoted ? '"' : '>';
                    const auto tokenStart = i++;
                    const auto nameStart = i;
                    while (i < s.size() && s[i] != end && s[i] != '\n' && s[i] != '\\')
                        ++i;
                    if (i < s.size() && s[i] == end) {
                        include.literal = true;
                        include.target = s.substr(nameStart, i - nameStart);
                        include.range = range(nameStart, i);
                        ++i;
                    } else {
                        include.range = range(tokenStart, i);
                        include.target = s.substr(tokenStart, i - tokenStart);
                    }
                    add(tokenStart, i, HighlightKind::String);
                } else {
                    const auto nameStart = i;
                    while (i < s.size() && s[i] != '\n' && !space(s[i]))
                        ++i;
                    include.target = s.substr(nameStart, i - nameStart);
                    include.range = range(nameStart, i);
                    add(nameStart, i, HighlightKind::Macro);
                }
                const auto endOfLine = s.find('\n', i);
                auto trailing = s.substr(i, endOfLine == s.npos ? s.size() - i : endOfLine - i);
                const auto comment = trailing.find("//");
                if (comment != trailing.npos)
                    trailing = trailing.substr(0, comment);
                while (!trailing.empty() && space(trailing.back()))
                    trailing.remove_suffix(1);
                if (!trailing.empty() && trailing.back() == '\\')
                    include.literal = false;
                while (!trailing.empty() && space(trailing.front()))
                    trailing.remove_prefix(1);
                if (!trailing.empty() && !trailing.starts_with("/*"))
                    include.literal = false;
                out.includes.push_back(std::move(include));
            }
            linePrefix = false;
            continue;
        }
        const bool atLinePrefix = linePrefix;
        linePrefix = false;
        if (c == '"' || c == '\'') {
            ++i;
            while (i < s.size() && s[i] != c && s[i] != '\n') {
                if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\n') {
                    i += 2;
                    continue;
                }
                if (s[i] == '\\' && i + 2 < s.size() && s[i + 1] == '\r' && s[i + 2] == '\n') {
                    i += 3;
                    continue;
                }
                if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] != '\n')
                    i += 2;
                else
                    ++i;
            }
            if (i < s.size() && s[i] == c)
                ++i;
            add(start, i, c == '"' ? HighlightKind::String : HighlightKind::Character);
            continue;
        }
        if (head(static_cast<unsigned char>(c))) {
            ++i;
            while (i < s.size() && tail(static_cast<unsigned char>(s[i])))
                ++i;
            const auto word = s.substr(start, i - start);
            if (kind == ShaderSourceKind::ShaderLab && atLinePrefix &&
                oneOf(word, {"HLSLPROGRAM", "HLSLINCLUDE", "CGPROGRAM", "CGINCLUDE", "ENDHLSL", "ENDCG"})) {
                program = word != "ENDHLSL" && word != "ENDCG";
                conditional = 0;
                add(start, i, HighlightKind::Directive);
            } else if (program && builtinType(word))
                add(start, i, HighlightKind::BuiltinType);
            else if (word == "true" || word == "false")
                add(start, i, HighlightKind::Boolean);
            else if (program && oneOf(word, {"break",
                                             "case",
                                             "cbuffer",
                                             "centroid",
                                             "class",
                                             "column_major",
                                             "const",
                                             "continue",
                                             "default",
                                             "discard",
                                             "do",
                                             "else",
                                             "export",
                                             "extern",
                                             "for",
                                             "groupshared",
                                             "if",
                                             "in",
                                             "inline",
                                             "inout",
                                             "interface",
                                             "linear",
                                             "nointerpolation",
                                             "noperspective",
                                             "out",
                                             "precise",
                                             "register",
                                             "return",
                                             "row_major",
                                             "sample",
                                             "shared",
                                             "snorm",
                                             "static",
                                             "struct",
                                             "switch",
                                             "tbuffer",
                                             "typedef",
                                             "uniform",
                                             "unorm",
                                             "volatile",
                                             "while"}))
                add(start, i, HighlightKind::Keyword);
            else if (!program &&
                     oneOf(word,
                           {"Shader",       "SubShader", "Properties", "Pass",        "Tags",    "LOD",    "Fallback",
                            "CustomEditor", "UsePass",   "Name",       "Blend",       "BlendOp", "ZWrite", "ZTest",
                            "Cull",         "ColorMask", "Offset",     "Stencil",     "Ref",     "Comp",   "Fail",
                            "ZFail",        "ReadMask",  "WriteMask",  "AlphaToMask", "Color",   "Float",  "Range",
                            "Int",          "Integer",   "Vector",     "Cube"}))
                add(start, i, HighlightKind::Keyword);
            continue;
        }
        if (c >= '0' && c <= '9') {
            ++i;
            while (i < s.size() && (tail(static_cast<unsigned char>(s[i])) || s[i] == '.' ||
                                    ((s[i] == '+' || s[i] == '-') && (s[i - 1] == 'e' || s[i - 1] == 'E'))))
                ++i;
            add(start, i, HighlightKind::Number);
            continue;
        }
        ++i;
        if (oneOf(s.substr(start, 1), {"{", "}", "[", "]", "(", ")"}))
            add(start, i, HighlightKind::Bracket);
        else if (oneOf(s.substr(start, 1), {";", ",", ":"}))
            add(start, i, HighlightKind::Punctuation);
        else if (oneOf(s.substr(start, 1), {"+", "-", "*", "/", "=", "<", ">", "!", "&", "|", "^", "%", "~", "?"}))
            add(start, i, HighlightKind::Operator);
    }
    return out;
}
} // namespace frost::syntax
