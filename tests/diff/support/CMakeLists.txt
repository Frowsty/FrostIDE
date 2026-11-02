# SPDX-License-Identifier: Apache-2.0
#
# Support libraries of the frost::diff tests (ADR 0019, D16), plain static libraries with TEST
# options and no gtest:
#   frost_diff_test_support  generators, independent oracles, property checks, golden readers and
#                            file helpers; the fuzz target links it, and tests/fuzz adds this
#                            directory itself in fuzz trees (where only this library is built)
#   frost_diff_git_support   the git differential runner and its batch writer (frost::io); only
#                            diff_git_tests and diff_stress_tests link it, never the fuzz target
add_library(frost_diff_test_support STATIC
    checks.cpp
    files.cpp
    generators.cpp
    goldens.cpp
    oracles.cpp)
target_include_directories(frost_diff_test_support PUBLIC ${FROST_SOURCE_DIR}/tests)
# checks.cpp reaches the library's private engine.hpp for the white-box refinement check.
target_include_directories(frost_diff_test_support PRIVATE ${FROST_SOURCE_DIR}/src/diff/src)
# frost::sched only for the header-only detail::SplitMix64.
target_link_libraries(frost_diff_test_support PUBLIC frost::diff frost::text frost::sched)
target_compile_definitions(frost_diff_test_support PUBLIC FROST_DIFF_TEST_DATA="${FROST_SOURCE_DIR}/tests/diff")
frost_apply_compile_options(frost_diff_test_support TEST)

if(NOT FROST_FUZZ)
    add_library(frost_diff_git_support STATIC
        batch_writer.cpp
        git_runner.cpp)
    target_link_libraries(frost_diff_git_support PUBLIC frost_diff_test_support frost::io)
    frost_apply_compile_options(frost_diff_git_support TEST)
endif()
