# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::project (W1, ADR 0032): the .sln, .slnx, .vcxproj and .filters readers, the
# SolutionModel, the trust store and the private path helpers (src/project/src, white-box), label
# "unit". The fixtures under fixtures/ are hand-written, byte-exact (.gitattributes -text keeps the
# BOMs and CRLF line ends) and shaped like a small SDK-generated solution of linked projects.
# The CMake part (ADR 0046): presets on fixtures/cmake/presets, profiles, the File API reply in
# fixtures/cmake/reply (synthetic, shaped like CMake 4.4's, with path placeholders) and the view model.
# project_cmake_tests checks the presets reader against the real cmake (fixtures/cmake/oracle).

frost_add_test(project_unit_tests
    SOURCES
        cmake_compile_database_test.cpp
        cmake_file_api_test.cpp
        cmake_presets_test.cpp
        cmake_profile_test.cpp
        directory_roles_test.cpp
        paths_test.cpp
        sln_reader_test.cpp
        slnx_reader_test.cpp
        solution_model_test.cpp
        trust_store_test.cpp
        vcxproj_reader_test.cpp
    DEPS frost::project)
target_include_directories(project_unit_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/project/src)
target_compile_definitions(project_unit_tests PRIVATE FROST_PROJECT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# The real cmake as the oracle of the presets reader (CMK-2): `cmake --list-presets=all` and
# `cmake --preset` on fixtures/cmake/presets and fixtures/cmake/oracle, with the pinned cmake and
# ninja that configured FrostIDE. Label "integration" with the tag "cmake".
frost_add_test(project_cmake_tests
    SOURCES cmake_presets_oracle_test.cpp
    DEPS frost::project frost::io
    LABELS integration cmake)
target_include_directories(project_cmake_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(project_cmake_tests PRIVATE
    FROST_PROJECT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    FROST_TEST_CMAKE="${CMAKE_COMMAND}"
    FROST_TEST_NINJA="${CMAKE_MAKE_PROGRAM}")
