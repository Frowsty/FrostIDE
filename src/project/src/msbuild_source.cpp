// SPDX-License-Identifier: Apache-2.0
// Pugixml-backed property/target/import source locations for SLN-18.
#include "frost/project/msbuild_source.hpp"

#include "xml.hpp"

#include <algorithm>
#include <functional>
namespace frost::project {
bool isMSBuildSource(std::string_view path) {
    return path.ends_with(".props") || path.ends_with(".targets") || path.ends_with(".vcxproj") ||
           path.ends_with(".csproj");
}
Result<MSBuildSource> parseMSBuildSource(std::string_view source, std::string_view path) {
    if (source.size() > 1024U * 1024U)
        return fail(ErrorCode::OutOfRange, "MSBuild source exceeds 1 MiB");
    pugi::xml_document document;
    if (auto parsed = detail::parseXml(document, source, path, "Project"); !parsed)
        return std::unexpected(parsed.error());
    MSBuildSource result;
    const auto declaration = [&](const pugi::xml_node& node, std::string name) {
        const auto offset = static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, node.offset_debug()));
        const auto end = source.begin() + static_cast<std::ptrdiff_t>(std::min(offset, source.size()));
        return MSBuildDeclaration{std::move(name), std::string(detail::elementText(node)),
                                  std::string(detail::attribute(node, "Condition")), offset,
                                  static_cast<std::size_t>(std::count(source.begin(), end, '\n'))};
    };
    std::function<void(pugi::xml_node, const std::string&)> visit;
    visit = [&](pugi::xml_node parent, const std::string& condition) {
        for (auto node : parent.children()) {
            if (node.type() != pugi::node_element)
                continue;
            const auto own = detail::attribute(node, "Condition");
            auto combined = condition;
            if (!own.empty())
                combined += (combined.empty() ? "" : " && ") + std::string(own);
            if (detail::isElement(node, "PropertyGroup")) {
                for (auto property : node.children()) {
                    if (property.type() != pugi::node_element)
                        continue;
                    auto row = declaration(property, std::string(detail::localName(property)));
                    if (!combined.empty())
                        row.condition = combined + (row.condition.empty() ? "" : " && " + row.condition);
                    result.properties.push_back(std::move(row));
                }
            } else if (detail::isElement(node, "Target")) {
                result.targets.push_back(declaration(node, std::string(detail::attribute(node, "Name"))));
            } else if (detail::isElement(node, "Import")) {
                auto value = std::string(detail::attribute(node, "Project"));
                const bool literal = !value.empty() && combined.empty() && value.find_first_of("$@%*?;") == value.npos;
                result.imports.push_back({std::move(value),
                                          static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, node.offset_debug())),
                                          literal});
            } else if (detail::isElement(node, "ImportGroup") || detail::isElement(node, "Choose") ||
                       detail::isElement(node, "When") || detail::isElement(node, "Otherwise")) {
                // Choose branches are opaque without actual MSBuild evaluation.
                if (!detail::isElement(node, "ImportGroup"))
                    combined += " (unevaluated branch)";
                visit(node, combined);
            }
        }
    };
    visit(document.document_element(), {});
    return result;
}
} // namespace frost::project
