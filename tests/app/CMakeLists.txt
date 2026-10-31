# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::app and the frost executable:
#   app_tests             unit tests (command line) and offscreen window tests;
#   frost_process_tests   integration tests that run the built frost binary.

frost_add_test(app_tests QT
    SOURCES command_line_test.cpp main_window_test.cpp
    DEPS frost::app)

add_library(frost_test_support STATIC ${FROST_SOURCE_DIR}/tests/support/process_runner.cpp)
target_include_directories(frost_test_support PUBLIC ${FROST_SOURCE_DIR}/tests)
frost_apply_compile_options(frost_test_support TEST)

frost_add_test(frost_process_tests
    SOURCES frost_process_test.cpp
    DEPS frost::core
    THIRD_PARTY frost_test_support
    LABELS integration)
# The tests run the real executable from its build directory.
target_compile_definitions(frost_process_tests PRIVATE FROST_EXE_PATH="$<TARGET_FILE:frost>")
add_dependencies(frost_process_tests frost)
