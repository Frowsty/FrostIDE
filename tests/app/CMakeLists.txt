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
    SOURCES command_line_test.cpp main_window_test.cpp stable_lsp_requester_test.cpp stall_hooks_test.cpp
            workbench_project_helpers_test.cpp
    DEPS frost::app frost::diag frost::io)
# tests/support/scratch_dir.hpp (the persistent services' data directory).
target_include_directories(app_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

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
# The crash tests (ADR 0022): crash_child (tests/diag) for the contract-violation dump, and the
# directory where the self-test artifacts are left for verify -Full's symbolize step.
target_compile_definitions(frost_process_tests PRIVATE
    FROST_CRASH_CHILD="$<TARGET_FILE:crash_child>"
    FROST_SELFTEST_ARTIFACTS="${CMAKE_BINARY_DIR}/selftest-artifacts/$<CONFIG>")
add_dependencies(frost_process_tests crash_child)

# The workbench (W1, ADR 0044) over copies of the build fixtures in a work directory under this
# build tree (not %TEMP%, where MSBuild warns with MSB8029): app_workbench_tests runs offscreen with
# no real tool (label unit); app_workbench_msbuild_tests (Windows) and app_workbench_clangd_tests run
# the real MSBuild and clangd (label integration, so RESOURCE_LOCK frost_desktop, with the tags
# msbuild and clangd; ADR 0027).
set(_frost_workbench_definitions
    FROST_BUILD_FIXTURES="${FROST_SOURCE_DIR}/tests/build/fixtures"
    FROST_APP_WORK_ROOT="${CMAKE_CURRENT_BINARY_DIR}/workbench-work")
frost_add_test(app_workbench_tests QT
    SOURCES workbench_test.cpp workbench_terminal_test.cpp workbench_engine_test.cpp workbench_find_test.cpp project_edit_process_test.cpp update_check_test.cpp update_readiness_test.cpp update_state_test.cpp
    DEPS frost::app frost::build frost::io frost::ui_find
    THIRD_PARTY Qt6::Network unofficial-sodium::sodium)
target_include_directories(app_workbench_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_workbench_tests PRIVATE ${_frost_workbench_definitions})
target_compile_definitions(app_workbench_tests PRIVATE FROST_DISCOVERY_STALL_CHILD="$<TARGET_FILE:engine_discovery_stall_child>")
add_dependencies(app_workbench_tests engine_discovery_stall_child)
# Exercise the release floor across independent processes, including a stale reader.
if(WIN32 AND NOT Python3_EXECUTABLE AND DEFINED ENV{FROST_SDK} AND EXISTS "$ENV{FROST_SDK}/py/Scripts/python.exe")
    set(Python3_EXECUTABLE "$ENV{FROST_SDK}/py/Scripts/python.exe")
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
add_test(NAME UpdateState.IndependentProcessesPreserveHigherFloor
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/update_state_process.py"
            "$<TARGET_FILE:app_workbench_tests>")
set_tests_properties(UpdateState.IndependentProcessesPreserveHigherFloor PROPERTIES
    LABELS integration
    RESOURCE_LOCK frost_desktop
    TIMEOUT 45)
# Real isolated scratch Git repositories, never the user's project or remotes.
file(TO_CMAKE_PATH "${FROST_GIT_EXECUTABLE}" _frost_app_git)
frost_add_test(app_workbench_git_tests QT
    SOURCES workbench_git_test.cpp
    DEPS frost::app frost::ui_vcs frost::vcs frost::io
    LABELS integration git)
target_include_directories(app_workbench_git_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_workbench_git_tests PRIVATE ${_frost_workbench_definitions} FROST_VCS_GIT="${_frost_app_git}")

if(WIN32)
    frost_add_test(app_workbench_msbuild_tests QT
        SOURCES workbench_msbuild_test.cpp workbench_r2a_debug_test.cpp workbench_project_edit_test.cpp
        DEPS frost::app frost::build frost::io
        LABELS integration msbuild)
    target_include_directories(app_workbench_msbuild_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
    target_compile_definitions(app_workbench_msbuild_tests PRIVATE ${_frost_workbench_definitions})
    target_compile_definitions(app_workbench_msbuild_tests PRIVATE
        FROST_PROJECT_HOST="${CMAKE_BINARY_DIR}/src/app/$<CONFIG>/project-host/ProjectHostVsProbe.exe")
    add_dependencies(app_workbench_msbuild_tests frost_project_host)
endif()
frost_add_test(app_workbench_clangd_tests QT
    SOURCES workbench_clangd_test.cpp
    DEPS frost::app frost::lang frost::io
    LABELS integration clangd)
target_include_directories(app_workbench_clangd_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_workbench_clangd_tests PRIVATE ${_frost_workbench_definitions})
if(WIN32)
    add_executable(held_msbuild_fixture held_msbuild_fixture.cpp)
    frost_apply_compile_options(held_msbuild_fixture TEST)
    target_compile_definitions(app_workbench_clangd_tests PRIVATE FROST_HELD_MSBUILD="$<TARGET_FILE:held_msbuild_fixture>")
    add_dependencies(app_workbench_clangd_tests held_msbuild_fixture)
endif()

frost_add_test(app_workbench_lua_tests QT
    SOURCES workbench_lua_test.cpp
    DEPS frost::app frost::lang frost::io
    LABELS integration)
target_include_directories(app_workbench_lua_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_workbench_lua_tests PRIVATE ${_frost_workbench_definitions})
add_executable(structure_lsp_fixture structure_lsp_fixture.cpp)
frost_apply_compile_options(structure_lsp_fixture TEST)
target_link_libraries(structure_lsp_fixture PRIVATE frost::lang)
target_compile_definitions(app_workbench_lua_tests PRIVATE FROST_STRUCTURE_LSP="$<TARGET_FILE:structure_lsp_fixture>")
add_dependencies(app_workbench_lua_tests structure_lsp_fixture)

frost_add_test(app_workbench_cmake_tests QT
    SOURCES workbench_cmake_test.cpp workbench_clangd_spawn_test.cpp
    DEPS frost::app frost::build frost::lang frost::io
    LABELS integration cmake clangd)
target_include_directories(app_workbench_cmake_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_workbench_cmake_tests PRIVATE ${_frost_workbench_definitions}
    FROST_TEST_C_COMPILER="${CMAKE_C_COMPILER}" FROST_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    FROST_TEST_CMAKE="${CMAKE_COMMAND}")

# Run configurations (RUN-2/RUN-3): the CMake executables of a real File API reply (oracle: the
# reply JSON) and their edits reaching the built program, a native configuration run from the
# toolbar whose Stop ends its run_tree_child tree (checked with OS process handles), and the trust
# gate. Real cmake, Ninja and the C compiler, so tag cmake.
frost_add_test(app_workbench_run_tests QT
    SOURCES workbench_run_test.cpp
    DEPS frost::app frost::build frost::run frost::rpc frost::io
    LABELS integration cmake)
target_include_directories(app_workbench_run_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_workbench_run_tests PRIVATE ${_frost_workbench_definitions}
    FROST_TEST_C_COMPILER="${CMAKE_C_COMPILER}"
    FROST_IO_TEST_CHILD="$<TARGET_FILE:io_test_child>"
    FROST_RUN_TREE_CHILD="$<TARGET_FILE:run_tree_child>")
add_dependencies(app_workbench_run_tests io_test_child run_tree_child)

# Attach to Process and its trust prompt (R0 follow-up 1): the pinned lldb-dap attaches to a real
# child; no clangd and no cmake run, so the suite carries no tag.
frost_add_test(app_workbench_attach_tests QT
    SOURCES workbench_attach_test.cpp
    DEPS frost::app frost::build frost::debug frost::io frost::lang
    LABELS integration)
target_include_directories(app_workbench_attach_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_workbench_attach_tests PRIVATE ${_frost_workbench_definitions})

frost_add_test(app_project_open_tests QT
    SOURCES project_open_process_test.cpp
    DEPS frost::app frost::lang frost::io
    LABELS integration)
target_include_directories(app_project_open_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_project_open_tests PRIVATE ${_frost_workbench_definitions} FROST_EXE_PATH="$<TARGET_FILE:frost>")
add_dependencies(app_project_open_tests frost)

frost_add_test(app_workbench_unity_tests QT
    SOURCES workbench_unity_test.cpp
    DEPS frost::app frost::lang frost::io
    LABELS integration)
target_include_directories(app_workbench_unity_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_workbench_unity_tests PRIVATE ${_frost_workbench_definitions})

frost_add_test(app_workbench_managed_tests QT
    SOURCES workbench_managed_test.cpp managed_debug_preparation_test.cpp mono_adapter_preparation_test.cpp
    DEPS frost::app frost::lang frost::io
    LABELS integration)
target_include_directories(app_workbench_managed_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_workbench_managed_tests PRIVATE ${_frost_workbench_definitions})

if(WIN32)
    target_link_libraries(app_workbench_managed_tests PRIVATE shell32 ole32 uuid)
endif()

# The R0 exit criteria (PLAN.md section 7, R0; ADR 0078) over the real tools: tool ablation, Go to
# Definition into a library header that writes nothing into the workspace, untrusted CMake and
# FrostEngine folders that start nothing before trust, and engine helpers beside a running LuaLS
# (R0 follow-up 13). Real cmake, clangd and LuaLS, so label integration (frost_desktop lock).
frost_add_test(app_r0_exit_tests QT
    SOURCES r0_exit_test.cpp
    DEPS frost::app frost::build frost::lang frost::io
    LABELS integration)
target_include_directories(app_r0_exit_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_r0_exit_tests PRIVATE ${_frost_workbench_definitions}
    FROST_TEST_C_COMPILER="${CMAKE_C_COMPILER}")

# The corpus tier (label corpus; tools/verify.ps1 -Corpus and verify.sh --corpus run it, -Quick and
# -Full exclude it): the pinned FrostEngine clone of tests/corpus/corpora.lock.json, which
# tools/corpora.py fetches. The pin and this platform's preset are compiled in from the lock file.
set(_frost_corpora_lock_file "${FROST_SOURCE_DIR}/tests/corpus/corpora.lock.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_frost_corpora_lock_file}")
file(READ "${_frost_corpora_lock_file}" _frost_corpora_lock)
string(JSON _frost_corpora_count LENGTH "${_frost_corpora_lock}" corpora)
set(_frost_engine_sha "")
math(EXPR _frost_corpora_last "${_frost_corpora_count} - 1")
foreach(_frost_index RANGE ${_frost_corpora_last})
    string(JSON _frost_corpus_id GET "${_frost_corpora_lock}" corpora ${_frost_index} id)
    if(_frost_corpus_id STREQUAL "frostengine")
        string(JSON _frost_engine_sha GET "${_frost_corpora_lock}" corpora ${_frost_index} sha)
        if(WIN32)
            string(JSON _frost_engine_preset GET "${_frost_corpora_lock}" corpora ${_frost_index} presets windows)
        else()
            string(JSON _frost_engine_preset GET "${_frost_corpora_lock}" corpora ${_frost_index} presets linux)
        endif()
    endif()
endforeach()
if(NOT _frost_engine_sha MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "${_frost_corpora_lock_file} pins no frostengine corpus")
endif()
frost_add_test(app_corpus_tests QT
    SOURCES corpus_test.cpp corpus_dirs_lua_test.cpp corpus_unit_tests_test.cpp corpus_doctest_test.cpp
    DEPS frost::app frost::build frost::diag frost::lang frost::io frost::vcs frost::search frost::ui_nav
         frost::project frost::document frost::ui_run
    LABELS integration corpus)
target_include_directories(app_corpus_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_corpus_tests PRIVATE ${_frost_workbench_definitions}
    FROST_CORPUS_FROSTENGINE_SHA="${_frost_engine_sha}" FROST_CORPUS_FROSTENGINE_PRESET="${_frost_engine_preset}"
    FROST_TEST_CMAKE="${CMAKE_COMMAND}")

# The automation endpoint (FND-7a, ADR 0080) in this process over a WorkbenchRig: the session token,
# the R0 trust criterion through the endpoint, and a small j1 over a C project with this build's
# cmake, Ninja and compiler (label integration, frost_desktop lock; tags cmake). The endpoint's code
# is in Debug only (FROST_AUTOMATION_ENDPOINT), so the other configurations test its absence.
frost_add_test(app_automation_endpoint_tests QT
    SOURCES automation_endpoint_test.cpp
    DEPS frost::app frost::build frost::io frost::rpc
    THIRD_PARTY Qt6::Network
    LABELS integration cmake)
target_include_directories(app_automation_endpoint_tests PRIVATE ${FROST_SOURCE_DIR}/tests
    ${FROST_SOURCE_DIR}/src/app/src)
target_compile_definitions(app_automation_endpoint_tests PRIVATE ${_frost_workbench_definitions}
    FROST_TEST_C_COMPILER="${CMAKE_C_COMPILER}" FROST_TEST_CMAKE="${CMAKE_COMMAND}")
if(FROST_ENABLE_AUTOMATION_ENDPOINT)
    target_compile_definitions(app_automation_endpoint_tests PRIVATE $<$<CONFIG:Debug>:FROST_AUTOMATION_ENDPOINT=1>)
endif()

# The Unit Tests window (RUN-7) over a trusted copy of tests/run/fixtures/ctest_project that the
# workbench configures with the pinned cmake and Ninja; its ctest is the oracle, and a stub ctest
# (tests/run's run_ctest_stub) gives the tool-error case (label integration, tag cmake).
frost_add_test(app_unit_tests_window_tests QT
    SOURCES unit_tests_window_test.cpp
    DEPS frost::app frost::build frost::io frost::ui_run
    LABELS integration cmake)
target_include_directories(app_unit_tests_window_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_unit_tests_window_tests PRIVATE ${_frost_workbench_definitions}
    FROST_RUN_FIXTURES="${FROST_SOURCE_DIR}/tests/run/fixtures"
    FROST_RUN_CTEST_STUB="$<TARGET_FILE:run_ctest_stub>")
add_dependencies(app_unit_tests_window_tests run_ctest_stub)
