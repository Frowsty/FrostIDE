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

# FEC-13 keeps semantic registration extraction read-only and checks identities against real ASTs.
frost_add_test(clang_binding_map_tests
    SOURCES binding_map_test.cpp
    DEPS frost::rpc frost::io frost::core
    LABELS integration)
target_include_directories(clang_binding_map_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(clang_binding_map_tests PRIVATE
    FROST_CLANG_BINARY="$<TARGET_FILE:frost-clang>"
    FROST_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    FROST_BINDING_MAP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/binding-map")
add_dependencies(clang_binding_map_tests frost-clang)

# Read-only REF-4 preparation: separate real-tool tests leave CLANG-0's rewrite spike intact.
frost_add_test(local_rename_plan_tests
    SOURCES local_rename_plan_test.cpp project_rename_plan_test.cpp ast_refactoring_plan_test.cpp
    DEPS frost::rpc frost::io frost::core
    LABELS integration)
target_include_directories(local_rename_plan_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(local_rename_plan_tests PRIVATE
    FROST_CLANG_BINARY="$<TARGET_FILE:frost-clang>"
    FROST_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}")
add_dependencies(local_rename_plan_tests frost-clang)

# REF-10 real LLVM YAML decoding and clang-tidy VFS replacement export (source-only until provisioned).
frost_add_test(clang_inspection_scope_tests
    SOURCES tidy_scope_fixes_test.cpp
    DEPS frost::lang frost::rpc frost::io frost::core
    LABELS integration)
target_include_directories(clang_inspection_scope_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(clang_inspection_scope_tests PRIVATE
    FROST_CLANG_BINARY="$<TARGET_FILE:frost-clang>"
    FROST_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}")
add_dependencies(clang_inspection_scope_tests frost-clang)
