# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::lang (W2, ADR 0036):
#   lang_unit_tests    JSON, framing, JSON-RPC and the LSP client against a scripted in-memory fake
#                      server (malformed, out-of-order, cancelled, crashed mid-request), document sync
#                      from ChangeSets, URIs, the diagnostics model, compile-command discovery and
#                      synthesis from the fixture solution, and finding clangd; the W2 navigation and
#                      completion models (ADR 0041: completion, fuzzy matching, snippets, signature
#                      help, hover, go to, usages, symbols, rename, quick fixes, formatting) against the
#                      fake server, workspace edits, and the MSBuild provider with a scripted launcher;
#                      semantic tokens and inactive regions (ADR 0047); label "unit";
#   lang_clangd_tests  the real clangd (the pinned LLVM's) end to end on tiny fixtures: a CMake-style
#                      compile_commands.json, the synthesized one for a .vcxproj, a restart after a
#                      killed clangd, and semantic tokens and an inactive region (ADR 0047); label
#                      "integration" (it starts real clangd processes, so RESOURCE_LOCK frost_desktop;
#                      ADR 0027) with the tag "clangd" (ctest -L clangd).
#                      Each test takes a second or two;
#   lang_msbuild_tests the MSBuild-evaluated provider with the real MSBuild on fixtures/msbuild_props
#                      (include directory and define only from a .props sheet and an environment
#                      variable), its cache, and a real clangd on the evaluated commands; label
#                      "integration" with the tags "msbuild" and "clangd", Windows only. A few seconds
#                      per test.
# The fixtures under fixtures/ are byte-exact inputs (REUSE.toml covers them).

frost_add_test(lang_unit_tests
    SOURCES
        compile_commands_test.cpp
        completion_test.cpp
        clangd_locator_test.cpp
        diagnostics_test.cpp
        document_sync_test.cpp
        features_test.cpp
        fuzzy_snippet_test.cpp
        json_test.cpp
        jsonrpc_test.cpp
        lsp_client_test.cpp
        msbuild_provider_test.cpp
        semantic_tokens_test.cpp
        uri_test.cpp
        workspace_edit_test.cpp
    DEPS frost::lang frost::project frost::text frost::io frost::sched)
target_include_directories(lang_unit_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(lang_unit_tests PRIVATE FROST_LANG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

frost_add_test(lang_clangd_tests
    SOURCES
        clangd_test.cpp
    DEPS frost::lang frost::project frost::text frost::io frost::sched
    LABELS integration clangd)
target_include_directories(lang_clangd_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(lang_clangd_tests PRIVATE FROST_LANG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

if(WIN32)
    frost_add_test(lang_msbuild_tests
        SOURCES
            msbuild_eval_test.cpp
        DEPS frost::lang frost::build frost::project frost::text frost::io frost::sched
        LABELS integration msbuild clangd)
    target_include_directories(lang_msbuild_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
    target_compile_definitions(lang_msbuild_tests PRIVATE FROST_LANG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
endif()
