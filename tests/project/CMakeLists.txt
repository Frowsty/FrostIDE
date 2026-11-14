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
        cl_command_lines_test.cpp
        cmake_profile_test.cpp
        directory_roles_test.cpp
        paths_test.cpp
        project_edit_test.cpp
        project_edit_plan_test.cpp
        project_host_protocol_test.cpp
        project_model_cache_test.cpp
        sln_reader_test.cpp
        standalone_project_test.cpp
        slnx_reader_test.cpp
        solution_configuration_edit_test.cpp
        solution_model_test.cpp
        trust_store_test.cpp
        vcxproj_reader_test.cpp
        vcxproj_rename_test.cpp
    DEPS frost::project)
target_include_directories(project_unit_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/project/src)
target_compile_definitions(project_unit_tests PRIVATE FROST_PROJECT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# The real cmake as the oracle of the presets reader (CMK-2): `cmake --list-presets=all` and
# `cmake --preset` on fixtures/cmake/presets and fixtures/cmake/oracle, with the pinned cmake and
# ninja that configured FrostIDE. Label "integration" with the tag "cmake".
frost_add_test(project_cmake_tests
    SOURCES cmake_presets_oracle_test.cpp cmake_membership_oracle_test.cpp
    DEPS frost::project frost::io
    LABELS integration cmake)
target_include_directories(project_cmake_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(project_cmake_tests PRIVATE
    FROST_PROJECT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    FROST_TEST_CMAKE="${CMAKE_COMMAND}"
    FROST_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    FROST_TEST_NINJA="${CMAKE_MAKE_PROGRAM}")

# SLN-0, SLN-2 and SLN-3 against this machine's real MSBuild (Visual Studio or its Build Tools,
# found with vswhere): the oracle (msbuild_oracle.hpp, MSBuild.exe run by the test), the shipped
# ProjectHost helper and SolutionEvaluation on fixtures/msvc_generated, the synthetic generated
# solution (.sln and .slnx). SLN-4, SLN-5 and SLN-7 (cl_command_lines_real_test.cpp): the
# GetClCommandLines oracle, the ProjectModelCache's warm open and an edited mapping built by
# MSBuild.exe. Windows only; label "integration" with the tag "msbuild". A machine without Visual
# Studio skips the tests. PROCESSORS 2: each test runs MSBuild next to the helper.
if(WIN32)
    frost_add_test(project_msbuild_tests
        SOURCES project_host_test.cpp cl_command_lines_real_test.cpp
        DEPS frost::project frost::build frost::io
        LABELS integration msbuild
        PROCESSORS 2)
    target_include_directories(project_msbuild_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/project/src)
    target_compile_definitions(project_msbuild_tests PRIVATE
        FROST_PROJECT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        FROST_PROJECT_HOST="${CMAKE_BINARY_DIR}/src/app/$<CONFIG>/project-host/ProjectHostVsProbe.exe")
    add_dependencies(project_msbuild_tests frost_project_host)

    # SLN-6 (project_edit.hpp): the shipped ProjectHost edits a scratch git copy of
    # fixtures/msvc_generated; git diff, MSBuild's evaluation and MSBuild's build are the oracles.
    frost_add_test(project_edit_msbuild_tests
        SOURCES project_edit_real_test.cpp
        DEPS frost::project frost::build frost::io frost::vcs
        LABELS integration msbuild
        PROCESSORS 2)
    target_include_directories(project_edit_msbuild_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/project/src)
    target_compile_definitions(project_edit_msbuild_tests PRIVATE
        FROST_PROJECT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        FROST_PROJECT_HOST="${CMAKE_BINARY_DIR}/src/app/$<CONFIG>/project-host/ProjectHostVsProbe.exe")
    add_dependencies(project_edit_msbuild_tests frost_project_host)
endif()

# SLN-2, SLN-3 and SLN-6 (edits in a scratch clone) on R2a, the pinned Notepad++ clone (tests/corpus/corpora.lock.json; its sha
# names the clone <corpora root>/r2a-<sha12>): label "corpus", so only verify -Corpus runs it.
if(WIN32)
    set(_frost_project_corpora_lock_file "${FROST_SOURCE_DIR}/tests/corpus/corpora.lock.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_frost_project_corpora_lock_file}")
    file(READ "${_frost_project_corpora_lock_file}" _frost_project_corpora_lock)
    string(JSON _frost_project_corpora_count LENGTH "${_frost_project_corpora_lock}" corpora)
    math(EXPR _frost_project_corpora_last "${_frost_project_corpora_count} - 1")
    set(_frost_r2a_sha "")
    foreach(_frost_project_index RANGE ${_frost_project_corpora_last})
        string(JSON _frost_project_id GET "${_frost_project_corpora_lock}" corpora ${_frost_project_index} id)
        if(_frost_project_id STREQUAL "r2a")
            string(JSON _frost_r2a_sha GET "${_frost_project_corpora_lock}" corpora ${_frost_project_index} sha)
        endif()
    endforeach()
    if(NOT _frost_r2a_sha MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "${_frost_project_corpora_lock_file} pins no r2a corpus")
    endif()
    frost_add_test(project_corpus_tests
        SOURCES project_corpus_test.cpp project_edit_corpus_test.cpp
        DEPS frost::project frost::build frost::io frost::vcs
        LABELS integration corpus msbuild
        PROCESSORS 2)
    target_include_directories(project_corpus_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/project/src)
    target_compile_definitions(project_corpus_tests PRIVATE
        FROST_CORPUS_R2A_SHA="${_frost_r2a_sha}"
        FROST_PROJECT_HOST="${CMAKE_BINARY_DIR}/src/app/$<CONFIG>/project-host/ProjectHostVsProbe.exe")
    add_dependencies(project_corpus_tests frost_project_host)
endif()
