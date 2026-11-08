# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::debug (ADR 0050):
#   debug_unit_tests   the DAP client over the fake-dap double (framing, requests, events, reverse
#                      requests, cancellation, an adapter crash), DebugSession's state machine against
#                      the scripted program, the breakpoint manager and its tracker and persistence
#                      over a MemFs, and lldb-dap's search, process spec, launch arguments and trust
#                      gate through the scripted launcher, label "unit" (every platform);
#   debug_lldb_tests   the pinned lldb-dap itself against fixtures/debuggee.cpp: a breakpoint stop, a
#                      local and the exit, label "integration" with the tag "lldb" (ctest -L lldb).
#                      It fails while lldb-dap is not installed (FND-6);
#   debug_dap_real_tests  the dap-real scenario suite (dap_real_test.cpp, DBG-1 and DBG-2a) against
#                      fixtures/dap_scenarios.cpp, label "integration" with the tag "lldb";
#   debug_corpus_tests (Windows) FrostEditor of the pinned FrostEngine clone under lldb-dap
#                      (corpus_debug_test.cpp), label "integration" with the tags "corpus" and "lldb".

frost_add_test(debug_unit_tests
    SOURCES
        breakpoints_test.cpp
        coreclr_dap_test.cpp
        dap_client_test.cpp
        debug_session_test.cpp
        debug_session_breakpoints_test.cpp
        lldb_dap_test.cpp
        mono_dap_test.cpp
        mono_wire_test.cpp
        mono_session_test.cpp
        mono_responses_test.cpp
        mono_gate_test.cpp
        mono_protocol_session_test.cpp
        mono_transport_test.cpp
        lua_debug_endpoint_test.cpp
    DEPS frost::debug frost::project frost::text frost::io)
target_include_directories(debug_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The debuggee: a plain C++ program without frost code, built without optimisation and with full
# debug information so lldb sees its lines and locals.
add_executable(debug_test_debuggee fixtures/debuggee.cpp)
# This fixture checks source-line binding, so its debug paths must match this worktree.
set_property(TARGET debug_test_debuggee PROPERTY CXX_COMPILER_LAUNCHER "")
frost_apply_compile_options(debug_test_debuggee TEST)
frost_copy_runtime_dlls(debug_test_debuggee)
if(MSVC)
    target_compile_options(debug_test_debuggee PRIVATE /Od /Ob0 /Z7)
    target_link_options(debug_test_debuggee PRIVATE /DEBUG:FULL /INCREMENTAL:NO)
else()
    target_compile_options(debug_test_debuggee PRIVATE -O0 -g)
endif()

# The dap-real scenario program (fixtures/dap_scenarios.cpp), built like the debuggee: no
# optimisation, a full PDB on Windows (clang-cl), DWARF on Linux (clang).
add_executable(debug_test_scenarios fixtures/dap_scenarios.cpp)
set_property(TARGET debug_test_scenarios PROPERTY CXX_COMPILER_LAUNCHER "")
frost_apply_compile_options(debug_test_scenarios TEST)
frost_copy_runtime_dlls(debug_test_scenarios)
if(MSVC)
    target_compile_options(debug_test_scenarios PRIVATE /Od /Ob0 /Z7)
    target_link_options(debug_test_scenarios PRIVATE /DEBUG:FULL /INCREMENTAL:NO)
else()
    target_compile_options(debug_test_scenarios PRIVATE -O0 -g)
    find_package(Threads REQUIRED)
    target_link_libraries(debug_test_scenarios PRIVATE Threads::Threads)
endif()

frost_add_test(debug_lldb_tests
    SOURCES lldb_session_test.cpp
    DEPS frost::debug frost::io
    LABELS integration lldb)
target_include_directories(debug_lldb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(debug_lldb_tests PRIVATE
    FROST_DEBUGGEE_PATH="$<TARGET_FILE:debug_test_debuggee>"
    FROST_DEBUGGEE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/debuggee.cpp")
add_dependencies(debug_lldb_tests debug_test_debuggee)

# The dap-real scenario suite (DBG-1, DBG-2a): one scenario per test against fixtures/dap_scenarios.cpp.
frost_add_test(debug_dap_real_tests
    SOURCES dap_real_test.cpp
    DEPS frost::debug frost::io
    LABELS integration lldb)
target_include_directories(debug_dap_real_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(debug_dap_real_tests PRIVATE
    FROST_SCENARIOS_PATH="$<TARGET_FILE:debug_test_scenarios>"
    FROST_SCENARIOS_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dap_scenarios.cpp")
add_dependencies(debug_dap_real_tests debug_test_scenarios)

# The corpus debug test (corpus_debug_test.cpp; DBG-1/DBG-2a acceptance): FrostEditor of the pinned
# FrostEngine clone, built under the corpus build lock and debugged with lldb-dap. Label corpus, so
# only the corpus tier (tools\verify.ps1 -Corpus) runs it. Windows only: on Linux FrostEditor is most
# of an engine build, which the corpus tier never does (tools/corpora.py, "engine").
if(WIN32)
    set(_frost_debug_corpora_lock_file "${FROST_SOURCE_DIR}/tests/corpus/corpora.lock.json")
    set(_frost_debug_sdk_lock_file "${FROST_SOURCE_DIR}/sdk/sdk.lock.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_frost_debug_corpora_lock_file}" "${_frost_debug_sdk_lock_file}")
    file(READ "${_frost_debug_corpora_lock_file}" _frost_debug_corpora_lock)
    string(JSON _frost_debug_count LENGTH "${_frost_debug_corpora_lock}" corpora)
    math(EXPR _frost_debug_last "${_frost_debug_count} - 1")
    set(_frost_debug_engine_sha "")
    foreach(_frost_debug_index RANGE ${_frost_debug_last})
        string(JSON _frost_debug_id GET "${_frost_debug_corpora_lock}" corpora ${_frost_debug_index} id)
        if(_frost_debug_id STREQUAL "frostengine")
            string(JSON _frost_debug_engine_sha GET "${_frost_debug_corpora_lock}" corpora ${_frost_debug_index} sha)
            string(JSON _frost_debug_engine_preset GET "${_frost_debug_corpora_lock}" corpora ${_frost_debug_index}
                   presets windows)
        endif()
    endforeach()
    if(NOT _frost_debug_engine_sha MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "${_frost_debug_corpora_lock_file} pins no frostengine corpus")
    endif()
    file(READ "${_frost_debug_sdk_lock_file}" _frost_debug_sdk_lock)
    string(JSON _frost_debug_vc_tools GET "${_frost_debug_sdk_lock}" msvc vcToolsVersion)
    frost_add_test(debug_corpus_tests
        SOURCES corpus_debug_test.cpp
        DEPS frost::debug frost::build frost::io
        LABELS integration corpus lldb)
    target_include_directories(debug_corpus_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FROST_SOURCE_DIR}/tests)
    target_compile_definitions(debug_corpus_tests PRIVATE
        FROST_CORPUS_FROSTENGINE_SHA="${_frost_debug_engine_sha}"
        FROST_CORPUS_FROSTENGINE_PRESET="${_frost_debug_engine_preset}"
        FROST_PINNED_VC_TOOLS="${_frost_debug_vc_tools}"
        FROST_TEST_CMAKE="${CMAKE_COMMAND}")
endif()
