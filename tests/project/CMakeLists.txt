# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::project (W1, ADR 0032): the .sln, .slnx, .vcxproj and .filters readers, the
# SolutionModel, the trust store and the private path helpers (src/project/src, white-box), label
# "unit". The fixtures under fixtures/ are hand-written, byte-exact (.gitattributes -text keeps the
# BOMs and CRLF line ends) and shaped like a small SDK-generated solution of linked projects.

frost_add_test(project_unit_tests
    SOURCES
        paths_test.cpp
        sln_reader_test.cpp
        slnx_reader_test.cpp
        solution_model_test.cpp
        trust_store_test.cpp
        vcxproj_reader_test.cpp
    DEPS frost::project)
target_include_directories(project_unit_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/project/src)
target_compile_definitions(project_unit_tests PRIVATE FROST_PROJECT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
