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
#   debug_msvc_tests   (Windows) cl.exe-built fixtures/msvc_debuggee.cpp under the pinned lldb-dap:
#                      type-filtered C++ throws (DBG-2b), STL display (DBG-4a) and Step Into through
#                      an incremental-link thunk, label "integration" with the tags "lldb" and "msvc";
#   debug_corpus_tests (Windows) FrostEditor of the pinned FrostEngine clone under lldb-dap
#                      (corpus_debug_test.cpp), label "integration" with the tags "corpus" and "lldb".

frost_add_test(debug_unit_tests
    SOURCES
        breakpoints_test.cpp
        coreclr_dap_test.cpp
        standalone_lua_dap_test.cpp
        cmake_dap_test.cpp
        dap_client_test.cpp
        dap_private_test.cpp
        dbgeng_dap_test.cpp
        pinned_watches_test.cpp
        debug_depth_test.cpp
        natvis_discovery_test.cpp
        debug_session_test.cpp
        debug_session_multi_test.cpp
        debug_terminate_test.cpp
        debug_session_breakpoints_test.cpp
        lldb_dap_test.cpp
        gdb_dap_test.cpp
        mono_dap_test.cpp
        msvc_throw_test.cpp
        mono_wire_test.cpp
        mono_session_test.cpp
        mono_responses_test.cpp
        mono_gate_test.cpp
        mono_protocol_session_test.cpp
        mono_transport_test.cpp
    DEPS frost::debug frost::project frost::text frost::io)
