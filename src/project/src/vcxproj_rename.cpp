// SPDX-License-Identifier: Apache-2.0
#include "frost/project/vcxproj_rename.hpp"

#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"
#include "frost/project/vcxproj.hpp"
#include "paths.hpp"
#include "text_util.hpp"
#include "xml.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <set>
#include <sstream>
#include <string_view>

namespace frost::project {
namespace {

bool equalPath(std::string_view left, std::string_view right) {
    auto a = detail::normalizeAbsolute(left);
    auto b = detail::normalizeAbsolute(right);
    return a && b && detail::pathKey(*a) == detail::pathKey(*b);
}

bool literal(std::string_view value) {
    return !value.empty() && value.find_first_of("$@%*?;\r\n") == std::string_view::npos &&
           value.find('\0') == std::string_view::npos;
}

std::string generic(std::string value) {
    std::ranges::replace(value, '\\', '/');
    return value;
}

std::vector<std::string> components(std::string_view value) {
    std::vector<std::string> result;
    for (std::size_t offset = 0; offset < value.size();) {
        const auto end = value.find('/', offset);
        const auto part = value.substr(offset, end == std::string_view::npos ? end : end - offset);
        if (!part.empty())
            result.emplace_back(part);
        if (end == std::string_view::npos)
            break;
        offset = end + 1;
    }
    return result;
}

// Preserve relative/absolute spelling and slash style. Resolve the resulting spelling below,
// so a different volume or a lexical root cannot silently produce a different source path.
std::string replacementInclude(std::string_view old, std::string_view base, std::string_view target) {
    const char separator = old.find('\\') == std::string_view::npos ? '/' : '\\';
    std::string result = generic(std::string(target));
    if (!io::isAbsolutePath(detail::toNativeSeparators(old))) {
        auto parents = components(generic(std::string(base)));
        auto destination = components(result);
        std::size_t common = 0;
        while (common < parents.size() && common < destination.size() &&
               detail::pathKey(parents[common]) == detail::pathKey(destination[common]))
            ++common;
        if (common > 0) {
            result.clear();
            for (std::size_t i = common; i < parents.size(); ++i)
                result += "../";
            for (std::size_t i = common; i < destination.size(); ++i) {
                if (!result.empty() && result.back() != '/')
                    result += '/';
                result += destination[i];
            }
        }
    }
    std::ranges::replace(result, '/', separator);
    return result;
}

const VcxprojRenameInput* findInput(std::span<const VcxprojRenameInput> inputs, std::string_view path) {
    for (const auto& input : inputs)
        if (equalPath(input.path, path))
            return &input;
    return nullptr;
}

Result<void> load(pugi::xml_document& xml, const VcxprojRenameInput& input) {
    if (!input.contents || input.contents->size() > 8 * 1024 * 1024 ||
        !io::isValidUtf8(*input.contents) || input.contents->find('\0') != std::string::npos)
        return fail(ErrorCode::Unsupported, "Capture readable UTF-8 project XML smaller than 8 MiB");
    auto parsed = xml.load_buffer(input.contents->data(), input.contents->size(),
                                  pugi::parse_full | pugi::parse_ws_pcdata, pugi::encoding_utf8);
    if (!parsed || !detail::isElement(xml.document_element(), "Project"))
        return fail(ErrorCode::Corrupt, std::format("{}: malformed MSBuild Project XML", input.path));
    for (auto node : xml.children())
        if (node.type() == pugi::node_doctype)
            return fail(ErrorCode::Unsupported, "Remove the XML DOCTYPE before transactional membership editing");
    return {};
}

struct XmlPlan {
    std::string after;
    std::size_t matched = 0;
    bool imports = false;
};

Result<XmlPlan> transform(const VcxprojRenameInput& input, std::string_view from, std::string_view to,
                          bool filters) {
    pugi::xml_document xml;
    if (auto loaded = load(xml, input); !loaded)
        return std::unexpected(loaded.error());
    const std::string base(io::parentPath(input.path));
    XmlPlan result;
    std::optional<Error> error;
    std::function<void(pugi::xml_node)> visit = [&](pugi::xml_node node) {
        if (error)
            return;
        std::set<std::string> attributes;
        for (auto attribute : node.attributes())
            if (!attributes.insert(detail::toLowerAscii(attribute.name())).second) {
                error = Error(ErrorCode::Corrupt, "Duplicate case-insensitive XML attributes are ambiguous");
                return;
            }
        if (detail::hasAttribute(node, "Condition")) {
            std::string condition(detail::attribute(node, "Condition"));
            for (std::string_view property : {"$(Configuration)", "$(Platform)"})
                for (auto at = condition.find(property); at != std::string::npos; at = condition.find(property))
                    condition.erase(at, property.size());
            if (condition.find_first_of("$@%()") != std::string::npos ||
                detail::toLowerAscii(condition).find("exists") != std::string::npos) {
                error = Error(ErrorCode::Unsupported,
                              "Use the MSBuild plan-only route for file-dependent or non-configuration conditions");
                return;
            }
        }
        if (detail::isElement(node, "Import") || detail::isElement(node, "Sdk") ||
            (node == xml.document_element() && detail::hasAttribute(node, "Sdk")))
            result.imports = true;
        if (node.type() == pugi::node_element && fileItemTypeOf(detail::localName(node))) {
            for (std::string_view operation : {"Include", "Update", "Remove"}) {
                if (!detail::hasAttribute(node, operation))
                    continue;
                const std::string old(detail::attribute(node, operation));
                if (!literal(old)) {
                    error = Error(ErrorCode::Unsupported,
                                  "Use explicit literal file items; wildcard/list/property membership is ambiguous");
                    return;
                }
                auto resolved = detail::resolvePath(base, old);
                if (!resolved) {
                    error = resolved.error();
                    return;
                }
                const bool selected = equalPath(*resolved, from);
                if (!equalPath(from, to) && equalPath(*resolved, to)) {
                    error = Error(ErrorCode::AlreadyExists, "The destination already has a project file item");
                    return;
                }
                if (!selected)
                    continue;
                if (operation != "Include" || !detail::isElement(node.parent(), "ItemGroup") ||
                    node.parent().parent() != xml.document_element() || detail::hasAttribute(node, "Exclude")) {
                    error = Error(ErrorCode::Unsupported,
                                  "Move the selected membership into a direct literal Project/ItemGroup Include");
                    return;
                }
                auto replacement = replacementInclude(old, base, to);
                auto checked = detail::resolvePath(base, replacement);
                if (!checked || !equalPath(*checked, to)) {
                    error = Error(ErrorCode::Unsupported, "The destination cannot be represented in this project");
                    return;
                }
                for (auto attribute : node.attributes())
                    if (detail::iequals(attribute.name(), "Include") && !attribute.set_value(replacement.c_str())) {
                        error = Error(ErrorCode::IoError, "Could not allocate the planned XML Include value");
                        return;
                    }
                // A Link is a display path: preserve its virtual directory and an intentional alias.
                for (auto child : node.children()) {
                    if (!detail::isElement(child, "Link"))
                        continue;
                    std::string link(detail::elementText(child));
                    if (!literal(link) || !child.first_child() || child.first_child().next_sibling() ||
                        (child.first_child().type() != pugi::node_pcdata && child.first_child().type() != pugi::node_cdata)) {
                        error = Error(ErrorCode::Unsupported, "Use literal Link metadata before renaming this item");
                        return;
                    }
                    const auto oldLeaf = generic(std::string(from));
                    const auto newLeaf = generic(std::string(to));
                    const auto slash = link.find_last_of("/\\");
                    const auto leaf = link.substr(slash == std::string::npos ? 0 : slash + 1);
                    if (detail::pathKey(leaf) == detail::pathKey(oldLeaf.substr(oldLeaf.find_last_of('/') + 1))) {
                        link.replace(slash == std::string::npos ? 0 : slash + 1, std::string::npos,
                                     newLeaf.substr(newLeaf.find_last_of('/') + 1));
                        if (!child.text().set(link.c_str())) {
                            error = Error(ErrorCode::IoError, "Could not allocate the planned XML Link value");
                            return;
                        }
                    }
                }
                ++result.matched;
            }
        }
        for (auto child : node.children())
            visit(child);
    };
    visit(xml.document_element());
    if (error)
        return std::unexpected(*error);
    if (filters && result.imports)
        return fail(ErrorCode::Unsupported, "Imported filters cannot be rewritten as local membership");
    if (!result.matched) {
        result.after = *input.contents;
        return result;
    }
    std::ostringstream stream;
    xml.save(stream, "", pugi::format_raw | pugi::format_no_declaration, pugi::encoding_utf8);
    result.after = stream.str();
    if (input.contents->starts_with("\xef\xbb\xbf"))
        result.after.insert(0, "\xef\xbb\xbf");
    return result;
}

} // namespace

Result<VcxprojRenamePlan> planVcxprojRename(const VcxprojRenameRequest& request) {
    if (request.inputs.size() > 512 || !literal(request.fromPath) || !literal(request.toPath) ||
        !detail::normalizeAbsolute(request.projectPath) ||
        !detail::normalizeAbsolute(request.fromPath) || !detail::normalizeAbsolute(request.toPath) ||
        request.fromPath == request.toPath)
        return fail(ErrorCode::InvalidArgument, "A membership rename needs distinct absolute source paths and bounded inputs");
    if (!detail::iendsWith(request.projectPath, ".vcxproj"))
        return fail(ErrorCode::InvalidArgument, "Select a .vcxproj definition for this rename");
    for (std::size_t i = 0; i < request.inputs.size(); ++i) {
        if (!detail::normalizeAbsolute(request.inputs[i].path))
            return fail(ErrorCode::InvalidArgument, "Every project input snapshot needs an absolute path");
        for (std::size_t j = 0; j < i; ++j)
            if (equalPath(request.inputs[i].path, request.inputs[j].path))
                return fail(ErrorCode::InvalidArgument, "Duplicate project input snapshots are ambiguous");
    }
    const auto* definition = findInput(request.inputs, request.projectPath);
    const auto* filters = findInput(request.inputs, request.projectPath + ".filters");
    if (!definition || !definition->contents || !filters)
        return fail(ErrorCode::InvalidArgument, "Capture the project and the presence or absence of its .filters file");
    if (equalPath(request.fromPath, definition->path) || equalPath(request.toPath, definition->path) ||
        equalPath(request.fromPath, filters->path) || equalPath(request.toPath, filters->path))
        return fail(ErrorCode::Unsupported, "Rename project definitions through the solution operation, not source membership");
    auto project = transform(*definition, request.fromPath, request.toPath, false);
    if (!project)
        return std::unexpected(project.error());
    if (!project->matched)
        return fail(ErrorCode::NotFound, "No explicit local project Include owns the selected file");
    if (project->imports)
        return fail(ErrorCode::Unsupported,
                    "Use the plan-only MSBuild helper to validate imported/SDK membership before and after rename");
    VcxprojRenamePlan plan;
    plan.dependencies.assign(request.inputs.begin(), request.inputs.end());
    plan.replacements.push_back({definition->path, *definition->contents, std::move(project->after)});
    if (filters->contents) {
        auto layout = transform(*filters, request.fromPath, request.toPath, true);
        if (!layout)
            return std::unexpected(layout.error());
        if (layout->matched)
            plan.replacements.push_back({filters->path, *filters->contents, std::move(layout->after)});
    }
    return plan;
}

Result<VcxprojRenamePlan> planVcxprojRenameWithHost(io::IProcessLauncher& launcher,
                                                    const ProjectEditLaunch& launch,
                                                    const ProjectEdit& edit,
                                                    std::span<const VcxprojRenameInput> inputs,
                                                    std::stop_token stop) {
    const auto* definition = findInput(inputs, edit.definition);
    const auto* filters = findInput(inputs, edit.definition + ".filters");
    if (!definition || !definition->contents || !filters)
        return fail(ErrorCode::InvalidArgument, "Capture the project and filters presence before membership planning");
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (!detail::normalizeAbsolute(inputs[i].path))
            return fail(ErrorCode::InvalidArgument, "Every input receipt needs an absolute path");
        for (std::size_t j = 0; j < i; ++j)
            if (equalPath(inputs[i].path, inputs[j].path))
                return fail(ErrorCode::InvalidArgument, "Duplicate input receipts are ambiguous");
    }
    auto planned = runProjectEditPlan(launcher, launch, edit, stop);
    if (!planned)
        return std::unexpected(planned.error());
    if (*definition->contents != planned->projectBefore || filters->contents != planned->filtersBefore)
        return fail(ErrorCode::Busy, "Project/filter buffers differ from the MSBuild plan; save or reload and preview again");
    VcxprojRenamePlan result;
    result.dependencies.assign(inputs.begin(), inputs.end());
    for (const auto& dependency : planned->imports) {
        if (const auto* observed = findInput(inputs, dependency.path)) {
            if (!observed->contents || *observed->contents != dependency.bytes)
                return fail(ErrorCode::Busy, "An import buffer differs from the evaluated input; save or reload first");
        } else {
            result.dependencies.push_back({dependency.path, dependency.bytes});
        }
    }
    result.replacements.push_back({definition->path, planned->projectBefore, planned->projectAfter});
    if (planned->filtersBefore && planned->filtersBefore != planned->filtersAfter)
        result.replacements.push_back({filters->path, *planned->filtersBefore, *planned->filtersAfter});
    return result;
}

} // namespace frost::project
