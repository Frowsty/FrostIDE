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
            workbench_project_helpers_test.cpp cmake_kit_test.cpp standalone_build_backend_test.cpp
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
    SOURCES workbench_test.cpp workbench_lua_runtime_test.cpp workbench_refactoring_test.cpp workbench_terminal_test.cpp engine_locator_test.cpp engine_native_debug_test.cpp workbench_find_test.cpp project_edit_process_test.cpp update_check_test.cpp update_readiness_test.cpp update_state_test.cpp
            multi_session_debug_test.cpp engine_unit_tests_window_test.cpp
            engine_unit_tests_source_test.cpp engine_run_debug_test.cpp
            engine_dump_controller_test.cpp engine_crash_observer_test.cpp engine_mixed_debug_test.cpp
            workbench_binding_test.cpp
            workbench_engine_mixed_test.cpp
            engine_crash_cleanup_test.cpp engine_native_host_test.cpp engine_tests_lifetime_test.cpp
            source_rename_plan_test.cpp
    DEPS frost::app frost::build frost::io frost::ui_find
    THIRD_PARTY Qt6::Network)
target_include_directories(app_workbench_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/app/src)
target_compile_definitions(app_workbench_tests PRIVATE ${_frost_workbench_definitions})

# FEC-13 drives actual Workbench binding navigation/usages/preview widgets against the locked
# mapper, clangd and LuaLS. Disposable preview application/build evidence lives in the engine target.
include("${FROST_SOURCE_DIR}/tests/support/EngineCorpus.cmake")
frost_add_test(app_binding_corpus_tests QT
    SOURCES workbench_binding_corpus_test.cpp
    DEPS frost::app frost::engine frost::build frost::lang frost::sched frost::text frost::io frost::rpc frost::core
    LABELS integration corpus clangd luals
    TIMEOUT 18000)
frost_configure_engine_corpus_target(app_binding_corpus_tests)
target_include_directories(app_binding_corpus_tests PRIVATE ${FROST_SOURCE_DIR}/src/app/src)
target_compile_definitions(app_binding_corpus_tests PRIVATE ${_frost_workbench_definitions}
    FROST_CLANG_BINARY="$<TARGET_FILE:frost-clang>"
    FROST_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    FROST_BINDING_CORPORA_SCRIPT="${FROST_SOURCE_DIR}/tools/corpora.py"
    FROST_BINDING_REPO_ROOT="${FROST_SOURCE_DIR}"
    FROST_BINDING_CORPUS_ARTIFACTS="${CMAKE_BINARY_DIR}/binding-corpus-artifacts")
add_dependencies(app_binding_corpus_tests frost-clang)

# DBG-10 real lldb sessions use actual source/stack/variable responses, separately from doubles.
frost_add_test(app_multi_session_debug_real_tests QT
    SOURCES multi_session_debug_real_test.cpp
    DEPS frost::app frost::ui_debug frost::debug frost::io
    LABELS integration lldb)
target_include_directories(app_multi_session_debug_real_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_multi_session_debug_real_tests PRIVATE ${_frost_workbench_definitions}
    FROST_SCENARIOS_PATH="$<TARGET_FILE:debug_test_scenarios>"
    FROST_SCENARIOS_SOURCE="${FROST_SOURCE_DIR}/tests/debug/fixtures/dap_scenarios.cpp")
add_dependencies(app_multi_session_debug_real_tests debug_test_scenarios)
# Persistent engine regressions use real loopback sockets; serialize them with the other
# integration cases. Filesystem-only discovery and protocol doubles remain in the unit target.
frost_add_test(app_engine_session_tests QT
    SOURCES workbench_engine_test.cpp workbench_engine_lua_debug_test.cpp
    DEPS frost::app frost::build frost::io frost::ui_find
    THIRD_PARTY Qt6::Network unofficial-sodium::sodium
    LABELS integration)
