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
#                      semantic tokens and inactive regions (ADR 0047); clangd's tweak and
#                      include-cleaner actions by name (FND-3); label "unit";
#   lang_clangd_tests  the real clangd (the pinned LLVM's) end to end on tiny fixtures: a CMake-style
#                      compile_commands.json, the synthesized one for a .vcxproj, a restart after a
#                      killed clangd, semantic tokens and an inactive region (ADR 0047), the
#                      refactoring tweaks and include cleaner with compiling results (FND-3), and
#                      LSP 3.17 positionEncodings negotiation with columns on a non-ASCII line
#                      (FND-9a); label
#                      "integration" (it starts real clangd processes, so RESOURCE_LOCK frost_desktop;
#                      ADR 0027) with the tag "clangd" (ctest -L clangd).
#                      Each test takes a second or two;
#   lang_msbuild_tests the MSBuild-evaluated provider with the real MSBuild on fixtures/msbuild_props
#                      (include directory and define only from a .props sheet and an environment
#                      variable), its cache, and a real clangd on the evaluated commands; label
#                      "integration" with the tags "msbuild" and "clangd", Windows only. A few seconds
#                      per test. With them SLN-5: clangd on GetClCommandLines from the shipped
#                      ProjectHost (tests/project/fixtures/msvc_generated) against MSBuild.exe.
# The fixtures under fixtures/ are byte-exact inputs (REUSE.toml covers them).

frost_add_test(lang_unit_tests
    SOURCES
        cpp_rename_plan_test.cpp
        cpp_refactoring_plan_test.cpp
        clangd_actions_test.cpp
        cmake_compile_commands_unit_test.cpp
        cmake_language_test.cpp
        compile_commands_test.cpp
        completion_test.cpp
        clangd_locator_test.cpp
        diagnostics_test.cpp
        document_features_test.cpp
        document_sync_test.cpp
        engine_automation_test.cpp
        engine_discovery_process_test.cpp
        features_test.cpp
        fuzzy_snippet_test.cpp
        templates_test.cpp
        json_schema_test.cpp
        json_test.cpp
        jsonrpc_test.cpp
        lsp_client_test.cpp
        lsp_workspace_test.cpp
        frost_engine_assets_test.cpp
        frost_engine_lua_test.cpp
        lua_host_test.cpp
        lua_rc_test.cpp
        lua_service_test.cpp
        msbuild_provider_test.cpp
        semantic_tokens_test.cpp
        uri_test.cpp
        workspace_edit_test.cpp
    DEPS frost::lang frost::project frost::text frost::io frost::sched)
