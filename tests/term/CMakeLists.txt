# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::term (ADR 0051):
#   term_unit_tests          the screen model fed recorded VT sequences, the key encoding and the
#                            "terminal keeps these keys" table, the shell choice and the trust rule,
#                            and TerminalSession over the FakePtyLauncher, label "unit";
#   term_integration_tests   a real pseudo terminal (ConPTY with cmd.exe on Windows, /bin/sh on Linux):
#                            echo, resize, Ctrl+C and exit with the tree ended, labels "integration"
#                            and "pty".

frost_add_test(term_unit_tests
    SOURCES
        keys_test.cpp
        pty_buffer_test.cpp
        session_test.cpp
        shell_test.cpp
        vt_screen_test.cpp
    DEPS frost::term)

frost_add_test(term_integration_tests
    SOURCES
        pty_test.cpp
    DEPS frost::term
    LABELS integration pty)

add_executable(term_pty_key_child pty_key_child.cpp)
frost_apply_compile_options(term_pty_key_child TEST)
frost_copy_runtime_dlls(term_pty_key_child)
target_compile_definitions(term_integration_tests PRIVATE FROST_TERM_KEY_CHILD="$<TARGET_FILE:term_pty_key_child>")
add_dependencies(term_integration_tests term_pty_key_child)
if(WIN32)
    frost_deploy_conpty(term_integration_tests)
endif()
