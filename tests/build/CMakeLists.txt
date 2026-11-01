# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::build:
#   build_unit_tests     the diagnostic parser, the line splitter, the JSON reader, vswhere parsing and
#                        instance selection, the MSBuild command line, BuildSession against the
#                        scripted launcher, diagnostic resolution and BuildService over a MemFs
#                        solution, label "unit" (every platform);
#   build_msbuild_tests  real vswhere and MSBuild against the tiny fixture solution (fixtures/), directly
#                        and through BuildService, label "integration;msbuild", Windows only. Filter
#                        with ctest -L msbuild or -LE msbuild.
#                        The builds run in a work directory under this build tree, not under %TEMP%, where
#                        MSBuild warns about intermediate directories (MSB8029).

frost_add_test(build_unit_tests
    SOURCES
        build_diagnostics_test.cpp
        build_service_test.cpp
        build_session_test.cpp
        diagnostic_test.cpp
        json_test.cpp
        line_splitter_test.cpp
        msbuild_driver_test.cpp
        msbuild_locator_test.cpp
    DEPS frost::build)
# src/build/src: json_test.cpp tests the private JSON reader.
target_include_directories(build_unit_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/build/src)

if(WIN32)
    frost_add_test(build_msbuild_tests
        SOURCES msbuild_build_test.cpp
        DEPS frost::build
        LABELS integration msbuild)
    target_include_directories(build_msbuild_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
    target_compile_definitions(build_msbuild_tests PRIVATE
        FROST_BUILD_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        FROST_BUILD_WORK_ROOT="${CMAKE_CURRENT_BINARY_DIR}/msbuild-work")
endif()
