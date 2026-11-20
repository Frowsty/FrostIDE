# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::app and the frost program:
#   app_tests             unit tests (command line) and offscreen window and stall-hook tests;
#   app_startup_tests     start-up hardening, guards and plugin pinning (frost/app/startup.hpp);
#   frost_process_tests   integration tests that run the built frost program (on Windows the
#                         frost.exe launcher and frost_app.dll) through frost::io's
#                         IProcessLauncher;
#   app_workbench_*       the workbench composition (W1): offscreen functional tests, and the
#                         real MSBuild and clangd suites (see the end of this file);
#   app_r0_exit_tests     the R0 exit criteria over the real tools (r0_exit_test.cpp);
#   app_unit_tests_window_tests  the Unit Tests window (RUN-7) over the real ctest;
#   app_corpus_tests      the corpus tier on the pinned FrostEngine clone (label corpus, verify
#                         -Corpus only).
#
# Parallel ctest (frost_add_test's policy; ADR 0027 decision 3): frost_process_tests (label
# integration, so by default) and app_startup_tests (explicitly) hold RESOURCE_LOCK frost_desktop
# with the other integration suites. Their tests
# create private desktops (desktops come from the session's desktop heap), raise MessageBoxW on
# them, start frost.exe with Qt and on the --selftest=onscreen and GUI cases a GPU driver, and
# use the launcher's short links under the per-user LOCALAPPDATA; within one verify run they have
# only ever run one at a time. frost_process_tests also counts as 2 of ctest's -j slots (each case
# runs frost.exe beside the test). app_tests runs offscreen Qt only and takes no lock.

frost_add_test(app_tests QT
# R16 normal .props/.targets navigation plus actual-configured CMK-9 atomic/stale-preview journeys.
frost_add_test(app_workbench_project_language_tests QT
    SOURCES workbench_project_language_test.cpp
    DEPS frost::app frost::lang frost::build frost::io
    LABELS integration cmake)
target_include_directories(app_workbench_project_language_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_workbench_project_language_tests PRIVATE ${_frost_workbench_definitions})

# RUN-12 later genuine instrumented selected-run/action/gutter journey.
frost_add_test(app_workbench_native_coverage_tests QT SOURCES workbench_native_coverage_test.cpp
    DEPS frost::app frost::run frost::lang frost::io frost::sched LABELS integration)
target_include_directories(app_workbench_native_coverage_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/app/src)
target_compile_definitions(app_workbench_native_coverage_tests PRIVATE ${_frost_workbench_definitions}
    FROST_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}")
