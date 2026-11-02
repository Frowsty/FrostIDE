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
# executables and shared library share output directories and checks that each directory
# gets its DLLs from exactly one copy step per configuration, and that every executable and
# shared library there, registered or not, is ordered after it
# (tests/cmake/check_runtime_dlls.cmake).
# io_no_static_init checks that frost_io's only static initialisers are the asio ones ADR 0020
# lists, in the objects that include asio, and that reactor.cpp carries the Winsock guard
# (tests/cmake/check_static_init.cmake; PLAN.md section 2.1). Registered only without sanitizers,
# whose instrumentation adds module constructors.
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

# The pinned Windows cache must keep each /Z7 object's source-root identity, while allowing
# same-tree hits and honoring the caller's changing cache-buster (isolated sccache server).
if(WIN32 AND _frost_sccache_found AND DEFINED ENV{FROST_SDK} AND DEFINED ENV{FROST_LLVM_ROOT})
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    add_test(NAME FrostCompilerCache.PreservesDebugSourceIdentity
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/check_compiler_cache_identity.py
            ${FROST_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(FrostCompilerCache.PreservesDebugSourceIdentity PROPERTIES LABELS unit TIMEOUT 120)
endif()


add_test(NAME FrostRuntimeDlls.CopiesOncePerDirectory
    COMMAND ${CMAKE_COMMAND}
        -DFROST_SOURCE_DIR=${FROST_SOURCE_DIR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/runtime_dlls_fixture
        -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        -DTOOLCHAIN=${_frost_fixture_toolchain}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_runtime_dlls.cmake)
# PROCESSORS 4 for parallel ctest (ADR 0027 decision 3): the fixture build runs ninja with its
# default parallelism, so it counts as several of ctest's -j slots.
set_tests_properties(FrostRuntimeDlls.CopiesOncePerDirectory PROPERTIES LABELS unit TIMEOUT 180 PROCESSORS 4)

if(NOT FROST_SANITIZE)
    # The LLVM tools beside the compiler (the pinned LLVM; never a PATH search).
    get_filename_component(_frost_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(FROST_LLVM_OBJDUMP llvm-objdump PATHS "${_frost_llvm_bin}" NO_DEFAULT_PATH REQUIRED)
    if(WIN32)
        find_program(FROST_LLVM_DEMANGLER llvm-undname PATHS "${_frost_llvm_bin}" NO_DEFAULT_PATH REQUIRED)
        set(_frost_object_format COFF)
    else()
        find_program(FROST_LLVM_DEMANGLER llvm-cxxfilt PATHS "${_frost_llvm_bin}" NO_DEFAULT_PATH REQUIRED)
        set(_frost_object_format ELF)
    endif()
    add_test(NAME io_no_static_init
        COMMAND ${CMAKE_COMMAND}
            -DOBJDUMP=${FROST_LLVM_OBJDUMP}
            -DDEMANGLER=${FROST_LLVM_DEMANGLER}
            -DARCHIVE=$<TARGET_FILE:frost_io>
            -DFORMAT=${_frost_object_format}
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_static_init.cmake)
    set_tests_properties(io_no_static_init PROPERTIES LABELS unit TIMEOUT 120)
endif()