target_include_directories(app_engine_session_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/app/src)
target_compile_definitions(app_engine_session_tests PRIVATE ${_frost_workbench_definitions})
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
        SOURCES workbench_msbuild_test.cpp workbench_project_edit_test.cpp
                workbench_project_host_test.cpp
        DEPS frost::app frost::build frost::io
        LABELS integration msbuild)
    # src/project/src: the MSBuild oracle (tests/project/msbuild_oracle.hpp) reads JSON with project's parser.
    target_include_directories(app_workbench_msbuild_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/project/src)
    target_compile_definitions(app_workbench_msbuild_tests PRIVATE ${_frost_workbench_definitions})
    target_compile_definitions(app_workbench_msbuild_tests PRIVATE
        FROST_PROJECT_HOST="${CMAKE_BINARY_DIR}/src/app/$<CONFIG>/project-host/ProjectHostVsProbe.exe")
    add_dependencies(app_workbench_msbuild_tests frost_project_host)
    # The real R2a debugger acceptance provisions its disposable pinned clone in Corpus.
    set(_frost_r2a_lock_file "${FROST_SOURCE_DIR}/tests/corpus/corpora.lock.json")
    set(_frost_r2a_sdk_file "${FROST_SOURCE_DIR}/sdk/sdk.lock.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_frost_r2a_lock_file}" "${_frost_r2a_sdk_file}")
    file(READ "${_frost_r2a_lock_file}" _frost_r2a_lock)
    string(JSON _frost_r2a_count LENGTH "${_frost_r2a_lock}" corpora)
    math(EXPR _frost_r2a_last "${_frost_r2a_count} - 1")
    set(_frost_r2a_sha "")
    foreach(_frost_r2a_index RANGE ${_frost_r2a_last})
        string(JSON _frost_r2a_id GET "${_frost_r2a_lock}" corpora ${_frost_r2a_index} id)
        if(_frost_r2a_id STREQUAL "r2a")
            string(JSON _frost_r2a_sha GET "${_frost_r2a_lock}" corpora ${_frost_r2a_index} sha)
        endif()
    endforeach()
    if(NOT _frost_r2a_sha MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "${_frost_r2a_lock_file} pins no R2a corpus")
    endif()
    file(READ "${_frost_r2a_sdk_file}" _frost_r2a_sdk)
    string(JSON _frost_r2a_vc GET "${_frost_r2a_sdk}" msvc vcToolsVersion)
    string(JSON _frost_r2a_winsdk GET "${_frost_r2a_sdk}" msvc windowsSdkVersion)
    file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" _frost_r2a_python)
    frost_add_test(app_r2a_debug_corpus_tests QT
        SOURCES workbench_r2a_debug_test.cpp
        DEPS frost::app frost::build frost::io frost::vcs
        LABELS integration corpus msbuild)
    target_include_directories(app_r2a_debug_corpus_tests PRIVATE ${FROST_SOURCE_DIR}/tests
        ${FROST_SOURCE_DIR}/src/project/src)
    target_link_libraries(app_r2a_debug_corpus_tests PRIVATE user32)
    target_compile_definitions(app_r2a_debug_corpus_tests PRIVATE ${_frost_workbench_definitions}
        FROST_PROJECT_HOST="${CMAKE_BINARY_DIR}/src/app/$<CONFIG>/project-host/ProjectHostVsProbe.exe"
        FROST_CORPUS_R2A_SHA="${_frost_r2a_sha}"
        FROST_PINNED_VC_TOOLS="${_frost_r2a_vc}" FROST_PINNED_WINSDK="${_frost_r2a_winsdk}"
        FROST_TEST_PYTHON="${_frost_r2a_python}"
        FROST_R2A_CORPORA_TOOL="${FROST_SOURCE_DIR}/tools/corpora.py")
    add_dependencies(app_r2a_debug_corpus_tests frost_project_host)
    # SLN-7: the Configuration Manager switches and remaps configurations; clangd follows on
    # GetClCommandLines (SLN-5) from the shipped ProjectHost. Tags "msbuild" and "clangd".
    frost_add_test(app_workbench_configuration_tests QT
        SOURCES workbench_configuration_manager_test.cpp
        DEPS frost::app frost::build frost::lang frost::io
        LABELS integration msbuild clangd)
    target_include_directories(app_workbench_configuration_tests PRIVATE ${FROST_SOURCE_DIR}/tests
                               ${FROST_SOURCE_DIR}/src/project/src)
    target_compile_definitions(app_workbench_configuration_tests PRIVATE ${_frost_workbench_definitions}
        FROST_PROJECT_HOST="${CMAKE_BINARY_DIR}/src/app/$<CONFIG>/project-host/ProjectHostVsProbe.exe")
    add_dependencies(app_workbench_configuration_tests frost_project_host)
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

