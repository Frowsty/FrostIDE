# SPDX-License-Identifier: Apache-2.0
# Small build for the cross-worktree sccache debug-record regression.
cmake_minimum_required(VERSION 4.0)
project(cache_identity LANGUAGES NONE)
include("${FROST_REPO}/cmake/FrostCompilerCache.cmake")
file(WRITE "${CMAKE_BINARY_DIR}/launcher.txt" "${CMAKE_CXX_COMPILER_LAUNCHER}")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/unit.obj"
    COMMAND ${CMAKE_CXX_COMPILER_LAUNCHER} "${FROST_CLANG_CL}" /nologo /c /Z7
            "/Fo${CMAKE_BINARY_DIR}/unit.obj" -- "${CMAKE_CURRENT_SOURCE_DIR}/unit.cpp"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit.cpp" VERBATIM)
add_custom_target(compile ALL DEPENDS "${CMAKE_BINARY_DIR}/unit.obj")
