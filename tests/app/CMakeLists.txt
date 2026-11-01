# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::app and the frost program:
#   app_tests             unit tests (command line) and offscreen window tests;
#   app_startup_tests     start-up hardening, guards and plugin pinning (frost/app/startup.hpp);
#   frost_process_tests   integration tests that run the built frost program (on Windows the
#                         frost.exe launcher and frost_app.dll) through frost::io's
#                         IProcessLauncher.

frost_add_test(app_tests QT
    SOURCES command_line_test.cpp main_window_test.cpp
    DEPS frost::app)

# Most of start-up hardening cannot be undone (startup.hpp), so StartupHardeningDeathTest
# applies it only in a death-test child of this binary, which needs no QApplication (no Qt
# test main).
frost_add_test(app_startup_tests
    SOURCES startup_test.cpp
    DEPS frost::app frost::io)
# tests/support/scratch_dir.hpp.
target_include_directories(app_startup_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
if(WIN32)
    # The SDK's own platform plugin, copied to a long scratch path by
    # StartupLoadPlatformPlugin.PinnedPluginSatisfiesQtsPlainPathLoad; and user32 for the
    # dialog-guard death test (its private desktop, MessageBoxW).
    target_compile_definitions(app_startup_tests PRIVATE FROST_QT_PLUGIN_DIR="${FROST_QT_PLUGIN_DIR}")
    target_link_libraries(app_startup_tests PRIVATE user32)
endif()

frost_add_test(frost_process_tests
    SOURCES frost_process_test.cpp
    DEPS frost::io
    LABELS integration)
# tests/support/scratch_dir.hpp and private_desktop.hpp.
target_include_directories(frost_process_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
if(WIN32)
    # tests/support/private_desktop.hpp: every frost child runs on a private desktop. The Qt
    # SDK's DLL and plugin directories supply files frost does not deploy (Qt6Network, qsvg)
    # for the late-load and plugin-preload cases.
    target_link_libraries(frost_process_tests PRIVATE user32)
    target_compile_definitions(frost_process_tests PRIVATE
        FROST_QT_BIN_DIR="${FROST_QT_BIN_DIR}" FROST_QT_PLUGIN_DIR="${FROST_QT_PLUGIN_DIR}")
endif()
# The tests run the real program from its build directory (on Windows frost.exe, with
# frost_app.dll beside it).
target_compile_definitions(frost_process_tests PRIVATE FROST_EXE_PATH="$<TARGET_FILE:frost>")
add_dependencies(frost_process_tests frost)
