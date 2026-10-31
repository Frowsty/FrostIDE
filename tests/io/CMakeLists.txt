# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::io:
#   io_unit_tests          pure logic and the doubles (MemFs, ScriptedProcessLauncher), label "unit";
#   io_integration_tests   the real file system and real processes, label "integration";
#   io_test_child          the helper program the process tests launch (not a test itself).

frost_add_test(io_unit_tests
    SOURCES
        mem_fs_test.cpp
        os_error_test.cpp
        path_test.cpp
        process_spec_test.cpp
        scripted_process_test.cpp
        utf_test.cpp
    DEPS frost::io)
target_include_directories(io_unit_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
if(WIN32)
    # CommandLineToArgvW, the oracle for the Windows quoting tests.
    target_link_libraries(io_unit_tests PRIVATE shell32)
endif()

# A plain console program built with the test flags; it uses only the OS and C runtime so it
# is an independent oracle for argv, environment and working-directory handling.
add_executable(io_test_child io_test_child.cpp)
frost_apply_compile_options(io_test_child TEST)
if(WIN32)
    target_link_libraries(io_test_child PRIVATE shell32)
endif()

frost_add_test(io_integration_tests
    SOURCES
        file_system_test.cpp
        process_test.cpp
    DEPS frost::io
    LABELS integration)
target_include_directories(io_integration_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(io_integration_tests PRIVATE FROST_IO_TEST_CHILD="$<TARGET_FILE:io_test_child>")
add_dependencies(io_integration_tests io_test_child)
