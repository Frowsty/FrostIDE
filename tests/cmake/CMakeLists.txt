# SPDX-License-Identifier: Apache-2.0
#
# Tests of the build system itself. FrostLayers.RejectsViolations configures a
# fixture full of layering violations with this build's toolchain and checks
# that each one is reported (tests/cmake/check_layering.cmake).
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
