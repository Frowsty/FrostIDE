# SPDX-License-Identifier: Apache-2.0
#
# CLANG-0 integration tests for the frost-clang helper engine (PLAN.md Appendix C, ADR 0083):
#   1. Spawns frost-clang in clangd LSP mode and verifies that FrostFeatureModule answers custom
#      JSON-RPC requests ("frost/clangInfo" and "frost/spikePing") over Content-Length stdio.
#   2. Spawns frost-clang in LibTooling AST rewrite mode ("--rewrite-symbol") on C++ fixtures,
#      verifies that only C++ AST occurrences of the target symbol are renamed (leaving comments,
#      string literals and longer identifiers untouched), and compiles the rewritten source with
#      the pinned Clang compiler.

frost_add_test(clang_spike_tests
    SOURCES
        clang_spike_test.cpp
    DEPS
        frost::rpc
        frost::io
        frost::core
    LABELS integration)
target_include_directories(clang_spike_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(clang_spike_tests PRIVATE
    FROST_CLANG_BINARY="$<TARGET_FILE:frost-clang>"
    FROST_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}")
add_dependencies(clang_spike_tests frost-clang)
