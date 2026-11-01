# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::app and the frost executable:
#   app_tests             unit tests (command line) and offscreen window tests;
#   app_startup_tests     start-up hardening and checks (frost/app/startup.hpp);
#   frost_process_tests   integration tests that run the built frost binary through
#                         frost::io's IProcessLauncher.

frost_add_test(app_tests QT
    SOURCES command_line_test.cpp main_window_test.cpp
    DEPS frost::app)

# Start-up hardening changes process-wide state for good, so it gets its own binary
# (and gtest_discover_tests runs each test in its own process).
frost_add_test(app_startup_tests
    SOURCES startup_test.cpp
    DEPS frost::app frost::io)
# tests/support/scratch_dir.hpp.
target_include_directories(app_startup_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

frost_add_test(frost_process_tests
    SOURCES frost_process_test.cpp
    DEPS frost::io
    LABELS integration)
# tests/support/scratch_dir.hpp.
target_include_directories(frost_process_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
# The tests run the real executable from its build directory.
target_compile_definitions(frost_process_tests PRIVATE FROST_EXE_PATH="$<TARGET_FILE:frost>")
add_dependencies(frost_process_tests frost)
