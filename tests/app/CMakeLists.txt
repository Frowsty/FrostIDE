# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::app and the frost program:
#   app_tests             unit tests (command line) and offscreen window tests;
#   app_startup_tests     start-up hardening, guards and plugin pinning (frost/app/startup.hpp);
#   frost_process_tests   integration tests that run the built frost program (on Windows the
#                         frost.exe launcher and frost_app.dll) through frost::io's
#                         IProcessLauncher.
#
# Parallel ctest (frost_add_test's policy; ADR 0027 decision 3): app_startup_tests and
# frost_process_tests hold RESOURCE_LOCK frost_desktop with io_integration_tests. Their tests
# create private desktops (desktops come from the session's desktop heap), raise MessageBoxW on
# them, start frost.exe with Qt and on the --selftest=onscreen and GUI cases a GPU driver, and
# use the launcher's short links under the per-user LOCALAPPDATA; within one verify run they have
# only ever run one at a time. frost_process_tests also counts as 2 of ctest's -j slots (each case
# runs frost.exe beside the test). app_tests runs offscreen Qt only and takes no lock.

frost_add_test(app_tests QT
    SOURCES command_line_test.cpp main_window_test.cpp
    DEPS frost::app)

# Most of start-up hardening cannot be undone (startup.hpp), so StartupHardeningDeathTest
# applies it only in a death-test child of this binary, which needs no QApplication (no Qt
# test main).
frost_add_test(app_startup_tests
    SOURCES startup_test.cpp
    DEPS frost::app frost::io
    RESOURCE_LOCK frost_desktop)
# tests/support/scratch_dir.hpp.
target_include_directories(app_startup_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
if(WIN32)
    # The SDK's own platform plugin, copied to a long scratch path by
    # StartupLoadPlatformPlugin.PinnedPluginSatisfiesQtsPlainPathLoad; user32 for the
    # dialog-guard death test (its private desktop, MessageBoxW); advapi32 for the token query
    # that StartupElevation compares with frost's own.
    target_compile_definitions(app_startup_tests PRIVATE FROST_QT_PLUGIN_DIR="${FROST_QT_PLUGIN_DIR}")
    target_link_libraries(app_startup_tests PRIVATE user32 advapi32)
endif()

frost_add_test(frost_process_tests
    SOURCES frost_process_test.cpp
    DEPS frost::io
    LABELS integration
    RESOURCE_LOCK frost_desktop
    PROCESSORS 2)
# tests/support/scratch_dir.hpp and private_desktop.hpp.
target_include_directories(frost_process_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
if(WIN32)
    # tests/support/private_desktop.hpp: every frost child runs on a private desktop. The Qt
    # SDK's DLL and plugin directories supply files frost does not deploy (Qt6Network, qsvg)
    # for the late-load and plugin-preload cases.
    target_link_libraries(frost_process_tests PRIVATE user32)
    target_compile_definitions(frost_process_tests PRIVATE
        FROST_QT_BIN_DIR="${FROST_QT_BIN_DIR}" FROST_QT_PLUGIN_DIR="${FROST_QT_PLUGIN_DIR}")

    # A stand-in for frost_app.dll (launcher_probe_app.cpp) that reports the launcher's error
    # mode and restart and can echo stdin with a chosen exit status, for
    # FrostProcess.LauncherSetsTheErrorModeForItsLoads and the restart tests. Built like the
    # launcher: the static release CRT, no sanitizer and no manifest, so it imports
    # KERNEL32.dll alone.
    add_library(frost_launcher_probe_app SHARED launcher_probe_app.cpp)
    frost_apply_compile_options(frost_launcher_probe_app TEST NO_SANITIZE)
    set_target_properties(frost_launcher_probe_app PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
    target_link_options(frost_launcher_probe_app PRIVATE /MANIFEST:NO)
    target_compile_definitions(frost_process_tests PRIVATE
        FROST_LAUNCHER_PROBE_APP="$<TARGET_FILE:frost_launcher_probe_app>")
    add_dependencies(frost_process_tests frost_launcher_probe_app)
endif()
# The tests run the real program from its build directory (on Windows frost.exe, with
# frost_app.dll beside it).
target_compile_definitions(frost_process_tests PRIVATE FROST_EXE_PATH="$<TARGET_FILE:frost>")
add_dependencies(frost_process_tests frost)
