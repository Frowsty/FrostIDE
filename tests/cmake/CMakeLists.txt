# SPDX-License-Identifier: Apache-2.0
#
# Tests of the build system itself. FrostLayers.RejectsViolations configures a
# fixture full of layering violations with this build's toolchain and checks
# that each one is reported (tests/cmake/check_layering.cmake).
# FrostCompilerCache.RechecksCachedSccache reconfigures a fixture without languages
# while the sccache pin and the installed copies change, and checks that a cached
# sccache that fails the version check is searched for again
# (tests/cmake/check_compiler_cache.cmake).
# FrostRuntimeDlls.CopiesOncePerDirectory configures and builds a fixture whose
# executables share output directories and checks that each directory gets its DLLs
# from exactly one copy step per configuration (tests/cmake/check_runtime_dlls.cmake).
if(VCPKG_CHAINLOAD_TOOLCHAIN_FILE)
    set(_frost_fixture_toolchain "${VCPKG_CHAINLOAD_TOOLCHAIN_FILE}")
else()
    set(_frost_fixture_toolchain "${CMAKE_TOOLCHAIN_FILE}")
endif()
add_test(NAME FrostLayers.RejectsViolations
    COMMAND ${CMAKE_COMMAND}
        -DFROST_SOURCE_DIR=${FROST_SOURCE_DIR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/layering_fixture
        "-DGENERATOR=Ninja"
        -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        -DTOOLCHAIN=${_frost_fixture_toolchain}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_layering.cmake)
set_tests_properties(FrostLayers.RejectsViolations PROPERTIES LABELS unit TIMEOUT 120)

add_test(NAME FrostCompilerCache.RechecksCachedSccache
    COMMAND ${CMAKE_COMMAND}
        -DFROST_SOURCE_DIR=${FROST_SOURCE_DIR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/compiler_cache_fixture
        -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_compiler_cache.cmake)
set_tests_properties(FrostCompilerCache.RechecksCachedSccache PROPERTIES LABELS unit TIMEOUT 120)

add_test(NAME FrostRuntimeDlls.CopiesOncePerDirectory
    COMMAND ${CMAKE_COMMAND}
        -DFROST_SOURCE_DIR=${FROST_SOURCE_DIR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/runtime_dlls_fixture
        -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        -DTOOLCHAIN=${_frost_fixture_toolchain}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_runtime_dlls.cmake)
set_tests_properties(FrostRuntimeDlls.CopiesOncePerDirectory PROPERTIES LABELS unit TIMEOUT 180)
