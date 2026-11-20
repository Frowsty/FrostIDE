// SPDX-License-Identifier: Apache-2.0
// C2 tool metadata and conservative wire/result pagination; never invents IDE answers.
#include "frost/agent/protocol.hpp"
namespace frost::agent {
Json mcpToolCatalog() {
    auto tools = Json::array();
    for (const auto* name : {"find_usages", "go_to_definition", "symbol_search", "type_hierarchy", "call_hierarchy",
             "file_problems", "workspace_diagnostics", "inspection_info", "project_model", "build_status", "test_results", "debugger_state",
             "rename_symbol", "apply_quick_fix", "build", "run_tests", "debug_start", "debug_continue", "debug_pause", "debug_stop"}) {
        const std::string_view tool(name);
        const bool mutation = tool == "rename_symbol" || tool == "apply_quick_fix" || tool == "build" || tool == "run_tests" || tool.starts_with("debug_");
        auto properties = Json::object();
        auto required = Json::array();
        const auto field = [&](std::string key, std::string type, bool mandatory) {
            properties.set(key, Json::object().set("type", std::move(type)));
            if (mandatory) required.push(std::move(key));
        };
        if (tool == "find_usages" || tool == "go_to_definition" || tool == "type_hierarchy" || tool == "call_hierarchy" || tool == "rename_symbol" || tool == "apply_quick_fix") {
            field("file", "string", true); field("line", "integer", true); field("character", "integer", true);
        }
        if (tool == "rename_symbol") field("newName", "string", true);
        if (tool == "apply_quick_fix") field("title", "string", true);
        if (tool == "symbol_search") field("query", "string", true);
        if (tool == "file_problems") field("file", "string", true);
        if (tool == "inspection_info") field("id", "string", true);
        field("cursor", "string", false);
        auto metadata = Json::object();
        if (tool == "find_usages" || tool == "go_to_definition" || tool == "file_problems" || tool == "project_model") metadata.set("anthropic/alwaysLoad", true);
        if (tool == "workspace_diagnostics" || tool == "project_model") metadata.set("anthropic/maxResultSizeChars", 500000);
        if (mutation) metadata.set("anthropic/requiresUserInteraction", true);
        auto descriptor = Json::object().set("name", std::string(tool)).set("description", "Real, consent-scoped FrostIDE data; unavailable tools fail explicitly")
            .set("inputSchema", Json::object().set("type", "object").set("properties", std::move(properties)).set("required", std::move(required)).set("additionalProperties", false))
            .set("annotations", Json::object().set("readOnlyHint", !mutation).set("destructiveHint", mutation))
            .set("_meta", std::move(metadata));
        if (mutation) descriptor.set("execution", Json::object().set("taskSupport", "required"));
        tools.push(std::move(descriptor));
    }
    return Json::object().set("tools", std::move(tools));
}
Result<std::vector<Json>> paginate(const Json& result, std::size_t pageBytes) {
    if (pageBytes < 256 || pageBytes > 12000) return fail(ErrorCode::InvalidArgument, "MCP page bound must be 256..12000 bytes");
    if (result.dump().size() <= pageBytes) return std::vector<Json>{result};
    if (!result.isArray()) return fail(ErrorCode::OutOfRange, "large MCP results must be structured arrays for pagination");
    std::vector<Json> pages;
    auto page = Json::array();
    std::size_t size = 2;
    for (const auto& item : result.items()) {
        const auto bytes = item.dump().size();
        if (bytes + 2 > pageBytes) return fail(ErrorCode::OutOfRange, "one MCP record exceeds page size; request narrower data");
        if (size + bytes + 1 > pageBytes) { pages.push_back(std::move(page)); page = Json::array(); size = 2; }
        page.push(item);
        size += bytes + 1;
        if (pages.size() >= 256) return fail(ErrorCode::OutOfRange, "MCP result exceeds bounded page retention; narrow the query");
    }
    if (!page.items().empty()) pages.push_back(std::move(page));
    return pages;
}
}
