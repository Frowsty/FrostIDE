// SPDX-License-Identifier: Apache-2.0
// Shared authored templates generate plain UTF-8; caller chooses actual context and applies once.
#include "frost/lang/file_templates.hpp"

#include <algorithm>
#include <array>
namespace frost::lang {
Result<std::vector<GeneratedTemplateFile>> renderFileTemplate(const FileTemplateRequest& request) {
    if (request.name.empty() || request.name.size() > 256 || request.author.size() > 256 ||
        request.description.size() > 4096 || request.author.find_first_of("\r\n") != request.author.npos ||
        request.description.find_first_of("\r\n") != request.description.npos ||
        request.author.find("*/") != request.author.npos || request.description.find("*/") != request.description.npos)
        return fail(ErrorCode::InvalidArgument,
                    "Provide a bounded name and single-line comment-safe author/description");
    constexpr std::string_view keywords =
        " alignas alignof and and_eq asm atomic_cancel atomic_commit atomic_noexcept auto bitand bitor bool break case "
        "catch char char8_t char16_t char32_t class compl concept const consteval constexpr constinit const_cast "
        "continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum explicit export "
        "extern false float for friend goto if inline int long mutable namespace new noexcept not not_eq nullptr "
        "operator or or_eq private protected public reflexpr register reinterpret_cast requires return short signed "
        "sizeof static static_assert static_cast struct switch synchronized template this thread_local throw true try "
        "typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq ";
    std::vector<std::string> components;
    for (std::size_t start = 0; start <= request.name.size();) {
        const auto end = request.name.find("::", start);
        const auto part = request.name.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() ||
            !((part.front() >= 'A' && part.front() <= 'Z') || (part.front() >= 'a' && part.front() <= 'z') ||
              part.front() == '_') ||
            keywords.find(" " + part + " ") != keywords.npos)
            return fail(ErrorCode::InvalidArgument, "Use a valid C++ identifier for each namespace/name component");
        for (char c : part)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
                return fail(ErrorCode::InvalidArgument, "Template names do not contain paths or punctuation");
        components.push_back(part);
        if (end == std::string::npos)
            break;
        start = end + 2;
    }
    if (request.kind != FileTemplateKind::Class && components.size() != 1)
        return fail(ErrorCode::InvalidArgument, "Only class templates accept namespace-qualified names");
    const auto& name = components.back();
    TemplateVariables variables{
        {"CLASS", name}, {"HEADER", name + ".hpp"}, {"AUTHOR", request.author}, {"DESCRIPTION", request.description}};
    std::vector<GeneratedTemplateFile> result;
    auto append = [&](std::string filename, std::string_view id) -> Result<void> {
        const auto* definition = findTemplate(id);
        if (!definition)
            return fail(ErrorCode::Internal, "Missing built-in file template");
        auto rendered = expandTemplate(definition->body, variables);
        if (!rendered)
            return std::unexpected(rendered.error());
        result.push_back({std::move(filename), std::move(rendered->parsed.text)});
        return {};
    };
    switch (request.kind) {
    case FileTemplateKind::Class: {
        if (auto generated = append(name + ".hpp", "file.cpp.class.header"); !generated)
            return std::unexpected(generated.error());
        if (auto generated = append(name + ".cpp", "file.cpp.class.source"); !generated)
            return std::unexpected(generated.error());
        if (components.size() > 1) {
            std::string space;
            for (std::size_t i = 0; i + 1 < components.size(); ++i) {
                if (i)
                    space += "::";
                space += components[i];
            }
            auto& header = result[0].contents;
            const auto begin = header.find("/**");
            if (begin == header.npos)
                return fail(ErrorCode::Internal, "Class template has no documentation anchor");
            header.insert(begin, "namespace " + space + " {\n");
            header += "} // namespace " + space + "\n";
            auto& source = result[1].contents;
            const auto constructor = source.find(name + "::");
            if (constructor == source.npos)
                return fail(ErrorCode::Internal, "Class template has no constructor");
            source.insert(constructor, space + "::");
        }
        break;
    }
    case FileTemplateKind::Source:
        if (auto made = append(name + ".cpp", "file.cpp.source"); !made)
            return std::unexpected(made.error());
        break;
    case FileTemplateKind::Header:
        if (auto made = append(name + ".hpp", "file.cpp.header"); !made)
            return std::unexpected(made.error());
        break;
    case FileTemplateKind::CMakeScript:
        if (auto made = append(name + ".cmake", "file.cmake"); !made)
            return std::unexpected(made.error());
        break;
    case FileTemplateKind::Executable: {
        auto rendered = expandTemplate(findTemplate("cpp.main")->body, {});
        if (!rendered)
            return std::unexpected(rendered.error());
        auto header = expandTemplate(findTemplate("file.cpp.source")->body, variables);
        if (!header)
            return std::unexpected(header.error());
        result.push_back({name + ".cpp", std::move(header->parsed.text) + "\n" + std::move(rendered->parsed.text)});
        break;
    }
    }
    return result;
}
} // namespace frost::lang
