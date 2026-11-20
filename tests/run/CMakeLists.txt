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
    SOURCES engine_session_configurations_test.cpp native_profile_test.cpp native_coverage_test.cpp vs_import_test.cpp run_configuration_test.cpp run_types_test.cpp frostengine_run_test.cpp engine_debug_mode_test.cpp
