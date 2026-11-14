// SPDX-License-Identifier: Apache-2.0
// Project actual named Lua AST nodes and their real grammar fields with finite ownership limits.
#include "frost/syntax/syntax_tree.hpp"

#include "language_state.hpp"
#include "parse.hpp"
#include "tree_state.hpp"

#include <utility>
namespace frost::syntax {
Result<LuaAstProjection> projectLuaSnapshot(text::Snapshot snapshot, const LuaAstLimits& limits) {
    if (snapshot.size().value() > limits.maxBytes || limits.maxBytes > 8 * 1024 * 1024)
        return fail(ErrorCode::OutOfRange, "Lua source exceeds the AST byte budget");
    if (limits.stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Lua parse cancelled");
    LanguageRegistry registry;
    auto language = registry.get(LanguageId::Lua);
    if (!language)
        return std::unexpected(language.error());
    detail::ParserPtr parser(ts_parser_new());
    if (!parser || !ts_parser_set_language(parser.get(), (*language)->grammar))
        return fail(ErrorCode::Unsupported, "Pinned Lua grammar is unavailable");
    detail::ParseJob job(std::move(parser), snapshot, nullptr);
    const auto deadline = limits.budget > IClock::Duration::zero()
                              ? std::optional(systemClock().steadyNow() + limits.budget)
                              : std::nullopt;
    auto parsed = detail::runParse(job, systemClock(), deadline, &limits.stop, nullptr);
    if (parsed.status == detail::ParseStatus::Stopped)
        return fail(ErrorCode::Cancelled, "Lua parse cancelled");
    if (parsed.status == detail::ParseStatus::Deadline)
        return fail(ErrorCode::Timeout, "Lua parse budget exceeded");
    if (parsed.status != detail::ParseStatus::Done)
        return fail(ErrorCode::Corrupt, "Lua parse failed");
    auto tree = detail::makeTree(std::move(parsed.tree), *language, snapshot, snapshot.version());
    return tree.luaAstProjection(limits);
}
Result<LuaAstProjection> SyntaxTree::luaAstProjection(const LuaAstLimits& limits) const {
    if (empty() || language() != LanguageId::Lua)
        return fail(ErrorCode::Unsupported, "A real Lua syntax tree is required");
    if (stale())
        return fail(ErrorCode::Busy, "Wait for the current Lua parse before refactoring");
    if (hasError())
        return fail(ErrorCode::Unsupported, "Fix Lua syntax errors before an AST refactoring");
    if (!limits.maxNodes || limits.maxNodes > 200000 || !limits.maxDepth || limits.maxDepth > 256 ||
        !limits.maxVisits || limits.maxVisits > 1000000 || limits.maxBytes > 8 * 1024 * 1024 ||
        text().size().value() > limits.maxBytes)
        return fail(ErrorCode::OutOfRange, "Lua AST projection limit exceeded");
    if (limits.stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Lua AST projection cancelled");
    const auto started = systemClock().steadyNow();
    LuaAstProjection result;
    result.snapshot = text();
    const auto root = ts_tree_root_node(m_state->tree.get());
    const auto range = [](TSNode node) {
        return text::ByteRange{ByteOffset{ts_node_start_byte(node)}, ByteOffset{ts_node_end_byte(node)}};
    };
    result.nodes.push_back({ts_node_type(root), {}, range(root), kNoLuaNode, {}});
    std::vector<std::pair<TSNode, std::uint32_t>> pending{{root, 0}};
    std::uint32_t visits = 0;
    for (std::size_t index = 0; index < pending.size(); ++index) {
        if (limits.stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Lua AST projection cancelled");
        if (limits.budget > IClock::Duration::zero() && systemClock().steadyNow() - started > limits.budget)
            return fail(ErrorCode::Timeout, "Lua AST projection budget exceeded");
        const auto [node, depth] = pending[index];
        const auto count = ts_node_child_count(node);
        for (std::uint32_t childIndex = 0; childIndex < count; ++childIndex) {
            if (++visits > limits.maxVisits)
                return fail(ErrorCode::OutOfRange, "Lua AST visit limit exceeded");
            if ((visits & 255) == 0 && limits.stop.stop_requested())
                return fail(ErrorCode::Cancelled, "Lua AST projection cancelled");
            if ((visits & 255) == 0 && limits.budget > IClock::Duration::zero() &&
                systemClock().steadyNow() - started > limits.budget)
                return fail(ErrorCode::Timeout, "Lua AST projection budget exceeded");
            const auto child = ts_node_child(node, childIndex);
            const auto* field = ts_node_field_name_for_child(node, childIndex);
            if (!ts_node_is_named(child)) {
                result.nodes[index].tokens.push_back({ts_node_type(child), field ? field : "", range(child)});
                continue;
            }
            if (depth >= limits.maxDepth || pending.size() >= limits.maxNodes)
                return fail(ErrorCode::OutOfRange, "Lua AST depth/node limit exceeded");
            const auto id = static_cast<std::uint32_t>(pending.size());
            result.nodes.push_back(
                {ts_node_type(child), field ? field : "", range(child), static_cast<std::uint32_t>(index), {}});
            result.nodes[index].children.push_back(id);
            pending.emplace_back(child, depth + 1);
        }
    }
    if (limits.stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Lua AST projection cancelled");
    if (limits.budget > IClock::Duration::zero() && systemClock().steadyNow() - started > limits.budget)
        return fail(ErrorCode::Timeout, "Lua AST projection budget exceeded");
    return result;
}
} // namespace frost::syntax
