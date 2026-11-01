# SPDX-License-Identifier: Apache-2.0
#
# frost::syntax (PLAN.md section 2 "Syntax", section 3.2; ADR 0023): the tree-sitter host. The
# vendored tree-sitter 0.27.1 runtime and seven grammars (third_party/, pinned in
# third_party/vendor.lock.json), our own highlight and outline queries (queries/, embedded at
# configure time), incremental parsing from frost::text snapshots with a 2 ms synchronous budget
# and a frost::sched worker fallback, and the 50 MB cut-off. Qt-free; no tree-sitter header in the
# public headers. Not linked into frost_app yet: m1/editor-view does that.
# TODO(M4): Markdown inline and other injections; CMake language service on the CMake grammar.

include(FrostEmbed)

set(_frost_ts_root "${FROST_SOURCE_DIR}/third_party/tree-sitter")
set(_frost_grammars "${FROST_SOURCE_DIR}/third_party/grammars")

# The runtime as one C translation unit with our Unicode decoders (ADR 0023 D2). Deliberately not
# frost_-prefixed: it is a third-party library for FrostLayers' THIRD_PARTY check.
add_library(tree_sitter_runtime STATIC src/c/frost_tree_sitter.c)
frost_apply_c_options(tree_sitter_runtime OWN)
target_include_directories(tree_sitter_runtime PRIVATE "${_frost_ts_root}/lib/src" src/c)
target_include_directories(tree_sitter_runtime SYSTEM PUBLIC "${_frost_ts_root}/lib/include")
target_compile_definitions(tree_sitter_runtime PRIVATE TREE_SITTER_HIDE_SYMBOLS)

# One static library per grammar, from upstream bytes only. Each grammar finds its own
# tree_sitter/parser.h next to its sources (the ABI 14 and 15 headers differ). Scanners allocate
# through the runtime's allocator hooks (TREE_SITTER_REUSE_ALLOCATOR), so ts_set_allocator covers
# them too.
function(_frost_tree_sitter_grammar id dir)
    set(sources "")
    foreach(source IN LISTS ARGN)
        list(APPEND sources "${dir}/${source}")
    endforeach()
    add_library(tree_sitter_${id} STATIC ${sources})
    frost_apply_c_options(tree_sitter_${id} VENDORED)
    target_compile_definitions(tree_sitter_${id} PRIVATE TREE_SITTER_HIDE_SYMBOLS TREE_SITTER_REUSE_ALLOCATOR)
    # The grammar's own tree_sitter/parser.h. Shared headers outside src (tree-sitter-xml's
    # common/scanner.h) include it by that name, which clang on Linux resolves only through an
    # include path (clang-cl also searches the directories of the including files).
    target_include_directories(tree_sitter_${id} PRIVATE "${dir}")
    target_link_libraries(tree_sitter_${id} PUBLIC tree_sitter_runtime)
endfunction()

_frost_tree_sitter_grammar(c "${_frost_grammars}/c/src" parser.c)
_frost_tree_sitter_grammar(cpp "${_frost_grammars}/cpp/src" parser.c scanner.c)
_frost_tree_sitter_grammar(c_sharp "${_frost_grammars}/c-sharp/src" parser.c scanner.c)
_frost_tree_sitter_grammar(cmake "${_frost_grammars}/cmake/src" parser.c scanner.c)
_frost_tree_sitter_grammar(xml "${_frost_grammars}/xml/xml/src" parser.c scanner.c)
_frost_tree_sitter_grammar(json "${_frost_grammars}/json/src" parser.c)
_frost_tree_sitter_grammar(markdown "${_frost_grammars}/markdown/tree-sitter-markdown/src" parser.c scanner.c)

frost_add_library(syntax LAYER syntax
    SOURCES
        src/input.cpp
        src/language.cpp
        src/parse.cpp
        src/queries.cpp
        src/syntax_session.cpp
        src/syntax_tree.cpp
        src/vocabulary.cpp
    DEPS frost::core frost::sched frost::text
    THIRD_PARTY
        tree_sitter_runtime
        tree_sitter_c tree_sitter_cpp tree_sitter_c_sharp tree_sitter_cmake
        tree_sitter_xml tree_sitter_json tree_sitter_markdown)
target_include_directories(frost_syntax PRIVATE src)

set(_frost_syntax_queries "")
foreach(language IN ITEMS c cpp csharp cmake xml json markdown)
    list(APPEND _frost_syntax_queries queries/${language}/highlights.scm queries/${language}/outline.scm)
endforeach()
frost_embed_files(frost_syntax FILES ${_frost_syntax_queries})
