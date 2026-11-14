# SPDX-License-Identifier: Apache-2.0
#
# The ctest fixture of the test runner tests (RUN-7; tests/run/test_runner_test.cpp and
# tests/app/unit_tests_window_test.cpp). A project without languages, so configuring it needs only
# cmake and Ninja: its tests run cmake -E itself, a Google Test program the FrostIDE build made
# (FROST_GTEST_FIXTURE, optional) and a process-tree helper (FROST_TREE_PROGRAM, optional). Every
# kind of ctest outcome is here: a pass, a failure, a skip (SKIP_RETURN_CODE), a disabled test, a
# program ctest cannot find, a timeout, and a name full of regular-expression characters.
cmake_minimum_required(VERSION 3.21)
project(CTestFixture NONE)
enable_testing()

add_test(NAME passes COMMAND ${CMAKE_COMMAND} -E true)
add_test(NAME fails COMMAND ${CMAKE_COMMAND} -E false)
add_test(NAME skipped COMMAND ${CMAKE_COMMAND} -E false)
set_tests_properties(skipped PROPERTIES SKIP_RETURN_CODE 1 LABELS "fast;core")
add_test(NAME disabled COMMAND ${CMAKE_COMMAND} -E true)
set_tests_properties(disabled PROPERTIES DISABLED TRUE)
add_test(NAME "spaces (and regex+chars) [x]" COMMAND ${CMAKE_COMMAND} -E echo "output <with> & markup")
add_test(NAME missing COMMAND frost_fixture_program_that_does_not_exist)
add_test(NAME slow COMMAND ${CMAKE_COMMAND} -E sleep 30)
set_tests_properties(slow PROPERTIES TIMEOUT 1)

if(FROST_GTEST_FIXTURE)
    # The whole program as one test, and one of its cases the way gtest_discover_tests registers it.
    add_test(NAME gtest_program COMMAND ${FROST_GTEST_FIXTURE})
    add_test(NAME Fixture.Passes COMMAND ${FROST_GTEST_FIXTURE} --gtest_filter=Fixture.Passes)
    add_test(NAME Fixture.Fails COMMAND ${FROST_GTEST_FIXTURE} --gtest_filter=Fixture.Fails)
endif()
if(FROST_TREE_PROGRAM)
    # A process tree of 1 + 2 + 4 processes that sleeps; only the cancel test runs it.
    add_test(NAME tree COMMAND ${FROST_TREE_PROGRAM} tree 2 2)
    set_tests_properties(tree PROPERTIES LABELS "tree")
endif()
