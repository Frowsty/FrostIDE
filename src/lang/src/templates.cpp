// SPDX-License-Identifier: Apache-2.0
// Authored template variables and mirrors use the same parser as language-server snippets.
#include "frost/lang/templates.hpp"
#include <algorithm>
#include <limits>

namespace frost::lang {
namespace {
constexpr std::size_t kLimit = 256 * 1024;
std::string literal(std::string_view value) {
    std::string result;
    for (char c : value) { if (c == '$' || c == '}' || c == '\\') result += '\\'; result += c; }
    return result;
}
constexpr TemplateDefinition kTemplates[]{
    {"lua.local", "local — binding", "lua", TemplateKind::Live, "local ${1:name} = ${2:value}$0"},
    {"lua.function", "local function", "lua", TemplateKind::Live, "local function ${1:name}(${2:arguments})\n    $0\nend"},
    {"lua.if", "if / then / end", "lua", TemplateKind::Live, "if ${1:condition} then\n    $0\nend"},
    {"lua.ife", "if / else", "lua", TemplateKind::Live, "if ${1:condition} then\n    $2\nelse\n    $0\nend"},
    {"lua.for", "for — numeric loop", "lua", TemplateKind::Live, "for ${1:i} = ${2:1}, ${3:count} do\n    $0\nend"},
    {"lua.pairs", "for — table pairs", "lua", TemplateKind::Live, "for ${1:key}, ${2:value} in pairs(${3:items}) do\n    $0\nend"},
    {"lua.ipairs", "for — ordered elements", "lua", TemplateKind::Live, "for ${1:index}, ${2:value} in ipairs(${3:items}) do\n    $0\nend"},
    {"lua.while", "while / do / end", "lua", TemplateKind::Live, "while ${1:condition} do\n    $0\nend"},
    {"lua.repeat", "repeat / until", "lua", TemplateKind::Live, "repeat\n    $0\nuntil ${1:condition}"},
    {"lua.table", "table constructor", "lua", TemplateKind::Live, "local ${1:name} = {\n    ${2:key} = ${3:value},\n}\n$0"},
    {"lua.module", "module table", "lua", TemplateKind::Live, "local ${1:M} = {}\n\n$0\n\nreturn ${1:M}"},
    {"lua.pcall", "protected call", "lua", TemplateKind::Live, "local ${1:ok}, ${2:result} = pcall(${3:callable})\nif not ${1:ok} then\n    $0\nend"},
    {"lua.doc", "Lua documentation", "lua", TemplateKind::Live, "--- ${1:Description}\n--- @author ${AUTHOR}\n--- @date ${DATE}\n---@param ${2:name} ${3:any}\n---@return ${4:any}\n$0"},
    {"file.lua.module", "Lua module", "lua", TemplateKind::File, "--- ${DESCRIPTION}\n--- @author ${AUTHOR}\nlocal ${CLASS} = {}\n\nreturn ${CLASS}\n"},
    {"cpp.for", "for — index loop", "cpp", TemplateKind::Live, "for (std::size_t ${1:i} = 0; ${1:i} < ${2:count}; ++${1:i}) {\n    $0\n}"},
    {"cpp.foreach", "foreach — range loop", "cpp", TemplateKind::Live, "for (auto&& ${1:item} : ${2:collection}) {\n    $0\n}"},
    {"cpp.rfor", "reverse index loop", "cpp", TemplateKind::Live, "for (std::size_t ${1:i} = ${2:count}; ${1:i}-- > 0;) {\n    $0\n}"},
    {"cpp.try", "try / catch", "cpp", TemplateKind::Live, "try {\n    $1\n} catch (const std::exception& ${2:e}) {\n    $0\n}"},
    {"cpp.cout", "print to stdout", "cpp", TemplateKind::Live, "std::cout << ${1:value} << '\\n';$0"},
    {"cpp.todo", "TODO comment", "cpp", TemplateKind::Live, "// TODO: $0"},
    {"cpp.if", "if — condition", "cpp", TemplateKind::Live, "if (${1:condition}) {\n    $0\n}"},
    {"cpp.ife", "if / else", "cpp", TemplateKind::Live, "if (${1:condition}) {\n    $2\n} else {\n    $0\n}"},
    {"cpp.class", "class definition", "cpp", TemplateKind::Live, "class ${1:ClassName} {\npublic:\n    $0\n};"},
    {"cpp.struct", "struct definition", "cpp", TemplateKind::Live, "struct ${1:StructName} {\n    $0\n};"},
    {"cpp.main", "main — entry point", "cpp", TemplateKind::Live, "int main() {\n    $0\n    return 0;\n}"},
    {"cpp.guard", "header guard", "cpp", TemplateKind::Live, "#pragma once\n$0"},
    {"cpp.doxygen", "Doxygen documentation", "cpp", TemplateKind::Live, "/**\n * @brief ${1:Description}\n * @author ${AUTHOR}\n * @date ${DATE}\n */\n$0"},
    {"cpp.postfix.if", ".if", "cpp", TemplateKind::Postfix, "if (${EXPR}) {\n    $0\n}"},
    {"cpp.postfix.for", ".for / .foreach", "cpp", TemplateKind::Postfix, "for (auto&& ${1:item} : ${EXPR}) {\n    $0\n}"},
    {"cpp.postfix.var", ".var", "cpp", TemplateKind::Postfix, "auto ${1:value} = ${EXPR};$0"},
    {"cpp.postfix.const", ".const", "cpp", TemplateKind::Postfix, "const auto ${1:value} = ${EXPR};$0"},
    {"cpp.postfix.return", ".return", "cpp", TemplateKind::Postfix, "return ${EXPR};$0"},
    {"cpp.postfix.await", ".await", "cpp", TemplateKind::Postfix, "co_await ${EXPR}$0"},
    {"cpp.postfix.notnull", ".notnull", "cpp", TemplateKind::Postfix, "if (${EXPR} != nullptr) {\n    $0\n}"},
    {"cpp.postfix.null", ".null", "cpp", TemplateKind::Postfix, "if (${EXPR} == nullptr) {\n    $0\n}"},
    {"csharp.for", "for — index loop", "csharp", TemplateKind::Live, "for (int ${1:i} = 0; ${1:i} < ${2:count}; ${1:i}++)\n{\n    $0\n}"},
    {"csharp.foreach", "foreach — collection loop", "csharp", TemplateKind::Live, "foreach (var ${1:item} in ${2:collection})\n{\n    $0\n}"},
    {"csharp.if", "if — condition", "csharp", TemplateKind::Live, "if (${1:condition})\n{\n    $0\n}"},
    {"csharp.ife", "if / else", "csharp", TemplateKind::Live, "if (${1:condition})\n{\n    $2\n}\nelse\n{\n    $0\n}"},
    {"csharp.try", "try / catch", "csharp", TemplateKind::Live, "try\n{\n    $1\n}\ncatch (${2:Exception} ${3:e})\n{\n    $0\n}"},
    {"csharp.class", "class definition", "csharp", TemplateKind::Live, "public class ${1:ClassName}\n{\n    $0\n}"},
    {"csharp.prop", "prop - auto property", "csharp", TemplateKind::Live, "public ${1:int} ${2:PropertyName} { get; set; }$0"},
    {"csharp.cw", "Console.WriteLine", "csharp", TemplateKind::Live, "Console.WriteLine(${1:value});$0"},
    {"csharp.todo", "TODO comment", "csharp", TemplateKind::Live, "// TODO: $0"},
    {"cmake.if", "if / endif", "cmake", TemplateKind::Live, "if(${1:condition})\n    $0\nendif()"},
    {"cmake.foreach", "foreach — list", "cmake", TemplateKind::Live, "foreach(${1:item} IN LISTS ${2:items})\n    $0\nendforeach()"},
    {"cmake.function", "function definition", "cmake", TemplateKind::Live, "function(${1:name})\n    $0\nendfunction()"},
    {"cmake.target", "new executable target", "cmake", TemplateKind::Live, "add_executable(${1:target}\n    ${2:main.cpp}\n)\ntarget_compile_features(${1:target} PRIVATE cxx_std_20)\n$0"},
    {"cmake.library", "new library target", "cmake", TemplateKind::Live, "add_library(${1:target} STATIC\n    ${2:source.cpp}\n)\ntarget_include_directories(${1:target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})\n$0"},
    {"cmake.postfix.if", ".if", "cmake", TemplateKind::Postfix, "if(${EXPR})\n    $0\nendif()"},
    {"cmake.postfix.for", ".for / .foreach", "cmake", TemplateKind::Postfix, "foreach(${1:item} IN LISTS ${EXPR})\n    $0\nendforeach()"},
    {"file.cpp.class.header", "C++ class header", "cpp", TemplateKind::File, "#pragma once\n\n/** @brief ${DESCRIPTION}\n * @author ${AUTHOR}\n */\nclass ${CLASS} {\npublic:\n    ${CLASS}();\n};\n"},
    {"file.cpp.class.source", "C++ class implementation", "cpp", TemplateKind::File, "#include \"${HEADER}\"\n\n${CLASS}::${CLASS}() = default;\n"},
    {"file.cpp.source", "C++ source file", "cpp", TemplateKind::File, "// ${DESCRIPTION}\n// Author: ${AUTHOR}\n$0"},
    {"file.cpp.header", "C++ header file", "cpp", TemplateKind::File, "#pragma once\n\n// ${DESCRIPTION}\n// Author: ${AUTHOR}\n$0"},
    {"file.cmake", "CMake script", "cmake", TemplateKind::File, "# ${DESCRIPTION}\n# Author: ${AUTHOR}\n$0"},
};
}
std::span<const TemplateDefinition> builtinTemplates() noexcept { return kTemplates; }
const TemplateDefinition* findTemplate(std::string_view id) noexcept {
    const auto found = std::ranges::find(kTemplates, id, &TemplateDefinition::id);
    return found == std::end(kTemplates) ? nullptr : found;
}
Result<TemplateExpansion> expandTemplate(std::string_view body, const TemplateVariables& variables) {
    if (body.size() > kLimit || variables.size() > 128) return fail(ErrorCode::OutOfRange, "Template input limit exceeded");
    for (const auto& [name,value] : variables) if (name.size() > 128 || value.size() > 64 * 1024 || value.find('\0') != value.npos)
        return fail(ErrorCode::InvalidArgument, "Template variable is invalid or oversized");
    std::string raw;
    std::size_t nesting = 0;
    for (std::size_t i = 0; i < body.size();) {
        if (body[i] == '\\' && i + 1 < body.size()) {
            if (raw.size() > kLimit-2) return fail(ErrorCode::OutOfRange,"Template output limit exceeded");
            raw.append(body.substr(i,2)); i += 2; continue;
        }
        if (body.substr(i,2) == "${" && i + 2 < body.size() && (body[i+2] == '_' || (body[i+2] >= 'A' && body[i+2] <= 'Z'))) {
            const auto end = body.find('}',i+2);
            if (end == body.npos) return fail(ErrorCode::InvalidArgument, "Unclosed template variable");
            const auto name = body.substr(i+2,end-i-2);
            // CMake's runtime variable is deliberately literal, not an IDE context variable.
            if (name == "CMAKE_CURRENT_SOURCE_DIR") raw += "\\${CMAKE_CURRENT_SOURCE_DIR\\}";
            else {
                const auto value = variables.find(name);
                if (value == variables.end()) return fail(ErrorCode::InvalidArgument, "Missing template input: " + std::string(name));
                raw += literal(value->second);
            }
            i = end+1;
        } else {
            if (body.substr(i,2) == "${" && ++nesting > 64) return fail(ErrorCode::OutOfRange,"Template nesting limit exceeded");
            if (body[i] == '}' && nesting) --nesting;
            raw += body[i++];
        }
        if (raw.size() > kLimit) return fail(ErrorCode::OutOfRange, "Template output limit exceeded");
    }
    if (nesting) return fail(ErrorCode::InvalidArgument,"Unclosed template placeholder");
    TemplateExpansion result{std::move(raw),{}};
    result.parsed = parseSnippet(result.snippet);
    if (result.parsed.budgetExceeded) return fail(ErrorCode::OutOfRange,"Template syntax exceeds parser work/depth budget");
    return result;
}
Result<TemplateAdvance> advanceTemplate(const LiveTemplateSession& session, std::string_view edited, std::uint32_t caret) {
    if (edited.size() > kLimit || session.expected.text.size() > kLimit || session.expected.tabStops.size() > 4096 || session.active >= session.expected.tabStops.size()) return fail(ErrorCode::InvalidArgument,"Template session ended");
    for (const auto& stop : session.expected.tabStops) if (stop.begin > stop.end || stop.end > session.expected.text.size())
        return fail(ErrorCode::InvalidArgument,"Template stop is outside its text snapshot");
    const auto current = session.expected.tabStops[session.active];
    const auto delta = static_cast<std::int64_t>(edited.size()) - static_cast<std::int64_t>(session.expected.text.size());
    const auto end = static_cast<std::int64_t>(current.end) + delta;
    if (end < current.begin || end > static_cast<std::int64_t>(edited.size()) || caret < current.begin || caret > end ||
        edited.substr(0,current.begin) != std::string_view(session.expected.text).substr(0,current.begin) ||
        edited.substr(static_cast<std::size_t>(end)) != std::string_view(session.expected.text).substr(current.end))
        return fail(ErrorCode::Busy,"Edits outside the active template placeholder ended the session");
    const std::string value(edited.substr(current.begin,static_cast<std::size_t>(end)-current.begin));
    if (delta || edited != session.expected.text) for (std::size_t i = 0; i < session.expected.tabStops.size(); ++i) {
        const auto& other = session.expected.tabStops[i];
        if (i != session.active && other.index != current.index && other.begin < current.end && other.end > current.begin)
            return fail(ErrorCode::Unsupported,"Editing nested placeholders ends the template session");
    }
    TemplateAdvance result; result.next = session;
    for (std::size_t i = 0; i < result.next.expected.tabStops.size(); ++i) {
        auto& stop = result.next.expected.tabStops[i];
        if (i == session.active) { stop.end = static_cast<std::uint32_t>(end); continue; }
        // Empty placeholders share a byte position but not insertion affinity: later stops
        // (especially final $0) follow inserted text; already visited stops remain before it.
        const bool following = current.begin != current.end ? stop.begin >= current.end :
            (stop.begin > current.begin || (stop.begin == current.begin && i > session.active));
        if (following) {
            stop.begin = static_cast<std::uint32_t>(static_cast<std::int64_t>(stop.begin)+delta);
            stop.end = static_cast<std::uint32_t>(static_cast<std::int64_t>(stop.end)+delta);
        }
    }
    std::vector<SnippetTabStop> mirrors;
    for (std::size_t i = 0; i < result.next.expected.tabStops.size(); ++i) {
        const auto& stop = result.next.expected.tabStops[i];
        if (i != session.active && stop.index == current.index) mirrors.push_back(stop);
    }
    std::ranges::sort(mirrors,{},&SnippetTabStop::begin);
    std::uint32_t previousEnd = 0;
    std::int64_t outputBytes = static_cast<std::int64_t>(edited.size());
    for (const auto& edit : mirrors) {
        if (edit.begin < previousEnd || edit.end < edit.begin || edit.end > edited.size()) return fail(ErrorCode::Unsupported,"Nested/overlapping linked placeholders are not editable");
        previousEnd = edit.end;
        outputBytes += static_cast<std::int64_t>(value.size()) - (edit.end-edit.begin);
    }
    if (outputBytes < 0 || outputBytes > static_cast<std::int64_t>(kLimit)) return fail(ErrorCode::OutOfRange,"Linked template output limit exceeded");
    for (const auto& stop : mirrors) result.edits.push_back({stop.begin,stop.end,value});
    std::string after(edited);
    for (auto it = result.edits.rbegin(); it != result.edits.rend(); ++it) after.replace(it->begin,it->end-it->begin,it->text);
    for (auto& stop : result.next.expected.tabStops) {
        std::int64_t shift = 0;
        for (const auto& edit : result.edits) {
            if (stop.index == current.index && edit.begin == stop.begin && edit.end == stop.end) { stop.end = stop.begin + static_cast<std::uint32_t>(edit.text.size()); break; }
            if (edit.end <= stop.begin) shift += static_cast<std::int64_t>(edit.text.size()) - (edit.end-edit.begin);
        }
        stop.begin = static_cast<std::uint32_t>(static_cast<std::int64_t>(stop.begin)+shift);
        stop.end = static_cast<std::uint32_t>(static_cast<std::int64_t>(stop.end)+shift);
    }
    result.next.expected.text = std::move(after);
    auto next = session.active+1;
    while (next < result.next.expected.tabStops.size() && result.next.expected.tabStops[next].index == current.index) ++next;
    result.next.active = static_cast<std::uint32_t>(next);
    if (next < result.next.expected.tabStops.size()) result.selection = result.next.expected.tabStops[next];
    return result;
}
} // namespace frost::lang
