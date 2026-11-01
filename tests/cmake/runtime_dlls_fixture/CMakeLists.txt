# SPDX-License-Identifier: Apache-2.0
#
# Fixture for tests/cmake/check_runtime_dlls.cmake: executables in the layouts FrostIDE
# has or may get, with stand-ins for the DLLs, so the check can see who copies what
# where and who waits for the copy (cmake/FrostLayers.cmake, frost_copy_runtime_dlls):
#   shared/   three registered executables in one output directory (like tests/sched or
#             tests/app), two of them linking the stand-in Qt library and one not, and
#             shared_raw, a plain add_executable that is never registered (like
#             s1_probe_validate beside s1_editor) and links a stand-in nobody else does;
#   top_plain in this directory's own output directory, linking nothing, so without the
#             ASan stand-in its directory has no DLLs at all;
#   bin_a/    and bin_root here: both set RUNTIME_OUTPUT_DIRECTORY to <build>/bin (one
#             spelling relative, one absolute), so two source directories share one
#             output directory.
# FIXTURE_ASAN (default ON) sets the ASan runtime stand-in. FIXTURE_PER_CONFIG_OUTPUT
# set to `registered` or `raw` adds a registered or an unregistered executable with
# RUNTIME_OUTPUT_DIRECTORY_DEBUG, which the helper does not support and must reject at
# configure time.
cmake_minimum_required(VERSION 4.0)
project(frost_runtime_dlls_fixture LANGUAGES CXX)
option(FIXTURE_ASAN "Set the ASan runtime stand-in" ON)
set(FIXTURE_PER_CONFIG_OUTPUT "" CACHE STRING "Add a per-configuration output directory: registered or raw")
list(APPEND CMAKE_MODULE_PATH "${FROST_SOURCE_DIR}/cmake")
include(FrostLayers)

set(fake "${CMAKE_BINARY_DIR}/fake")
file(MAKE_DIRECTORY "${fake}")
if(FIXTURE_ASAN)
    # Stands in for clang's ASan runtime, which the root CMakeLists.txt names only in
    # clang-cl ASan builds; on Linux the helper must ignore it.
    set(FROST_ASAN_RUNTIME_DLL "${fake}/fake_asan_runtime.dll")
    file(WRITE "${FROST_ASAN_RUNTIME_DLL}" "stand-in\n")
endif()
if(WIN32)
    # Stand in for imported shared libraries (fake::qt for a Qt module, fake::extra for
    # one only shared_raw links): DLLs that are never loaded, and import libraries
    # that are empty archives (just the magic, with a bare LF, which file(WRITE) would
    # turn into CRLF), so linking them adds nothing.
    foreach(module IN ITEMS qt extra)
        file(WRITE "${fake}/fake_${module}.dll" "stand-in\n")
        file(CONFIGURE OUTPUT "${fake}/fake_${module}.lib" CONTENT "!<arch>\n" NEWLINE_STYLE UNIX)
        add_library(fake::${module} SHARED IMPORTED)
        set_target_properties(fake::${module} PROPERTIES
            IMPORTED_LOCATION "${fake}/fake_${module}.dll"
            IMPORTED_IMPLIB "${fake}/fake_${module}.lib")
    endforeach()
endif()

# Adds executable <name> (an empty main) that needs its runtime DLLs; WITH_QT links the
# stand-in Qt library on Windows. RAW leaves it unregistered.
function(fixture_consumer name)
    cmake_parse_arguments(ARG "WITH_QT;RAW" "" "" ${ARGN})
    add_executable(${name} "${PROJECT_SOURCE_DIR}/main.cpp")
    if(ARG_WITH_QT AND WIN32)
        target_link_libraries(${name} PRIVATE fake::qt)
    endif()
    if(NOT ARG_RAW)
        frost_copy_runtime_dlls(${name})
    endif()
endfunction()

add_subdirectory(shared)
add_subdirectory(bin_a)
fixture_consumer(top_plain)
fixture_consumer(bin_root)
set_target_properties(bin_root PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(FIXTURE_PER_CONFIG_OUTPUT STREQUAL "registered")
    fixture_consumer(per_config)
elseif(FIXTURE_PER_CONFIG_OUTPUT STREQUAL "raw")
    fixture_consumer(per_config RAW)
endif()
if(TARGET per_config)
    set_target_properties(per_config PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/debug-bin")
endif()
