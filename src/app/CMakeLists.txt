# SPDX-License-Identifier: Apache-2.0
#
# frost::app (the application library: command line, start-up hardening and checks,
# main window, self-test) and the FrostIDE program built from it. The library exists so
# tests can exercise the same code the program runs.
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

frost_add_library(app LAYER app
    SOURCES
        include/frost/app/main_window.hpp
        src/command_line.cpp
        src/main_window.cpp
        src/selftest.cpp
        src/startup.cpp
        ${frost_startup_core_sources}
    DEPS frost::core
    THIRD_PARTY Qt6::Widgets)
set_target_properties(frost_app PROPERTIES AUTOMOC ON)
if(WIN32)
    # The DPI-awareness APIs in selftest.cpp and the dialog guard in startup.cpp (user32).
    target_link_libraries(frost_app PRIVATE user32)
endif()

if(NOT WIN32)
    frost_add_executable(frost LAYER app
        SOURCES src/main.cpp
        DEPS frost::app frost::core
        THIRD_PARTY Qt6::Widgets)
    return()
endif()

# frost_app.dll. Its import library gets its own name, because frost_app.lib is frost::app's
# static library in the same directory.
frost_add_library(app_shared LAYER app SHARED
    SOURCES src/main.cpp
    DEPS frost::app frost::core
    THIRD_PARTY Qt6::Widgets)
set_target_properties(frost_app_shared PROPERTIES OUTPUT_NAME frost_app ARCHIVE_OUTPUT_NAME frost_app_shared)
# /MANIFEST:NO: without it CMake's vs_link_dll step embeds an RT_MANIFEST 2 (trustInfo) into the
# DLL, which would make the loader apply its 250-unit plain-path limit to it; the process
# manifest is frost.exe's. No /DEPENDENTLOADFLAG: a value would replace the launcher's
# LoadLibraryExW flags for the DLL's imports (frost/app/startup_core.hpp, kLauncherLoadFlags).
# FrostProcess.AppDllEmbedsNoManifest and AppDllHasNoDependentLoadFlags check both.
target_link_options(frost_app_shared PRIVATE /MANIFEST:NO)
# CommandLineToArgvW (shell32) and MessageBoxW (user32) in main.cpp.
target_link_libraries(frost_app_shared PRIVATE shell32 user32)

# frost.exe, the launcher. The version resource carries the project version; the manifest
# comes from the root CMakeLists.txt.
configure_file(resources/windows/frost.rc.in ${CMAKE_CURRENT_BINARY_DIR}/frost.rc @ONLY)
# The manifest is pulled in by a resource statement, which rc depfiles do not list.
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/frost.rc PROPERTIES
    OBJECT_DEPENDS ${FROST_MANIFEST_PATH})
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
# Platform plugins are loaded from <exe dir>/platforms: the native one and
# the offscreen one used by --selftest.
add_custom_command(TARGET frost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:frost>/platforms
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FROST_QT_PLUGIN_DIR}/platforms/qwindows$<$<CONFIG:Debug>:d>.dll
            ${FROST_QT_PLUGIN_DIR}/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll
            $<TARGET_FILE_DIR:frost>/platforms
    VERBATIM)
