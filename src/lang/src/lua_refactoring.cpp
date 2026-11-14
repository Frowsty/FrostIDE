// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/lua_refactoring.hpp"

#include <algorithm>
#include <map>
#include <set>
namespace frost::lang {
namespace {
using syntax::kNoLuaNode;
using syntax::LuaAstProjection;
struct Binding {
    std::uint32_t node, statement, scope;
    std::uint64_t visible;
    std::vector<std::uint32_t> reads, writes;
};
struct Model {
    LuaAstProjection ast;
    std::vector<std::uint32_t> scopes;
    std::vector<Binding> bindings;
    std::map<std::pair<std::uint32_t, std::string>, std::vector<std::uint32_t>> names;
    std::set<std::uint32_t> declarations;
    std::map<std::uint32_t, std::size_t> declarationCounts;
    std::string text(std::uint32_t id) const { return ast.snapshot.text(ast.nodes[id].range); }
    bool inside(std::uint32_t id, std::uint32_t ancestor) const {
        while (id != kNoLuaNode) {
            if (id == ancestor)
                return true;
            id = ast.nodes[id].parent;
        }
        return false;
    }
    std::uint32_t nearest(std::uint32_t id, std::string_view kind) const {
        while (id != kNoLuaNode) {
            if (ast.nodes[id].kind == kind)
                return id;
            id = ast.nodes[id].parent;
        }
        return kNoLuaNode;
    }
};
bool token(const syntax::LuaAstNode& node, std::string_view value) {
    return std::ranges::any_of(node.tokens, [&](const auto& t) { return t.kind == value; });
}
bool identifier(std::string_view name) {
    if (name.empty() || name.size() > 128 ||
        !((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z') || name[0] == '_'))
        return false;
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    constexpr std::string_view words = " and break do else elseif end false for function goto if in local nil not or "
                                       "repeat return then true until while ";
    return words.find(" " + std::string(name) + " ") == words.npos;
}
Result<Model> model(const syntax::SyntaxTree& tree, std::stop_token stop) {
    syntax::LuaAstLimits limits;
    limits.stop = stop;
    auto projected = tree.luaAstProjection(limits);
    if (!projected)
        return std::unexpected(projected.error());
    Model m;
    m.ast = std::move(*projected);
    const auto& nodes = m.ast.nodes;
    m.scopes.resize(nodes.size());
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Lua refactoring cancelled");
        const auto& n = nodes[i];
        m.scopes[i] = (n.kind == "chunk" || n.kind == "block") ? i : m.scopes[n.parent];
        // These require closure/loop/extended-dialect binding rules beyond this profile.
        if (n.kind == "function_declaration" || n.kind == "function_definition" || n.kind == "for_statement" ||
            n.kind == "repeat_statement" || n.kind == "goto_statement" || n.kind == "label_statement" ||
            n.kind == "vararg_expression" || n.field == "attribute" || token(n, "global"))
            return fail(ErrorCode::Unsupported,
                        "Lua refactoring currently requires a closure-free, non-loop-binding source profile");
        if (n.kind == "identifier") {
            const auto name = m.text(i);
            if (name == "_ENV" || name == "_G" || name == "getfenv" || name == "setfenv" || name == "debug" ||
                name == "load" || name == "loadfile" || name == "dofile")
                return fail(ErrorCode::Unsupported,
                            "Dynamic Lua environment/introspection cannot be refactored safely");
        }
    }
    // Actual variable_list nodes, not textual 'local' matches. Initializers see the outer binding.
    for (std::uint32_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].kind == "identifier") {
            if (stop.stop_requested())
                return fail(ErrorCode::Cancelled, "Lua refactoring cancelled");
            const auto list = nodes[i].parent;
            if (list == kNoLuaNode || nodes[list].kind != "variable_list")
                continue;
            const auto statement = m.nearest(list, "variable_declaration");
            if (statement == kNoLuaNode)
                continue;
            bool local = false;
            for (auto p = list; p != kNoLuaNode; p = nodes[p].parent) {
                local |= token(nodes[p], "local");
                if (p == statement)
                    break;
            }
            if (!local)
                return fail(ErrorCode::Unsupported, "Only local Lua declarations can be refactored");
            const auto binding = static_cast<std::uint32_t>(m.bindings.size());
            m.bindings.push_back({i, statement, m.scopes[statement], nodes[statement].range.end.value(), {}, {}});
            ++m.declarationCounts[statement];
            m.names[{m.scopes[statement], m.text(i)}].push_back(binding);
            m.declarations.insert(i);
        }
    for (auto& [key, ids] : m.names)
        std::ranges::sort(ids, [&](auto a, auto b) { return m.bindings[a].visible < m.bindings[b].visible; });
    for (std::uint32_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].kind == "identifier" && !m.declarations.contains(i)) {
            if (stop.stop_requested())
                return fail(ErrorCode::Cancelled, "Lua refactoring cancelled");
            const auto& n = nodes[i];
            const auto parent = n.parent;
            if (parent == kNoLuaNode)
                continue;
            if ((nodes[parent].kind == "dot_index_expression" && n.field == "field") ||
                (nodes[parent].kind == "method_index_expression" && n.field == "method"))
                continue;
            // A named table key is not a lexical variable; computed bracket keys are.
            if (nodes[parent].kind == "field" && n.field == "name" && !token(nodes[parent], "["))
                continue;
            auto scope = m.scopes[i];
            for (;;) {
                const auto found = m.names.find({scope, m.text(i)});
                if (found != m.names.end()) {
                    const auto& ids = found->second;
                    auto at = std::upper_bound(ids.begin(), ids.end(), n.range.begin.value(),
                                               [&](auto offset, auto id) { return offset < m.bindings[id].visible; });
                    if (at != ids.begin()) {
                        auto& binding = m.bindings[*std::prev(at)];
                        const bool write = nodes[parent].kind == "variable_list" &&
                                           m.nearest(parent, "assignment_statement") != kNoLuaNode;
                        (write ? binding.writes : binding.reads).push_back(i);
                        break;
                    }
                }
                if (scope == 0)
                    break;
                scope = m.scopes[nodes[scope].parent];
            }
        }
    return m;
}
std::vector<std::uint32_t> descendants(const Model& m, std::uint32_t root, std::string_view kind) {
    std::vector<std::uint32_t> result, pending{root};
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (m.ast.nodes[id].kind == kind)
            result.push_back(id);
        for (auto child : m.ast.nodes[id].children)
            pending.push_back(child);
    }
    return result;
}
Result<void> singleValueDeclaration(const Model& m, std::uint32_t statement) {
    const auto count = m.declarationCounts.find(statement);
    const auto names = count == m.declarationCounts.end() ? 0 : count->second;
    auto values = descendants(m, statement, "expression_list");
    if (names != 1 || values.size() != 1 || m.ast.nodes[values[0]].children.size() != 1)
        return fail(ErrorCode::Unsupported,
                    "Select a single initialized local; multi-value adjustment is not supported");
    return {};
}
} // namespace
Result<LuaRefactoringProposal> planLuaInlineVariable(const syntax::SyntaxTree& tree, ByteOffset selected,
                                                     std::stop_token stop) {
    auto built = model(tree, stop);
    if (!built)
        return std::unexpected(built.error());
    auto& m = *built;
    for (const auto& b : m.bindings) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Lua refactoring cancelled");
        auto contains = [&](auto id) {
            const auto r = m.ast.nodes[id].range;
            return selected >= r.begin && selected <= r.end;
        };
        if (!contains(b.node) && !std::ranges::any_of(b.reads, contains))
            continue;
        if (!b.writes.empty() || b.reads.empty())
            return fail(ErrorCode::Unsupported, "Inline requires an unchanged local with actual read references");
        if (auto valid = singleValueDeclaration(m, b.statement); !valid)
            return std::unexpected(valid.error());
        const auto values = descendants(m, b.statement, "expression_list");
        const auto value = m.ast.nodes[values[0]].children[0];
        const auto& kind = m.ast.nodes[value].kind;
        if (kind != "number" && kind != "string" && kind != "nil" && kind != "true" && kind != "false")
            return fail(ErrorCode::Unsupported,
                        "Inline currently accepts immutable scalar literals, not calls/allocations/aliases");
        if (!descendants(m, b.statement, "comment").empty())
            return fail(ErrorCode::Unsupported, "Move declaration comments explicitly before inlining");
        LuaRefactoringProposal plan{m.ast.snapshot, "Inline Lua local", {}};
        plan.edits.push_back({m.ast.nodes[b.statement].range, {}});
        const auto replacement = "(" + m.text(value) + ")";
        if (b.reads.size() > 100000 || replacement.size() > 8 * 1024 * 1024 / (b.reads.size() + 1))
            return fail(ErrorCode::OutOfRange, "Lua inline proposal exceeds its edit/output budget");
        for (auto read : b.reads) {
            if (stop.stop_requested())
                return fail(ErrorCode::Cancelled, "Lua refactoring cancelled");
            plan.edits.push_back({m.ast.nodes[read].range, replacement});
        }
        std::ranges::sort(plan.edits, {}, [](const auto& e) { return e.range.begin; });
        return plan;
    }
    return fail(ErrorCode::NotFound, "Select a local declaration or its resolved reference");
}
Result<LuaRefactoringProposal> planLuaExtractFunction(const syntax::SyntaxTree& tree, text::ByteRange selected,
                                                      std::string name, std::stop_token stop) {
    if (!identifier(name))
        return fail(ErrorCode::InvalidArgument, "Use a fresh Lua function identifier");
    auto built = model(tree, stop);
    if (!built)
        return std::unexpected(built.error());
    auto& m = *built;
    const auto& nodes = m.ast.nodes;
    if (selected.begin >= selected.end || selected.end.value() > m.ast.snapshot.size().value())
        return fail(ErrorCode::OutOfRange, "Select complete Lua statements");
    for (std::uint32_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].kind == "identifier" && m.text(i) == name)
            return fail(ErrorCode::AlreadyExists,
                        "The extracted function name occurs in this source; choose a fresh name");
    std::vector<std::uint32_t> statements;
    for (std::uint32_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].kind == "chunk" || nodes[i].kind == "block") {
            if (stop.stop_requested())
                return fail(ErrorCode::Cancelled, "Lua refactoring cancelled");
            std::vector<std::uint32_t> matching;
            for (auto child : nodes[i].children)
                if (nodes[child].range.begin >= selected.begin && nodes[child].range.end <= selected.end)
                    matching.push_back(child);
            if (!matching.empty() && nodes[matching.front()].range.begin == selected.begin &&
                nodes[matching.back()].range.end == selected.end) {
                statements = std::move(matching);
                break;
            }
        }
    if (statements.empty())
        return fail(ErrorCode::Unsupported, "Selection must exactly cover complete statements in one block");
    for (std::uint32_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].range.begin >= selected.begin && nodes[i].range.end <= selected.end) {
            if (stop.stop_requested())
                return fail(ErrorCode::Cancelled, "Lua refactoring cancelled");
            const auto& kind = nodes[i].kind;
            if (kind == "return_statement" || kind == "break_statement" || kind == "if_statement" ||
                kind == "while_statement" || kind == "repeat_statement" || kind == "do_statement")
                return fail(ErrorCode::Unsupported,
                            "Extract supports straight-line statements without control transfers");
        }
    std::vector<std::string> outputs;
    for (const auto& b : m.bindings)
        if (nodes[b.statement].range.begin >= selected.begin && nodes[b.statement].range.end <= selected.end) {
            if (stop.stop_requested())
                return fail(ErrorCode::Cancelled, "Lua refactoring cancelled");
            if (auto valid = singleValueDeclaration(m, b.statement); !valid)
                return std::unexpected(valid.error());
            if (std::ranges::any_of(b.reads, [&](auto id) { return nodes[id].range.begin >= selected.end; }) ||
                std::ranges::any_of(b.writes, [&](auto id) { return nodes[id].range.begin >= selected.end; }))
                outputs.push_back(m.text(b.node));
        }
    const auto source = m.ast.snapshot.text();
    const auto begin = selected.begin.value();
    const auto line = source.rfind('\n', begin ? begin - 1 : 0);
    const auto lineStart = line == source.npos ? 0 : line + 1;
    std::string indent = source.substr(lineStart, begin - lineStart);
    if (indent.find_first_not_of(" \t") != indent.npos)
        return fail(ErrorCode::Unsupported, "Start the extraction on its own statement line");
    const std::string newline = source.find("\r\n") != source.npos ? "\r\n" : "\n";
    auto body = m.ast.snapshot.text(selected);
    std::string nested;
    for (std::size_t p = 0; p < body.size();) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Lua refactoring cancelled");
        const auto end = body.find('\n', p);
        const auto length = end == body.npos ? body.size() - p : end - p;
        if (indent.size() + 4 + length + 1 > 8 * 1024 * 1024 - nested.size())
            return fail(ErrorCode::OutOfRange, "Extracted Lua body exceeds its output budget");
        nested += indent;
        nested += "    ";
        nested.append(body, p, length);
        if (end == body.npos)
            break;
        nested += '\n';
        p = end + 1;
        if (body.substr(p, indent.size()) == indent)
            p += indent.size();
    }
    std::string joined;
    for (const auto& output : outputs) {
        if (!joined.empty())
            joined += ", ";
        joined += output;
    }
    if (nested.size() + joined.size() * 2 + indent.size() * 4 + name.size() * 2 + 128 > 8 * 1024 * 1024)
        return fail(ErrorCode::OutOfRange, "Extracted Lua proposal exceeds its output budget");
    std::string replacement = "local function " + name + "()" + newline + nested + newline;
    if (!joined.empty())
        replacement += indent + "    return " + joined + newline;
    replacement += indent + "end" + newline + indent;
    if (!joined.empty())
        replacement += "local " + joined + " = ";
    replacement += name + "()";
    return LuaRefactoringProposal{m.ast.snapshot, "Extract Lua function", {{selected, std::move(replacement)}}};
}
} // namespace frost::lang
