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
#                      It skips while lldb-dap is not installed.

frost_add_test(debug_unit_tests
    SOURCES
        breakpoints_test.cpp
        coreclr_dap_test.cpp
        dap_client_test.cpp
        debug_session_test.cpp
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

frost_add_test(debug_lldb_tests
    SOURCES lldb_session_test.cpp
    DEPS frost::debug frost::io
    LABELS integration lldb)
target_include_directories(debug_lldb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(debug_lldb_tests PRIVATE
    FROST_DEBUGGEE_PATH="$<TARGET_FILE:debug_test_debuggee>"
    FROST_DEBUGGEE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/debuggee.cpp")
add_dependencies(debug_lldb_tests debug_test_debuggee)