target_include_directories(lang_unit_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(lang_unit_tests PRIVATE FROST_LANG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    FROST_LANG_SOURCE_DIR="${FROST_SOURCE_DIR}/src/lang")
add_executable(engine_discovery_stall_child engine_discovery_stall_child.cpp)
frost_apply_compile_options(engine_discovery_stall_child TEST)
target_compile_definitions(lang_unit_tests PRIVATE FROST_DISCOVERY_STALL_CHILD="$<TARGET_FILE:engine_discovery_stall_child>")
add_dependencies(lang_unit_tests engine_discovery_stall_child)

frost_add_test(lang_clangd_tests
    SOURCES
        clangd_test.cpp
        clangd_encoding_test.cpp
        clangd_features_test.cpp
        navigation_test.cpp
        refactoring_test.cpp
    DEPS frost::lang frost::project frost::text frost::io frost::sched
    LABELS integration clangd)
target_include_directories(lang_clangd_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(lang_clangd_tests PRIVATE FROST_LANG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# CPP-10 exercises the pinned clang-tidy process as well as inspection profiles and SARIF.
if(WIN32 AND NOT Python3_EXECUTABLE AND DEFINED ENV{FROST_SDK} AND EXISTS "$ENV{FROST_SDK}/py/Scripts/python.exe")
    set(Python3_EXECUTABLE "$ENV{FROST_SDK}/py/Scripts/python.exe")
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" _frost_lang_python)
frost_add_test(lang_inspection_tests
    SOURCES inspection_test.cpp
    DEPS frost::lang frost::project frost::text frost::io frost::sched
    LABELS integration)
target_include_directories(lang_inspection_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(lang_inspection_tests PRIVATE
    FROST_TEST_PYTHON="${_frost_lang_python}"
    FROST_VALIDATE_SARIF="${FROST_SOURCE_DIR}/tools/validate_sarif.py")

frost_add_test(lang_code_vision_real_tests
    SOURCES code_vision_real_test.cpp
    DEPS frost::lang frost::text frost::io frost::sched
    LABELS integration clangd)
target_include_directories(lang_code_vision_real_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

if(WIN32)
    frost_add_test(lang_msbuild_tests
        SOURCES
            cl_command_lines_clangd_test.cpp
            msbuild_eval_test.cpp
        DEPS frost::lang frost::build frost::project frost::text frost::io frost::sched
        LABELS integration msbuild clangd)
    target_include_directories(lang_msbuild_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
    target_compile_definitions(lang_msbuild_tests PRIVATE
        FROST_LANG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        FROST_PROJECT_FIXTURE_DIR="${FROST_SOURCE_DIR}/tests/project/fixtures"
        FROST_PROJECT_HOST="${CMAKE_BINARY_DIR}/src/app/$<CONFIG>/project-host/ProjectHostVsProbe.exe")
    add_dependencies(lang_msbuild_tests frost_project_host)
endif()

# CMK-4 and CPP-3 with the real cmake, Ninja (and on Windows the Visual Studio 2022 generator) on
# fixtures/cmake_contexts, checked with the real clangd (`clangd --check`): the codemodel's commands
# against CMake's own database, the owned directory and the resolve context. Label "integration"
# with the tags "cmake" and "clangd". About a minute (two configures and a dozen clangd checks).
frost_add_test(lang_cmake_tests
    SOURCES cmake_compile_commands_test.cpp
    DEPS frost::lang frost::build frost::project frost::text frost::io frost::sched
    LABELS integration cmake clangd)
target_include_directories(lang_cmake_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(lang_cmake_tests PRIVATE
    FROST_LANG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    FROST_TEST_CMAKE="${CMAKE_COMMAND}"
    FROST_TEST_NINJA="${CMAKE_MAKE_PROGRAM}"
    FROST_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}")

frost_add_test(lang_lua_tests
    SOURCES lua_e2e_test.cpp lua_workspace_e2e_test.cpp
    DEPS frost::lang frost::project frost::text frost::io frost::sched
    LABELS integration)
target_include_directories(lang_lua_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

# Real pinned Roslyn-backed managed server from configured, offline-verified fixture tools.
include("${FROST_SOURCE_DIR}/tests/support/ManagedFixtures.cmake")
frost_add_managed_fixture_test(lang_managed_tests
    SOURCES managed_e2e_test.cpp
    DEPS frost::lang frost::project frost::text frost::io frost::sched
    LABELS integration)
target_include_directories(lang_managed_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

if(WIN32)
    target_link_libraries(lang_managed_tests PRIVATE shell32 ole32 uuid)
endif()

# CPP-4 and FND-18 corpus tier (docs/adr/0084-cpp4-indexing-at-scale-baseline.md): full clangd
# background index on the pinned FrostEngine clone alongside a concurrent cmake --build under the
# ResourceGovernor build lock and RSS caps.
set(_frost_lang_corpora_lock_file "${FROST_SOURCE_DIR}/tests/corpus/corpora.lock.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_frost_lang_corpora_lock_file}")
file(READ "${_frost_lang_corpora_lock_file}" _frost_lang_corpora_lock)
string(JSON _frost_lang_corpora_count LENGTH "${_frost_lang_corpora_lock}" corpora)
set(_frost_lang_engine_sha "")
math(EXPR _frost_lang_corpora_last "${_frost_lang_corpora_count} - 1")
foreach(_frost_lang_index RANGE ${_frost_lang_corpora_last})
    string(JSON _frost_lang_corpus_id GET "${_frost_lang_corpora_lock}" corpora ${_frost_lang_index} id)
    if(_frost_lang_corpus_id STREQUAL "frostengine")
        string(JSON _frost_lang_engine_sha GET "${_frost_lang_corpora_lock}" corpora ${_frost_lang_index} sha)
        if(WIN32)
            string(JSON _frost_lang_engine_preset GET "${_frost_lang_corpora_lock}" corpora ${_frost_lang_index} presets windows)
        else()
            string(JSON _frost_lang_engine_preset GET "${_frost_lang_corpora_lock}" corpora ${_frost_lang_index} presets linux)
        endif()
    endif()
endforeach()
if(NOT _frost_lang_engine_sha MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "${_frost_lang_corpora_lock_file} pins no frostengine corpus")
endif()
frost_add_test(lang_index_corpus_tests
    SOURCES clangd_index_corpus_test.cpp
    DEPS frost::lang frost::project frost::text frost::io frost::sched
    LABELS integration corpus)
target_include_directories(lang_index_corpus_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(lang_index_corpus_tests PRIVATE
    FROST_CORPUS_FROSTENGINE_SHA="${_frost_lang_engine_sha}"
    FROST_CORPUS_FROSTENGINE_PRESET="${_frost_lang_engine_preset}"
    FROST_TEST_CMAKE="${CMAKE_COMMAND}")

# CPP-6 oracle comparisons run only in the mandatory corpus tier, never by textual inclusion
# into a UI test. Missing pinned input is a failure in these tests.
frost_add_test(lang_cpp6_corpus_tests
    SOURCES cpp6_navigation_corpus_test.cpp
    DEPS frost::lang frost::project frost::text frost::io frost::sched
    LABELS integration corpus)
target_include_directories(lang_cpp6_corpus_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(lang_cpp6_corpus_tests PRIVATE
    FROST_CORPUS_FROSTENGINE_SHA="${_frost_lang_engine_sha}"
    FROST_CORPUS_FROSTENGINE_PRESET="${_frost_lang_engine_preset}")

frost_add_test(lang_code_vision_corpus_tests
    SOURCES code_vision_corpus_test.cpp
    DEPS frost::lang frost::text frost::io frost::sched
    LABELS integration corpus clangd)
target_include_directories(lang_code_vision_corpus_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(lang_code_vision_corpus_tests PRIVATE
    FROST_CORPUS_FROSTENGINE_SHA="${_frost_lang_engine_sha}"
    FROST_CORPUS_FROSTENGINE_PRESET="${_frost_lang_engine_preset}")

frost_add_test(lang_inspection_corpus_tests
    SOURCES inspection_corpus_test.cpp
    DEPS frost::lang frost::project frost::text frost::io frost::sched frost::vcs
    LABELS integration corpus)
target_include_directories(lang_inspection_corpus_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(lang_inspection_corpus_tests PRIVATE
    FROST_CORPUS_FROSTENGINE_SHA="${_frost_lang_engine_sha}"
    FROST_CORPUS_FROSTENGINE_PRESET="${_frost_lang_engine_preset}"
    FROST_TEST_PYTHON="${_frost_lang_python}"
    FROST_VALIDATE_SARIF="${FROST_SOURCE_DIR}/tools/validate_sarif.py")
