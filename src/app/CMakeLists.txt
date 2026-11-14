# SPDX-License-Identifier: Apache-2.0
#
# frost::app (the application library: command line, start-up hardening and checks,
# diagnostics wiring, the one composition root (frost::app::Workbench, ADR 0044) and the main window
# over frost::ui_shell (ADR 0038) with the navigation workbench (ADR 0042), self-test) and the FrostIDE
# program built from it. The library exists so tests can exercise the same code the program runs.
#
# On Windows the program is two images (ADR 0012):
#   frost.exe       the launcher (src/launcher.cpp): static CRT in every configuration, no
#                   sanitizer, KERNEL32.dll as its only import, the process manifest and the
#                   version resource. It hardens the process and loads frost_app.dll through
#                   its \\?\ path, so frost starts from a directory of any length; from an
#                   image path of MAX_PATH or more, a start that may load a GPU driver is
#                   restarted through a short junction under %LOCALAPPDATA%\FrostIDE\i;
#   frost_app.dll   the application (src/main.cpp's frost_main and frost::app), target
#                   frost_app_shared.
# The Qt-free start-up core (src/startup_core.cpp) is compiled into both: one static library
# cannot serve the /MT launcher and the /MD application. On Linux frost is one executable.

set(frost_startup_core_sources include/frost/app/startup_core.hpp src/startup_core.cpp)

# The automation endpoint (src/automation_endpoint.hpp; FND-7a, ADR 0080): with this ON, Debug
# builds define FROST_AUTOMATION_ENDPOINT, and frost listens on a local socket when
# FROST_AUTOMATION_CONNECTION names a connection file. Release and the other configurations compile
# only its "not available" stub; tests/tools/test_automation_release_probe.py checks that the
# Release image holds no trace of the protocol.
option(FROST_ENABLE_AUTOMATION_ENDPOINT "Debug frost has the test automation endpoint (test builds)"
       ${FROST_BUILD_TESTS})
set(frost_automation_definitions "")
if(FROST_ENABLE_AUTOMATION_ENDPOINT)
    set(frost_automation_definitions $<$<CONFIG:Debug>:FROST_AUTOMATION_ENDPOINT=1>)
endif()

