# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::syntax (ADR 0023), label unit: the file-name table, query compilation, our
# Unicode decoders, highlight spans and the outline for a sample of every grammar, enclosing
# ranges, incremental parsing against fresh parses, CRLF and lone-CR rows, the 50 MB cut-off and
# the worker fallback. The white-box tests read the library's private headers (src/syntax/src)
# and the vendored tree-sitter API.
# TODO(M1): the hardening phase adds the property, fuzz, stress, TSan and perf suites of ADR 0023.

frost_add_test(syntax_tests
    SOURCES
        language_test.cpp
        highlight_test.cpp
        session_test.cpp
        lua_ast_test.cpp
        syntax_test_support.cpp
    DEPS frost::syntax frost::sched frost::text
    LABELS unit)
target_include_directories(syntax_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/syntax/src)
