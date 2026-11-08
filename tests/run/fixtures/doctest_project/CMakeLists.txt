# SPDX-License-Identifier: Apache-2.0
#
# The ctest fixture of the doctest runner tests (RUN-8; tests/run/doctest_runner_test.cpp). A project
# without languages: its tests are FrostEngine-style per-suite doctest tests (`--no-intro=true
# --test-suite=<suite>`) of run_doctest_stub (FROST_DOCTEST_STUB), which hands back the captured real
# doctest reports in FROST_DOCTEST_FIXTURES (tests/run/fixtures/doctest), and one plain failing test.
cmake_minimum_required(VERSION 3.21)
project(DoctestFixture NONE)
enable_testing()

set(fixtures "${FROST_DOCTEST_FIXTURES}")
set(work "${CMAKE_CURRENT_BINARY_DIR}/work")
file(MAKE_DIRECTORY "${work}")

# A suite with failing cases: its console output, list and report are doctest's for the Math suite.
add_test(NAME Doctest.Math COMMAND ${FROST_DOCTEST_STUB} --no-intro=true --test-suite=Math
    "--stub-list=${fixtures}/list_math.xml" "--stub-run=${fixtures}/run_math.xml"
    "--stub-console=${fixtures}/console_math.txt" --stub-exit=1)
set_tests_properties(Doctest.Math PROPERTIES WORKING_DIRECTORY "${work}"
    ENVIRONMENT "FROST_DOCTEST_STUB_ENV=from the ENVIRONMENT property")
# A suite whose program exits during a case (std::_Exit(3)): its report is cut short.
add_test(NAME Doctest.Exit COMMAND ${FROST_DOCTEST_STUB} --no-intro=true --test-suite=Exit
    "--stub-list=${fixtures}/list.xml" "--stub-run=${fixtures}/run_exit.xml" --stub-exit=3)
# A program that writes no report and exits 2.
add_test(NAME Doctest.NoReport COMMAND ${FROST_DOCTEST_STUB} --no-intro=true --test-suite=None --stub-exit=2)
# A suite that takes a minute; only the cancel test runs it.
add_test(NAME Doctest.Slow COMMAND ${FROST_DOCTEST_STUB} --no-intro=true --test-suite=Slow --stub-sleep-ms=60000
    "--stub-list=${fixtures}/list_math.xml" "--stub-run=${fixtures}/run_one.xml")
set_tests_properties(Doctest.Slow PROPERTIES LABELS "slow")
add_test(NAME plain_fails COMMAND ${CMAKE_COMMAND} -E false)