target_include_directories(debug_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The legacy Lua DAP catalogue/refusal checks do not claim live protocol conformance. The actual
# locked-engine gates below exercise Debug, Release and Dist, including watchdog and hook safety.
frost_add_test(lua_dap_conformance_tests
    SOURCES lua_dap_conformance_test.cpp
    DEPS frost::debug frost::io frost::rpc frost::core
    LABELS unit)
target_include_directories(lua_dap_conformance_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lua_dap_conformance_tests PRIVATE
    FROST_LUA_DAP_CONFORMANCE_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/lua_dap_conformance_baseline.json")

# Authentication tests use actual loopback sockets, including negative transcript vectors.
frost_add_test(lua_debug_endpoint_tests
    SOURCES lua_debug_endpoint_test.cpp
    DEPS frost::debug frost::rpc frost::io frost::core
    THIRD_PARTY unofficial-sodium::sodium
    LABELS integration lua)
target_include_directories(lua_debug_endpoint_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    target_link_libraries(lua_debug_endpoint_tests PRIVATE ws2_32)
endif()

# FER-8/LUA-11 uses the same locked engine SHA as every R6 corpus target. Release and Dist
# require separately provisioned native engine presets; Debug timing cannot substitute for them.
include("${FROST_SOURCE_DIR}/tests/support/EngineCorpus.cmake")

# DBG-11 drives actual LLDB native all-stop and the pinned Lua endpoint through the app coordinator.
if(FROST_BUILD_UI)
    frost_add_test(engine_mixed_debug_real_corpus_tests QT
        SOURCES engine_mixed_debug_real_corpus_test.cpp
        DEPS frost::app frost::ui_debug frost::debug frost::engine frost::build frost::io frost::rpc
        LABELS integration corpus lldb lua
        TIMEOUT 18000)
    frost_configure_engine_corpus_target(engine_mixed_debug_real_corpus_tests)
    target_include_directories(engine_mixed_debug_real_corpus_tests PRIVATE ${FROST_SOURCE_DIR}/src/app/src)
    target_compile_definitions(engine_mixed_debug_real_corpus_tests PRIVATE
        FROST_APP_WORK_ROOT="${CMAKE_CURRENT_BINARY_DIR}/mixed-work"
        FROST_BUILD_FIXTURES="${FROST_SOURCE_DIR}/tests/build/fixtures")
endif()

set(_frost_lua_nm "${FROST_LLVM_ROOT}/bin/llvm-nm${CMAKE_EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${_frost_lua_nm}")
    message(FATAL_ERROR "the pinned LLVM symbol tool is required for real Lua Dist acceptance: ${_frost_lua_nm}")
endif()
if(WIN32)
    set(_frost_lua_release_preset windows-release)
    set(_frost_lua_dist_preset windows-dist)
else()
    set(_frost_lua_release_preset linux-release)
    set(_frost_lua_dist_preset linux-dist)
endif()
foreach(_frost_lua_gate IN ITEMS conformance release dist)
    set(_frost_lua_target "lua_dap_real_${_frost_lua_gate}_tests")
    frost_add_test(${_frost_lua_target}
        SOURCES lua_dap_real_corpus_test.cpp
        DEPS frost::debug frost::engine frost::build frost::io frost::rpc frost::core
        LABELS integration corpus lua
        TIMEOUT 18000)
    frost_configure_engine_corpus_target(${_frost_lua_target})
    target_compile_definitions(${_frost_lua_target} PRIVATE
        FROST_LUA_REAL_DRIVER="${CMAKE_CURRENT_SOURCE_DIR}/lua_dap_real_driver.py"
        FROST_LUA_REAL_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/lua_real"
        FROST_LUA_REAL_CATALOGUE="${CMAKE_CURRENT_SOURCE_DIR}/lua_dap_conformance_baseline.json"
        FROST_LUA_REAL_NM="${_frost_lua_nm}"
        FROST_LUA_REAL_RELEASE_PRESET="${_frost_lua_release_preset}"
        FROST_LUA_REAL_MODE="${_frost_lua_gate}")
    if(_frost_lua_gate STREQUAL "conformance")
        get_target_property(_frost_lua_debug_preset ${_frost_lua_target} FROST_ENGINE_CORPUS_PRESET)
        target_compile_definitions(${_frost_lua_target} PRIVATE FROST_LUA_REAL_CONFORMANCE=1
            FROST_LUA_REAL_ENGINE_PRESET="${_frost_lua_debug_preset}")
    elseif(_frost_lua_gate STREQUAL "release")
        target_compile_definitions(${_frost_lua_target} PRIVATE FROST_LUA_REAL_RELEASE=1
            FROST_LUA_REAL_ENGINE_PRESET="${_frost_lua_release_preset}")
    else()
        target_compile_definitions(${_frost_lua_target} PRIVATE FROST_LUA_REAL_DIST=1
            FROST_LUA_REAL_ENGINE_PRESET="${_frost_lua_dist_preset}")
    endif()
endforeach()

if(WIN32)
    # Helpers build in the pinned engine; the IDE links none of its archives or CRT.
    frost_add_test(debug_dbgeng_real_tests
        SOURCES dbgeng_real_test.cpp
        DEPS frost::debug frost::engine frost::build frost::rpc frost::io frost::core
        LABELS integration corpus dbgeng
        TIMEOUT 18000)
    frost_configure_engine_corpus_target(debug_dbgeng_real_tests)
    target_include_directories(debug_dbgeng_real_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(debug_dbgeng_real_tests PRIVATE
        FROST_DBGENG_ADAPTER_PATH="$<TARGET_FILE:frost-dbgeng-dap>")
    add_dependencies(debug_dbgeng_real_tests frost-dbgeng-dap)
endif()

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

# The MSVC debuggee tests (msvc_debug_test.cpp; DBG-2b, DBG-4a): fixtures/msvc_debuggee.cpp built at
# test time by cl.exe of the pinned MSVC toolset (sdk/sdk.lock.json, msvc.vcToolsVersion) and debugged
# with the pinned lldb-dap. Windows only: the MSVC ABI's throws and STL exist only there.
if(WIN32)
    file(READ "${FROST_SOURCE_DIR}/sdk/sdk.lock.json" _frost_debug_msvc_lock)
    string(JSON _frost_debug_msvc_tools GET "${_frost_debug_msvc_lock}" msvc vcToolsVersion)
    frost_add_test(debug_natvis_pack_tests
        SOURCES natvis_pack_real_test.cpp
        DEPS frost::debug frost::build frost::io
        LABELS integration dbgeng msvc)
    target_include_directories(debug_natvis_pack_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FROST_SOURCE_DIR}/tests)
    target_compile_definitions(debug_natvis_pack_tests PRIVATE
        FROST_PINNED_VC_TOOLS="${_frost_debug_msvc_tools}"
        FROST_DBGENG_ADAPTER_PATH="$<TARGET_FILE:frost-dbgeng-dap>"
        FROST_VISUALIZER_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/visualizer_types.cpp"
        FROST_VISUALIZER_PACK="${FROST_SOURCE_DIR}/assets/visualizers/FrostCommon.natvis")
    add_dependencies(debug_natvis_pack_tests frost-dbgeng-dap)
    frost_add_test(debug_dbgeng_depth_tests
        SOURCES dbgeng_native_depth_test.cpp
        DEPS frost::debug frost::build frost::rpc frost::io frost::core
        LABELS integration dbgeng msvc
        TIMEOUT 180)
    target_include_directories(debug_dbgeng_depth_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FROST_SOURCE_DIR}/tests)
    target_compile_definitions(debug_dbgeng_depth_tests PRIVATE
        FROST_DBGENG_ADAPTER_PATH="$<TARGET_FILE:frost-dbgeng-dap>"
        FROST_DBGENG_DEPTH_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dbgeng_native_depth.cpp"
        FROST_PINNED_VC_TOOLS="${_frost_debug_msvc_tools}")
    add_dependencies(debug_dbgeng_depth_tests frost-dbgeng-dap)
    frost_add_test(debug_msvc_tests
        SOURCES msvc_debug_test.cpp
        DEPS frost::debug frost::build frost::io
        LABELS integration lldb msvc)
    target_include_directories(debug_msvc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FROST_SOURCE_DIR}/tests)
    target_compile_definitions(debug_msvc_tests PRIVATE
        FROST_MSVC_DEBUGGEE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/msvc_debuggee.cpp"
        FROST_PINNED_VC_TOOLS="${_frost_debug_msvc_tools}")
endif()

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


# User-installed GDB >=16 with Python/DAP; no downloaded GPL binary. Fails when not provisioned.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    frost_add_test(debug_gdb_tests
        SOURCES gdb_dap_real_test.cpp
        DEPS frost::debug frost::project frost::io
        LABELS integration gdb)
    target_include_directories(debug_gdb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(debug_gdb_tests PRIVATE
        FROST_DEBUGGEE_PATH="$<TARGET_FILE:debug_test_debuggee>"
        FROST_DEBUGGEE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/debuggee.cpp"
        FROST_GDB_TRUST_PATH="${CMAKE_CURRENT_BINARY_DIR}/gdb-trusted-roots.txt")
    add_dependencies(debug_gdb_tests debug_test_debuggee)
endif()

# Authored R12 real-tool workflows; missing configured tools fail, never skip/pass by simulation.
include("${FROST_SOURCE_DIR}/cmake/FrostLuaRuntime.cmake")
frost_add_test(debug_standalone_lua_real_tests
    SOURCES standalone_lua_real_test.cpp
    DEPS frost::debug frost::run frost::project frost::io frost::core
    LABELS integration lua standalone)
target_include_directories(debug_standalone_lua_real_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FROST_SOURCE_DIR}/tests)
file(TO_CMAKE_PATH "${FROST_LUA54_EXECUTABLE}" _frost_test_lua)
file(TO_CMAKE_PATH "${FROST_LUA_DEBUG_ADAPTER}" _frost_test_lua_adapter)
target_compile_definitions(debug_standalone_lua_real_tests PRIVATE
    FROST_TEST_LUA_INTERPRETER="${_frost_test_lua}"
    FROST_TEST_LUA_DEBUG_ADAPTER="${_frost_test_lua_adapter}"
    FROST_TEST_LUA_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/standalone_lua.lua")
