# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::run (RUN-1):
#   run_unit_tests         RunConfiguration validation, the JSON store over a MemFs workspace,
#                          launchPlanFor and displayCommandLine, and the RunManager rules that start no
#                          process (refusals, Busy, Prepare, a failed or cancelled before-launch step),
#                          label "unit";
#   run_process_tests      RunManager with real processes (io_test_child; output, exit code, argv,
#                          environment, working directory, input, Busy, Stop, a resolving before-launch
#                          handler, a launch failure) and the RUN-1 acceptance: Stop leaves no descendant
#                          of a real seven-process tree (run_tree_child), checked by enumerating the
#                          OS process table, label "integration" (every platform);
#   run_tree_child         the helper program that builds that tree (not a test itself).

frost_add_test(run_unit_tests
    SOURCES run_configuration_test.cpp
    DEPS frost::run frost::rpc frost::io frost::core)
target_include_directories(run_unit_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

# A plain console program built with the test flags; only the OS and the C runtime (no frost code),
# so the tree it builds does not depend on the code under test.
add_executable(run_tree_child run_tree_child.cpp)
frost_apply_compile_options(run_tree_child TEST)
frost_copy_runtime_dlls(run_tree_child)

frost_add_test(run_process_tests
    SOURCES run_manager_test.cpp run_manager_tree_test.cpp
    DEPS frost::run frost::io frost::core
    LABELS integration)
# tests/io/process_env.hpp (io_test_child and ProcessWatch), tests/support/scratch_dir.hpp.
target_include_directories(run_process_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(run_process_tests PRIVATE
    FROST_IO_TEST_CHILD="$<TARGET_FILE:io_test_child>"
    FROST_RUN_TREE_CHILD="$<TARGET_FILE:run_tree_child>")
add_dependencies(run_process_tests io_test_child run_tree_child)
