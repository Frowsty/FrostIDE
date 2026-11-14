# SPDX-License-Identifier: Apache-2.0
#
# frost::debug (PLAN.md section 3.2 and M4; ADR 0050): U1's minimal debugger, Qt-free. The DAP 1.71
# client over an adapter's stdio (Content-Length framing from frost::rpc), the fake-dap double the
# tests script, the DebugSession model (launch sequence, stops, threads, lazy frames, scopes,
# variables, evaluate, stepping), line breakpoints that move with edits and persist in
# .frost/workspace.json, and lldb-dap: discovery at its pinned SDK path, its process spec and the
# trust-gated session start. Depends on rpc, project (trust), settings (workspace.json), text
# (anchors) and io; never on build or lang (the same rank), so the Debug run's hand-off is
# translated in frost::ui.
# TODO(M6): attach, conditional and function breakpoints, data breakpoints, the disassembly and
# memory views, symbol servers and natvis (ADR 0050, "M6").

# The lldb version pinned in sdk/sdk.lock.json (its "lldb" entry when present, else LLVM's: lldb-dap
# ships with the LLVM release), so findLldbDap can name the SDK's lldb-dap.
file(READ "${FROST_SOURCE_DIR}/sdk/sdk.lock.json" _frost_sdk_lock)
if(WIN32)
    set(_frost_sdk_os windows)
else()
    set(_frost_sdk_os linux)
endif()
string(JSON _frost_lldb_version ERROR_VARIABLE _frost_lldb_missing GET "${_frost_sdk_lock}" ${_frost_sdk_os} lldb version)
if(_frost_lldb_missing)
    string(JSON _frost_lldb_version GET "${_frost_sdk_lock}" ${_frost_sdk_os} llvm version)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FROST_SOURCE_DIR}/sdk/sdk.lock.json")

frost_add_library(debug LAYER debug
    SOURCES
        src/breakpoints.cpp
        src/coreclr_dap.cpp
        src/dap_client.cpp
        src/dap_types.cpp
        src/dbgeng_dap.cpp
        src/natvis_discovery.cpp
        src/debug_session.cpp
        src/debug_session_breakpoints.cpp
        src/debug_session_requests.cpp
        src/debug_session_stops.cpp
        src/fake_debug_adapter.cpp
        src/lldb_dap.cpp
        src/gdb_dap.cpp
        src/gdb_transport.cpp
        src/msvc_throw.cpp
        src/mono_dap.cpp
        src/mono_wire.cpp
        src/mono_session.cpp
        src/mono_responses.cpp
        src/mono_gate.cpp
        src/mono_protocol_session.cpp
        src/mono_transport.cpp
        src/mono_windows_peer.cpp
        src/lua_debug_adapter.cpp
        src/lua_debug_endpoint.cpp
        src/lua_dap_transport.cpp
        src/lua_dap_wire.cpp
    DEPS frost::rpc frost::project frost::settings frost::text frost::io frost::core
    THIRD_PARTY unofficial-sodium::sodium)
target_compile_definitions(frost_debug PRIVATE FROST_PINNED_LLDB_VERSION="${_frost_lldb_version}")

if(WIN32)
    target_link_libraries(frost_debug PRIVATE iphlpapi)
endif()