frost_add_library(app LAYER app
    SOURCES
        include/frost/app/diagnostics_qt.hpp
        include/frost/app/document_editor.hpp
        include/frost/app/main_window.hpp
        include/frost/app/settings_adapters.hpp
        include/frost/app/welcome_screen.hpp
        include/frost/app/workbench.hpp
        src/automation_endpoint.cpp
        src/automation_endpoint.hpp
        src/command_line.cpp
        src/cmake_build_backend.hpp
        src/standalone_build_backend.hpp
        src/standalone_build_backend.cpp
        src/cmake_integration.hpp
        src/cmake_integration.cpp
        src/cmake_kit_dialog.cpp
        src/workbench_dependencies.cpp
        src/engine_integration.cpp
        src/engine_integration.hpp
        src/engine_integration_internal.hpp
        src/engine_integration_session.cpp
        src/engine_integration_events.cpp
        src/engine_integration_commands.cpp
        src/engine_integration_run.cpp
        src/engine_integration_run_debug.cpp
        src/engine_integration_native_pause.cpp
        src/engine_integration_native_host.cpp
        src/engine_native_host.hpp
        src/engine_native_host_internal.hpp
        src/engine_mixed_launch.cpp
        src/engine_mixed_launch.hpp
        src/engine_integration_crash.cpp
        src/engine_integration_tests.cpp
        src/engine_dump_controller.cpp
        src/engine_dump_controller.hpp
        src/engine_crash_observer.cpp
        src/engine_crash_observer.hpp
        src/engine_crash_cleanup.cpp
        src/engine_binding_integration.cpp
        src/engine_binding_integration.hpp
        src/engine_binding_work.hpp
        src/engine_binding_capture.cpp
        src/engine_binding_query.cpp
        src/engine_run_debug.cpp
        src/engine_run_debug.hpp
        src/engine_unit_tests_window.cpp
        src/engine_unit_tests_window.hpp
        src/engine_test_retirement.cpp
        src/multi_session_debug.cpp
        src/multi_session_debug.hpp
        src/engine_locator.cpp
        src/engine_locator.hpp
        src/update_check.cpp
        src/update_state.cpp
        src/update_state.hpp
        src/update_check.hpp
        src/update_download.cpp
        src/update_download.hpp
        src/unity_control.cpp
        src/unity_control.hpp
        src/git_integration.cpp
        src/git_integration.hpp
        src/engine_request_helper.cpp
        src/engine_request_helper_session.cpp
        src/engine_request_helper.hpp
        src/project_open_broker.cpp
        src/project_open_broker.hpp
        src/project_properties_dialog.cpp
        src/project_properties_dialog.hpp
        src/project_run_dialogs.cpp
        src/project_tool_dialogs.cpp
        src/project_unit_tests.cpp
        src/lua_unit_tests_window.cpp
        src/lua_unit_tests_window.hpp
        src/unit_tests_window.cpp
        src/unit_tests_window.hpp
        src/project_properties_xml.cpp
        src/diagnostics_qt.cpp
        src/document_editor.cpp
        src/main_window.cpp
        src/selftest.cpp
        src/settings_dialog.cpp
        src/settings_dialog.hpp
        src/settings_keymap_page.cpp
        src/settings_keymap_page.hpp
        src/settings_lua_page.cpp
        src/settings_lua_page.hpp
        src/settings_adapters.cpp
        src/startup.cpp
        src/welcome_screen.cpp
        src/workbench.cpp
        src/git_clone.hpp
        src/workbench_actions.cpp
        src/workbench_attach.cpp
        src/workbench_clone.cpp
        src/workbench_prompts.cpp
        src/workbench_run.cpp
        src/workbench_lua_runtime.cpp
        src/workbench_visualizers.cpp
        src/workbench_multi_debug.cpp
        src/workbench_run_configurations.cpp
        src/workbench_tool_windows.cpp
        src/workbench_update.cpp
        src/language_bridge.hpp
        src/stable_lsp_requester.hpp
        src/workbench_documents.cpp
        src/workbench_engine.cpp
        src/workbench_engine_debug.cpp
        src/workbench_engine_lua_debug.cpp
        src/workbench_engine_lua_debug.hpp
        src/workbench_bindings.cpp
        src/engine_mixed_debug.hpp
        src/workbench_mixed_debug.cpp
        src/engine_native_debug.hpp
        src/workbench_language.cpp
        src/workbench_refactoring.cpp
        src/workbench_cpp_rename.cpp
        src/workbench_cpp_refactoring.cpp
        src/workbench_cpp_move.cpp
        src/workbench_cpp_published_fixes.cpp
        src/workbench_save_actions.cpp
        src/workbench_lua.cpp
        src/workbench_lua_refactoring.cpp
        src/workbench_lua_module.cpp
        src/workbench_inspection.cpp
        src/workbench_cpp_scope_fixes.cpp
        src/workbench_managed.cpp
        src/managed_debug_preparation.cpp
        src/managed_source_snapshot.cpp
        src/mono_adapter_preparation.cpp
        src/mono_adapter_preparation.hpp
        src/mono_source_admission.cpp
        src/mono_source_admission.hpp
        src/mono_held_capture.cpp
        src/mono_held_capture.hpp
        src/workbench_managed_debug.cpp
        src/project_edit_process.cpp
        src/workbench_project_edit.cpp
        src/unity_context.hpp
        src/workbench_project.cpp
        src/workbench_project_helpers.hpp
        src/workbench_new_project.cpp
        src/workbench_templates.cpp
        src/workbench_template_dialogs.cpp
        src/workbench_project_sources.cpp
        src/workbench_source_rename.cpp
        src/workbench_source_rename_integration.cpp
        src/workbench_source_rename_preview.cpp
        src/workbench_file_ops.cpp
        src/workbench_clangd.cpp
        src/workbench_navigation_bar.cpp
        src/workbench_terminal.cpp
        src/workbench_find.cpp
        ${frost_startup_core_sources}
    DEPS frost::ui_vcs frost::vcs frost::ui_find frost::ui_debug frost::debug frost::ui_cmake frost::ui_terminal frost::term frost::ui_workbench frost::ui_nav frost::ui_lang frost::ui_run frost::ui_toolwindows frost::ui_editor
         frost::ui_shell frost::ui frost::engine frost::lang frost::run frost::build frost::document frost::project frost::settings
         frost::update frost::syntax frost::storage frost::text frost::input frost::rpc frost::io frost::sched frost::diag frost::core
    THIRD_PARTY Qt6::Widgets Qt6::Network)
target_compile_definitions(frost_app PRIVATE
    FROST_UPDATE_PUBLIC_KEY_HEX="${FROST_UPDATE_PUBLIC_KEY_HEX}"
    FROST_UPDATE_BUILD_NUMBER="${FROST_UPDATE_BUILD_NUMBER}"
    ${frost_automation_definitions})