# CPP-5 and REF-12 with the real clangd and clang-format (label integration, tag clangd): clangd's
# links, folds, inlay hints and AST in the active editor, and the actions on save.
frost_add_test(app_workbench_cpp_editor_tests QT
    SOURCES workbench_cpp_editor_test.cpp workbench_inspection_test.cpp
    DEPS frost::app frost::lang frost::io frost::sched
    LABELS integration clangd)
target_include_directories(app_workbench_cpp_editor_tests PRIVATE ${FROST_SOURCE_DIR}/tests
    ${FROST_SOURCE_DIR}/src/app/src)
target_compile_definitions(app_workbench_cpp_editor_tests PRIVATE ${_frost_workbench_definitions})

# R6 uses mandatory isolated real engines; these cases belong only to the corpus tier.
include("${FROST_SOURCE_DIR}/tests/support/EngineCorpus.cmake")
frost_add_test(app_engine_session_acceptance_tests QT
    SOURCES engine_session_acceptance_test.cpp
    DEPS frost::app frost::engine frost::build frost::rpc frost::io frost::sched
    LABELS integration corpus)
frost_configure_engine_corpus_target(app_engine_session_acceptance_tests)
target_include_directories(app_engine_session_acceptance_tests PRIVATE ${FROST_SOURCE_DIR}/src/app/src)
target_compile_definitions(app_engine_session_acceptance_tests PRIVATE ${_frost_workbench_definitions})
if(WIN32)
    # FER-16 composed acceptance launches the actual read-only dump adapter, never a scripted peer.
    target_compile_definitions(app_engine_session_acceptance_tests PRIVATE
        FROST_DBGENG_ADAPTER_PATH="$<TARGET_FILE:frost-dbgeng-dap>")
    add_dependencies(app_engine_session_acceptance_tests frost-dbgeng-dap)
endif()

frost_add_test(app_workbench_lua_tests QT
    SOURCES workbench_lua_test.cpp workbench_lua_workspace_test.cpp
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
# RUN-5: an attach configuration attaches to a uniquely named io_test_child copy, and Debug runs a
# native configuration's before-launch tasks (io_test_child) first.
frost_add_test(app_workbench_attach_tests QT
    SOURCES workbench_attach_test.cpp
    DEPS frost::app frost::build frost::debug frost::io frost::lang frost::run
    LABELS integration)
target_include_directories(app_workbench_attach_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(app_workbench_attach_tests PRIVATE ${_frost_workbench_definitions}
    FROST_IO_TEST_CHILD="$<TARGET_FILE:io_test_child>")
add_dependencies(app_workbench_attach_tests io_test_child)

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

include("${FROST_SOURCE_DIR}/tests/support/ManagedFixtures.cmake")
frost_add_managed_fixture_test(app_workbench_managed_tests QT
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
            corpus_debug_test.cpp corpus_cpp_editor_test.cpp
    DEPS frost::app frost::build frost::diag frost::lang frost::io frost::vcs frost::search frost::ui_nav
         frost::project frost::document frost::ui_run frost::debug
    LABELS integration corpus)
target_include_directories(app_corpus_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/app/src)
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
    DEPS frost::app frost::build frost::debug frost::io frost::ui_run
    LABELS integration cmake lldb)
target_include_directories(app_unit_tests_window_tests PRIVATE ${FROST_SOURCE_DIR}/tests
    ${FROST_SOURCE_DIR}/src/app/src)
target_compile_definitions(app_unit_tests_window_tests PRIVATE ${_frost_workbench_definitions}
    FROST_RUN_FIXTURES="${FROST_SOURCE_DIR}/tests/run/fixtures"
    FROST_RUN_CTEST_STUB="$<TARGET_FILE:run_ctest_stub>"
    FROST_RUN_GTEST_FIXTURE="$<TARGET_FILE:run_gtest_fixture>"
    FROST_RUN_GTEST_SOURCE="${FROST_SOURCE_DIR}/tests/run/gtest_fixture.cpp")
add_dependencies(app_unit_tests_window_tests run_ctest_stub run_gtest_fixture)

if(FROST_BUILD_FRAMEWORK_ACCEPTANCE)
    target_compile_definitions(app_unit_tests_window_tests PRIVATE FROST_RUN_CATCH_FIXTURE="$<TARGET_FILE:run_catch_fixture>")
    add_dependencies(app_unit_tests_window_tests run_catch_fixture)
endif()
