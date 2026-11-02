# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::build:
#   build_unit_tests     the diagnostic parser, the line splitter, the JSON reader, vswhere parsing and
#                        instance selection, the MSBuild command line, BuildSession against the
#                        scripted launcher, diagnostic resolution and BuildService over a MemFs
#                        solution, label "unit" (every platform);
#   build_msbuild_tests  real vswhere and MSBuild against the tiny fixture solution (fixtures/), directly
#                        and through BuildService, label "integration" (so RESOURCE_LOCK frost_desktop;
#                        ADR 0027) with the tag "msbuild", Windows only. Filter with ctest -L msbuild or
#                        -LE msbuild.
#                        The builds run in a work directory under this build tree, not under %TEMP%, where
#                        MSBuild warns about intermediate directories (MSB8029).
#   build_cmake_tests    a real configure and build of the tiny CMake project (fixtures/cmake/tiny) with
#                        the pinned cmake and ninja and FrostIDE's own C compiler (clang-cl through the
#                        captured MSVC environment on Windows, clang on Linux): the model, diagnostics,
#                        Clean, the reconfigure on a CMakeLists.txt change and a CMake Error; label
#                        "integration" with the tag "cmake" (ADR 0046).
#   build_msvc_env_tests the real vcvarsall.bat capture, label "integration" with the tag "msvc",
#                        Windows only.
# The CMake unit tests (output parsers, toolchain, CMakeService) are in build_unit_tests; they read the
# synthetic File API reply of tests/project/fixtures/cmake/reply.

frost_add_test(build_unit_tests
    SOURCES
        build_diagnostics_test.cpp
        build_service_test.cpp
        build_session_test.cpp
        cmake_service_test.cpp
        cmake_toolchain_test.cpp
        diagnostic_test.cpp
        json_test.cpp
        line_splitter_test.cpp
        msbuild_driver_test.cpp
        msbuild_locator_test.cpp
        output_parsers_test.cpp
    DEPS frost::build)
# src/build/src: json_test.cpp tests the private JSON reader.
target_include_directories(build_unit_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/build/src)
target_compile_definitions(build_unit_tests PRIVATE
    FROST_CMAKE_REPLY_FIXTURE_DIR="${FROST_SOURCE_DIR}/tests/project/fixtures/cmake/reply")

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

frost_add_test(build_cmake_tests
    SOURCES cmake_build_test.cpp
    DEPS frost::build
    LABELS integration cmake)
target_include_directories(build_cmake_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
# The tools that configured FrostIDE (the pinned cmake and ninja) and its own C compiler.
target_compile_definitions(build_cmake_tests PRIVATE
    FROST_BUILD_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    FROST_TEST_CMAKE="${CMAKE_COMMAND}"
    FROST_TEST_NINJA="${CMAKE_MAKE_PROGRAM}"
    FROST_TEST_C_COMPILER="${CMAKE_C_COMPILER}")

if(WIN32)
    frost_add_test(build_msvc_env_tests
        SOURCES msvc_env_test.cpp
        DEPS frost::build
        LABELS integration msvc)
    target_include_directories(build_msvc_env_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
endif()