set_target_properties(frost_app PROPERTIES AUTOMOC ON)
if(WIN32)
    # The DPI-awareness APIs in selftest.cpp and the dialog guard in startup.cpp (user32).
    target_link_libraries(frost_app PRIVATE user32)
endif()

if(WIN32)
    frost_add_executable(frost_held_read LAYER app SOURCES src/mono_held_read_main.cpp)
endif()

function(frost_deploy_clang_platform target)
    # Copy, never link, the isolated /MT LLVM engine into the application's runtime directory.
    add_custom_target(frost_platform_engine
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:frost-clang> $<TARGET_FILE_DIR:${target}>
        DEPENDS frost-clang VERBATIM)
    add_dependencies(${target} frost_platform_engine)
endfunction()

function(frost_stage_lua_test_bridge target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/lua-tests"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FROST_SOURCE_DIR}/tools/luaunit-discovery.lua"
            "$<TARGET_FILE_DIR:${target}>/lua-tests/luaunit-discovery.lua"
        VERBATIM)
endfunction()

if(NOT WIN32)
    frost_add_executable(frost LAYER app
        SOURCES src/main.cpp
        DEPS frost::app frost::ui_shell frost::ui frost::diag frost::core
        THIRD_PARTY Qt6::Widgets)
    # The crash handler frost starts lives beside the executable (frost/app/diagnostics_qt.hpp).
    frost_deploy_crashpad_handler(frost)
    if(FROST_ENABLE_SPAWN_LOG)
        target_compile_definitions(frost PRIVATE $<$<CONFIG:Debug>:FROST_SPAWN_LOG=1>)
    endif()
    target_compile_definitions(frost PRIVATE ${frost_automation_definitions})
    add_custom_target(frost_visualizers
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${FROST_SOURCE_DIR}/assets/visualizers $<TARGET_FILE_DIR:frost>/visualizers
        VERBATIM)
    add_dependencies(frost frost_visualizers)
    frost_deploy_clang_platform(frost)
    frost_stage_lua_test_bridge(frost)
    include("${FROST_SOURCE_DIR}/cmake/FrostLuaRuntime.cmake")
    frost_stage_lua_runtime(frost)
    return()
endif()

# frost_app.dll. Its import library gets its own name, because frost_app.lib is frost::app's
# static library in the same directory.
frost_add_library(app_shared LAYER app SHARED
    SOURCES src/main.cpp src/absl_symbolize_windows.cpp
    DEPS frost::app frost::ui_shell frost::ui frost::diag frost::core
    THIRD_PARTY Qt6::Widgets)
# src/absl_symbolize_windows.cpp replaces abseil's dbghelp.dll symbolizer (re2's fatal-error stack
# dump) with one that names nothing, so frost_app.dll imports no dbghelp.dll; it needs only
# abseil's headers (absl_symbolize.lib is on the link line through re2 already).
target_include_directories(frost_app_shared SYSTEM PRIVATE
    $<TARGET_PROPERTY:absl::symbolize,INTERFACE_INCLUDE_DIRECTORIES>)
# The crash handler frost_app.dll starts lives beside it (frost/app/diagnostics_qt.hpp).
frost_deploy_crashpad_handler(frost_app_shared)
set_target_properties(frost_app_shared PROPERTIES OUTPUT_NAME frost_app ARCHIVE_OUTPUT_NAME frost_app_shared)
# /MANIFEST:NO: without it CMake's vs_link_dll step embeds an RT_MANIFEST 2 (trustInfo) into the
# DLL, which would make the loader apply its 250-unit plain-path limit to it; the process
# manifest is frost.exe's. No /DEPENDENTLOADFLAG: a value would replace the launcher's
# LoadLibraryExW flags for the DLL's imports (frost/app/startup_core.hpp, kLauncherLoadFlags).
# FrostProcess.AppDllEmbedsNoManifest and AppDllHasNoDependentLoadFlags check both.
target_link_options(frost_app_shared PRIVATE /MANIFEST:NO)
# CommandLineToArgvW (shell32) and MessageBoxW (user32) in main.cpp.
target_link_libraries(frost_app_shared PRIVATE shell32 user32)
# The spawn log in Debug test builds only (FROST_ENABLE_SPAWN_LOG, frost/io/spawn_log.hpp).
if(FROST_ENABLE_SPAWN_LOG)
    target_compile_definitions(frost_app_shared PRIVATE $<$<CONFIG:Debug>:FROST_SPAWN_LOG=1>)
