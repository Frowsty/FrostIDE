# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::diff (ADR 0019), split by label because labels apply per executable:
#   diff_tests          unit:        LineSequence, histogram goldens and dialect canaries, Myers vs
#                                    the DP oracle, compaction, the apply property, limits,
#                                    refinement, unified goldens, merge goldens and properties,
#                                    the default cap on ordinary files, the reuse paths,
#                                    concurrency (TSan in WSL -Full)
#   diff_git_tests      integration: the differential against `git diff --histogram --no-index`
#                                    (the dialect probe, every generator class, unified bodies,
#                                    git apply of our patches, fault injection of the runner)
#   diff_stress_tests   stress:      the long property, git and merge runs
#   diff_perf_tests     perf:        the derived sub-budgets of ADR 0019 (asserted only in
#                                    optimised, unsanitised builds) and the work-cap tuning numbers
# The white-box tests include the library's private headers (src/diff/src).
add_subdirectory(support)

# git is the oracle of the differential test. Found at configure time; FROST_GIT (an absolute path)
# overrides it at run time. Without git the git tests fail with a message saying how to fix it.
# A given or cached path that is not an existing absolute path is dropped and searched for again
# (find_program never re-checks a cached result), and the path goes into a C string literal in
# CMake's forward-slash form, so a Windows path's backslashes are no escape sequences.
if(FROST_GIT_EXECUTABLE AND (NOT IS_ABSOLUTE "${FROST_GIT_EXECUTABLE}" OR NOT EXISTS "${FROST_GIT_EXECUTABLE}"))
    message(STATUS "FROST_GIT_EXECUTABLE='${FROST_GIT_EXECUTABLE}' is not an existing absolute path; searching again")
    unset(FROST_GIT_EXECUTABLE CACHE)
endif()
find_program(FROST_GIT_EXECUTABLE NAMES git
    DOC "absolute path of git, the oracle of the diff differential test (ADR 0019)")
if(NOT FROST_GIT_EXECUTABLE)
    message(WARNING "git not found: the diff git tests will fail; configure with -DFROST_GIT_EXECUTABLE=<absolute path>")
    set(frost_diff_git "")
else()
    file(TO_CMAKE_PATH "${FROST_GIT_EXECUTABLE}" frost_diff_git)
endif()

frost_add_test(diff_tests
    SOURCES
        cap_test.cpp
        compaction_test.cpp
        concurrency_test.cpp
        histogram_test.cpp
        limits_test.cpp
        line_sequence_test.cpp
        merge_test.cpp
        myers_test.cpp
        property_test.cpp
        refine_test.cpp
        reuse_test.cpp
        unified_test.cpp
    DEPS frost::diff frost_diff_test_support
    LABELS unit)
target_include_directories(diff_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/diff/src)

frost_add_test(diff_git_tests
    SOURCES
        git_test.cpp
        git_test_support.cpp
    DEPS frost::diff frost::io frost_diff_test_support frost_diff_git_support
    LABELS integration)
target_include_directories(diff_git_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(diff_git_tests PRIVATE FROST_DIFF_GIT="${frost_diff_git}")

frost_add_test(diff_stress_tests
    SOURCES
        stress_test.cpp
        git_test_support.cpp
    DEPS frost::diff frost::io frost_diff_test_support frost_diff_git_support
    LABELS stress)
target_include_directories(diff_stress_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(diff_stress_tests PRIVATE FROST_DIFF_GIT="${frost_diff_git}")

frost_add_test(diff_perf_tests
    SOURCES
        perf_test.cpp
    DEPS frost::diff frost_diff_test_support
    LABELS perf)
target_include_directories(diff_perf_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/diff/src)
