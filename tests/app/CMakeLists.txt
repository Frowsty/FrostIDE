# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::app and the frost executable:
#   app_tests             unit tests (command line) and offscreen window tests;
#   frost_process_tests   integration tests that run the built frost binary through
#                         frost::io's IProcessLauncher.

frost_add_test(app_tests QT
    SOURCES command_line_test.cpp main_window_test.cpp
    DEPS frost::app)

frost_add_test(frost_process_tests
    SOURCES frost_process_test.cpp
    DEPS frost::io
    LABELS integration)
# tests/support/scratch_dir.hpp.
target_include_directories(frost_process_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
# The tests run the real executable from its build directory.
target_compile_definitions(frost_process_tests PRIVATE FROST_EXE_PATH="$<TARGET_FILE:frost>")
add_dependencies(frost_process_tests frost)