endif()
target_compile_definitions(frost_app_shared PRIVATE ${frost_automation_definitions})

# frost.exe, the launcher. The version resource carries the project version; the manifest
# comes from the root CMakeLists.txt, and the application icon comes from resources/icons/frost.
set(FROST_APP_ICON_PATH "${CMAKE_SOURCE_DIR}/resources/icons/frost/frost.ico")
configure_file(resources/windows/frost.rc.in ${CMAKE_CURRENT_BINARY_DIR}/frost.rc @ONLY)
# The manifest and icon are pulled in by resource statements, which rc depfiles do not list.
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/frost.rc PROPERTIES
    OBJECT_DEPENDS "${FROST_MANIFEST_PATH};${FROST_APP_ICON_PATH}")
frost_add_executable(frost LAYER app WIN32 NO_SANITIZE
    SOURCES src/launcher.cpp ${frost_startup_core_sources} ${CMAKE_CURRENT_BINARY_DIR}/frost.rc)
target_include_directories(frost PRIVATE include)
# The release static CRT in every configuration: the debug CRT's own assertion boxes would be
# out of reach of suppressCrashDialogs(), which configures frost_app.dll's shared CRT.
set_target_properties(frost PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
# frost.rc embeds our manifest; stop the linker from generating a second one. The static
# CRT's wmain entry under the GUI subsystem. /DEPENDENTLOADFLAG:0xA00
# (LOAD_LIBRARY_SEARCH_APPLICATION_DIR | _SYSTEM32) restricts the search for frost.exe's
# static imports, which are KERNEL32.dll alone (FrostProcess.LauncherImportsOnlyKernel32):
# defence in depth, read back by the self-test (check "dependent-load-flags").
target_link_options(frost PRIVATE /MANIFEST:NO /ENTRY:wmainCRTStartup /DEPENDENTLOADFLAG:0xA00)
if(FROST_ASAN_RUNTIME_DLL)
    # The launcher loads the ASan runtime before frost_app.dll, so it initialises before the
    # UCRT and Qt whatever order the linker gave frost_app.dll's imports.
    get_filename_component(_frost_asan_runtime_name "${FROST_ASAN_RUNTIME_DLL}" NAME)
    target_compile_definitions(frost PRIVATE FROST_LAUNCHER_PRELOAD_ASAN_RUNTIME="${_frost_asan_runtime_name}")
endif()
# Building frost builds the DLL it loads.
add_dependencies(frost frost_app_shared)
frost_deploy_clang_platform(frost)
add_dependencies(frost frost-dbgeng-dap)
# The Windows helper uses the locally installed VS MSBuild assemblies at runtime. Build it with
# the pinned .NET SDK in a private profile; it does not redistribute those VS assemblies.
add_custom_target(frost_project_host
    COMMAND "$ENV{FROST_SDK}/py/Scripts/python.exe" "${CMAKE_SOURCE_DIR}/tools/build_project_host.py"
            --sdk "$ENV{FROST_SDK}"
            --work "${CMAKE_BINARY_DIR}/project-host-work/$<CONFIG>"
            --output "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/project-host"
    VERBATIM)
add_dependencies(frost frost_project_host)
frost_deploy_conpty(frost)
# Platform plugins are loaded from <exe dir>/platforms: the native one and
# the offscreen one used by --selftest.
add_custom_command(TARGET frost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:frost>/platforms
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FROST_QT_PLUGIN_DIR}/platforms/qwindows$<$<CONFIG:Debug>:d>.dll
            ${FROST_QT_PLUGIN_DIR}/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll
            $<TARGET_FILE_DIR:frost>/platforms
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:frost>/tls
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FROST_QT_PLUGIN_DIR}/tls/qschannelbackend$<$<CONFIG:Debug>:d>.dll
            $<TARGET_FILE_DIR:frost>/tls
    VERBATIM)

# Original visualizer packs are staged alongside the application on every platform.
add_custom_target(frost_visualizers
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${FROST_SOURCE_DIR}/assets/visualizers $<TARGET_FILE_DIR:frost>/visualizers
    VERBATIM)

add_dependencies(frost frost_visualizers)

# Optional locally built standalone Lua; no download or recipe execution during the IDE build.
include("${FROST_SOURCE_DIR}/cmake/FrostLuaRuntime.cmake")
frost_stage_lua_runtime(frost)

frost_stage_lua_test_bridge(frost)
