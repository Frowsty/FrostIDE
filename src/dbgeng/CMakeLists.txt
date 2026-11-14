# SPDX-License-Identifier: Apache-2.0
# Windows-only FER-16 adapter. DbgEng is dynamically loaded from System32 (ADR 0081),
# never imported from a redistributed debugger package. The DAP sidecar has no Qt dependency.
frost_add_executable(frost-dbgeng-dap LAYER debug
    SOURCES main.cpp adapter.cpp model.cpp internal.hpp
    DEPS frost::rpc frost::io frost::core)
target_link_libraries(frost-dbgeng-dap PRIVATE oleaut32)
# The application resolves its own trusted sidecar beside frost.exe. A plain output directory
# also lets the shared runtime-DLL deployment collect its dependencies for that same directory.
set_target_properties(frost-dbgeng-dap PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/../app"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/../app")
