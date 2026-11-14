# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::run (RUN-1, RUN-2, RUN-5):
#   run_unit_tests         RunConfiguration validation, the JSON store over a MemFs workspace,
#                          launchPlanFor and displayCommandLine, the RunManager rules that start no
#                          process (refusals, Busy, Prepare, a failed or cancelled before-launch step),
#                          and the generated types (CMake executables, MSBuild applications over a
#                          MemFs solution, native) with RunConfigurationSet's merge and store rules,
#                          label "unit";
#   run_process_tests      RunManager with real processes (io_test_child; output, exit code, argv,
#                          environment, working directory, input, Busy, Stop, a resolving before-launch
#                          handler, a launch failure) and the RUN-1 acceptance: Stop leaves no descendant
#                          of a real seven-process tree (run_tree_child), checked by enumerating the
#                          OS process table, label "integration" (every platform); RUN-5's
#                          before-launch tasks (order, output, a failing task, Stop during a task,
#                          Prepare) with io_test_child as tasks and program;
#   run_tree_child         the helper program that builds that tree (not a test itself).

frost_add_test(run_unit_tests
    SOURCES vs_import_test.cpp run_configuration_test.cpp run_types_test.cpp frostengine_run_test.cpp engine_debug_mode_test.cpp
    DEPS frost::run frost::project frost::rpc frost::io frost::core)
target_include_directories(run_unit_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

include("${FROST_SOURCE_DIR}/tests/support/EngineCorpus.cmake")
frost_add_test(frostengine_run_corpus_tests
    SOURCES frostengine_run_corpus_test.cpp
    DEPS frost::run frost::engine frost::build frost::io frost::rpc frost::core
    LABELS integration corpus)
frost_configure_engine_corpus_target(frostengine_run_corpus_tests)

# A plain console program built with the test flags; only the OS and the C runtime (no frost code),
# so the tree it builds does not depend on the code under test.
add_executable(run_tree_child run_tree_child.cpp)
frost_apply_compile_options(run_tree_child TEST)
frost_copy_runtime_dlls(run_tree_child)

frost_add_test(run_process_tests
    SOURCES run_before_launch_test.cpp run_manager_test.cpp run_manager_tree_test.cpp
    DEPS frost::run frost::io frost::core
    LABELS integration)
# tests/io/process_env.hpp (io_test_child and ProcessWatch), tests/support/scratch_dir.hpp.
target_include_directories(run_process_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(run_process_tests PRIVATE
    FROST_IO_TEST_CHILD="$<TARGET_FILE:io_test_child>"
    FROST_RUN_TREE_CHILD="$<TARGET_FILE:run_tree_child>")
add_dependencies(run_process_tests io_test_child run_tree_child)

# The test runner (RUN-7):
#   run_test_parser_tests  the parsers of ctest's and Google Test's own output (test_parsers.cpp),
#                          over outputs captured from the pinned ctest 4.4.5 and Google Test 1.18,
#                          ctestSetupFor and the selection arguments, label "unit";
#   run_test_runner_tests  TestRunner with the real ctest on fixtures/ctest_project and the real
#                          run_gtest_fixture: the list equals `ctest -N`, outcomes equal ctest's console,
#                          Google Test cases equal the program's own XML report, a selected run, cancel
#                          ends the whole tree (process enumeration), and the stub ctest's error, label
#                          "integration" (tag cmake);
#   run_gtest_fixture      the Google Test program the fixture runs (not a test itself);
#   run_ctest_stub         ctest(.exe) that exits 1 and prints nothing (not a test itself).
#   (RUN-8) run_test_parser_tests also holds doctest_parsers_test.cpp: doctest's command lines and
#                          the parsers of its XML case list, XML report and console failure blocks,
#                          over real doctest 2.5.3 output in fixtures/doctest; run_test_runner_tests
#                          holds the doctest jobs with run_doctest_stub (below).
frost_add_test(run_test_parser_tests
    SOURCES test_parsers_test.cpp doctest_parsers_test.cpp
    DEPS frost::run frost::project frost::io frost::core)
target_include_directories(run_test_parser_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(run_test_parser_tests PRIVATE FROST_RUN_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

add_executable(run_gtest_fixture gtest_fixture.cpp)
frost_apply_compile_options(run_gtest_fixture TEST)
# RUN-10 debugs this fixture's exact case/source line in every IDE configuration. Like the
# dedicated debugger fixtures, keep its line table stable rather than selecting an inlined
# Google Test helper in Release; the IDE and user-project compiler settings remain unchanged.
if(MSVC)
    target_compile_options(run_gtest_fixture PRIVATE /Od /Ob0 /Z7)
    target_link_options(run_gtest_fixture PRIVATE /DEBUG:FULL /INCREMENTAL:NO)
else()
    target_compile_options(run_gtest_fixture PRIVATE -O0 -g)
endif()
target_link_libraries(run_gtest_fixture PRIVATE GTest::gtest GTest::gtest_main)
frost_copy_runtime_dlls(run_gtest_fixture)

add_executable(run_ctest_stub ctest_stub.cpp)
frost_apply_compile_options(run_ctest_stub TEST)
set_target_properties(run_ctest_stub PROPERTIES OUTPUT_NAME ctest
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ctest-stub")
frost_copy_runtime_dlls(run_ctest_stub)

frost_add_test(run_test_runner_tests
    SOURCES test_runner_test.cpp
    DEPS frost::run frost::project frost::rpc frost::io frost::core
    LABELS integration cmake)
target_include_directories(run_test_runner_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
# The tools that configured FrostIDE (the pinned cmake and Ninja) configure the fixture.
target_compile_definitions(run_test_runner_tests PRIVATE
    FROST_RUN_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    FROST_TEST_CMAKE="${CMAKE_COMMAND}"
    FROST_TEST_NINJA="${CMAKE_MAKE_PROGRAM}"
    FROST_RUN_GTEST_FIXTURE="$<TARGET_FILE:run_gtest_fixture>"
    FROST_RUN_CTEST_STUB="$<TARGET_FILE:run_ctest_stub>"
    FROST_RUN_TREE_CHILD="$<TARGET_FILE:run_tree_child>")
add_dependencies(run_test_runner_tests run_gtest_fixture run_ctest_stub run_tree_child)

# The doctest runner (RUN-8):
#   run_doctest_runner_tests  TestRunner's doctest jobs and the doctest part of a ctest run, with the
#                             real ctest on fixtures/doctest_project and run_doctest_stub handing back
#                             captured doctest reports: command lines, working directory and
#                             ENVIRONMENT, listing order, a program without a report, the console
#                             failures of a ctest run and Stop, label "integration" (tag cmake);
#   run_doctest_stub          the stand-in doctest program (not a test itself). The real doctest is
#                             FrostEngine's FrostTests (tests/app/corpus_doctest_test.cpp).
add_executable(run_doctest_stub doctest_stub.cpp)
frost_apply_compile_options(run_doctest_stub TEST)
frost_copy_runtime_dlls(run_doctest_stub)

frost_add_test(run_doctest_runner_tests
    SOURCES doctest_runner_test.cpp
    DEPS frost::run frost::project frost::rpc frost::io frost::core
    LABELS integration cmake)
target_include_directories(run_doctest_runner_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(run_doctest_runner_tests PRIVATE
    FROST_RUN_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    FROST_TEST_CMAKE="${CMAKE_COMMAND}"
    FROST_TEST_NINJA="${CMAKE_MAKE_PROGRAM}"
    FROST_RUN_DOCTEST_STUB="$<TARGET_FILE:run_doctest_stub>")
add_dependencies(run_doctest_runner_tests run_doctest_stub)
