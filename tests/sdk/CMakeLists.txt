# SPDX-License-Identifier: Apache-2.0
#
# Tests of the SDK artifacts that sdk/sdk.lock.json pins and the repository's recipes build:
#   sdk_lldb_tests     the pinned lldb-dap (tools/build-lldb.ps1, tools/build-lldb.sh; ADR 0049) end
#                      to end on the fixture debuggee over stdio: initialize, launch, setBreakpoints,
#                      configurationDone, the stop and the top frame's line, threads, continue to the
#                      exit code, disconnect; label "integration" with the tag "lldb" (ctest -L lldb).
#                      Skipped with a message when the pinned lldb-dap is not installed. A second or
#                      two per test.
#   sdk_lldb_debuggee  the fixture debuggee (fixtures/lldb_debuggee), the program those tests debug;
#                      not a test itself. Built with the test toolchain but without frost's options or
#                      sanitizers, unoptimised and with full debug information in every configuration
#                      (a PDB from lld-link on Windows, DWARF on Linux), and with the static CRT on
#                      Windows, so it needs no DLL beside it.

file(READ "${FROST_SOURCE_DIR}/sdk/sdk.lock.json" _frost_sdk_lock)
if(WIN32)
    string(JSON _frost_lldb_version GET "${_frost_sdk_lock}" windows lldb version)
    set(_frost_sdk_root "E:/sdk")
else()
    string(JSON _frost_lldb_version GET "${_frost_sdk_lock}" linux lldb version)
    set(_frost_sdk_root "$ENV{HOME}/sdk")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FROST_SOURCE_DIR}/sdk/sdk.lock.json")

set(_frost_debuggee_dir ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/lldb_debuggee)
add_executable(sdk_lldb_debuggee ${_frost_debuggee_dir}/main.cpp ${_frost_debuggee_dir}/worker.cpp)
target_compile_features(sdk_lldb_debuggee PRIVATE cxx_std_17)
if(MSVC)
    # /Z7 here and /DEBUG:FULL below: lld-link writes the PDB that lldb-dap reads with its native reader.
    target_compile_options(sdk_lldb_debuggee PRIVATE /Od /Ob0 /Z7 /EHsc)
    target_link_options(sdk_lldb_debuggee PRIVATE /DEBUG:FULL /INCREMENTAL:NO)
    set_target_properties(sdk_lldb_debuggee PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
else()
    target_compile_options(sdk_lldb_debuggee PRIVATE -O0 -g)
    target_link_libraries(sdk_lldb_debuggee PRIVATE Threads::Threads)
endif()

frost_add_test(sdk_lldb_tests
    SOURCES
        lldb_dap_test.cpp
    DEPS frost::lang frost::io
    LABELS integration lldb)
target_include_directories(sdk_lldb_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(sdk_lldb_tests PRIVATE
    FROST_LLDB_VERSION="${_frost_lldb_version}"
    FROST_SDK_DEFAULT_ROOT="${_frost_sdk_root}"
    FROST_LLDB_DEBUGGEE="$<TARGET_FILE:sdk_lldb_debuggee>"
    FROST_LLDB_DEBUGGEE_SOURCE_DIR="${_frost_debuggee_dir}")
add_dependencies(sdk_lldb_tests sdk_lldb_debuggee)
if(WIN32)
    # user32: tests/support/private_desktop.hpp.
    target_link_libraries(sdk_lldb_tests PRIVATE user32)
endif()
