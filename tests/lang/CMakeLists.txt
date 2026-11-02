# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::lang (W2, ADR 0036):
#   lang_unit_tests    JSON, framing, JSON-RPC and the LSP client against a scripted in-memory fake
#                      server (malformed, out-of-order, cancelled, crashed mid-request), document sync
#                      from ChangeSets, URIs, the diagnostics model, compile-command discovery and
#                      synthesis from the fixture solution, and finding clangd; label "unit";
#   lang_clangd_tests  the real clangd (the pinned LLVM's) end to end on tiny fixtures: a CMake-style
#                      compile_commands.json, the synthesized one for a .vcxproj, and a restart after a
#                      killed clangd; label "clangd". Each test takes a second or two.
# The fixtures under fixtures/ are byte-exact inputs (REUSE.toml covers them).

frost_add_test(lang_unit_tests
    SOURCES
        compile_commands_test.cpp
        clangd_locator_test.cpp
        diagnostics_test.cpp
        document_sync_test.cpp
        json_test.cpp
        jsonrpc_test.cpp
        lsp_client_test.cpp
        uri_test.cpp
    DEPS frost::lang frost::project frost::text frost::io frost::sched)
target_include_directories(lang_unit_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(lang_unit_tests PRIVATE FROST_LANG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

frost_add_test(lang_clangd_tests
    SOURCES
        clangd_test.cpp
    DEPS frost::lang frost::project frost::text frost::io frost::sched
    LABELS clangd)
target_include_directories(lang_clangd_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(lang_clangd_tests PRIVATE FROST_LANG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
