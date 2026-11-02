// SPDX-License-Identifier: Apache-2.0
//
// ts.hpp: frost::syntax's private access to the tree-sitter C API (ADR 0023): the api.h include,
// the entry points of the seven vendored grammars, and owning pointers for parsers, trees, query
// cursors and tree cursors. Only src/syntax and its tests include it.
#pragma once

#include <tree_sitter/api.h>

#include <memory>

extern "C" {
// Defined by the vendored grammars (third_party/grammars/*/src/parser.c); each returns a static,
// immutable TSLanguage.
const TSLanguage* tree_sitter_c(void);
const TSLanguage* tree_sitter_cpp(void);
const TSLanguage* tree_sitter_c_sharp(void);
const TSLanguage* tree_sitter_cmake(void);
const TSLanguage* tree_sitter_xml(void);
const TSLanguage* tree_sitter_json(void);
const TSLanguage* tree_sitter_markdown(void);
const TSLanguage* tree_sitter_lua(void);
}

namespace frost::syntax::detail {

struct ParserDeleter {
    void operator()(TSParser* parser) const noexcept { ts_parser_delete(parser); }
};
struct TreeDeleter {
    void operator()(TSTree* tree) const noexcept { ts_tree_delete(tree); }
};
struct QueryCursorDeleter {
    void operator()(TSQueryCursor* cursor) const noexcept { ts_query_cursor_delete(cursor); }
};

// Owning pointers; each frees on any thread.
using ParserPtr = std::unique_ptr<TSParser, ParserDeleter>;
using TreePtr = std::unique_ptr<TSTree, TreeDeleter>;
using QueryCursorPtr = std::unique_ptr<TSQueryCursor, QueryCursorDeleter>;

// A TSTreeCursor that deletes itself (ts_tree_cursor_delete) on scope exit.
class TreeCursor {
public:
    explicit TreeCursor(TSNode node) noexcept : m_cursor(ts_tree_cursor_new(node)) {}
    ~TreeCursor() { ts_tree_cursor_delete(&m_cursor); }
    TreeCursor(const TreeCursor&) = delete ("owns a tree-sitter cursor");
    TreeCursor& operator=(const TreeCursor&) = delete ("owns a tree-sitter cursor");
    [[nodiscard]] TSTreeCursor* get() noexcept { return &m_cursor; }

private:
    TSTreeCursor m_cursor;
};

} // namespace frost::syntax::detail
