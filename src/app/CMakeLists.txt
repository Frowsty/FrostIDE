# SPDX-License-Identifier: Apache-2.0
#
# frost::app (the application library: command line, main window, self-test)
# and frost, the IDE executable. The library exists so tests can exercise the
# same code the executable runs.

frost_add_library(app LAYER app
    SOURCES
        include/frost/app/main_window.hpp
        src/command_line.cpp
        src/main_window.cpp
        src/selftest.cpp
    DEPS frost::core
    THIRD_PARTY Qt6::Widgets)
set_target_properties(frost_app PROPERTIES AUTOMOC ON)
if(WIN32)
    # CommandLineToArgvW (shell32) and the DPI-awareness APIs (user32).
    target_link_libraries(frost_app PRIVATE shell32 user32)
endif()

set(frost_sources src/main.cpp)
if(WIN32)
    # The version resource carries the project version; the manifest comes from the root CMakeLists.txt.
    configure_file(resources/windows/frost.rc.in ${CMAKE_CURRENT_BINARY_DIR}/frost.rc @ONLY)
    list(APPEND frost_sources ${CMAKE_CURRENT_BINARY_DIR}/frost.rc)
    # The manifest is pulled in by a resource statement, which rc depfiles do not list.
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/frost.rc PROPERTIES
        OBJECT_DEPENDS ${FROST_MANIFEST_PATH})
endif()

frost_add_executable(frost LAYER app WIN32
    SOURCES ${frost_sources}
    DEPS frost::app frost::core
    THIRD_PARTY Qt6::Widgets)

if(WIN32)
    # frost.rc embeds our manifest; stop the linker from generating a second one.
    target_link_options(frost PRIVATE /MANIFEST:NO)
    frost_copy_runtime_dlls(frost)
    # Platform plugins are loaded from <exe dir>/platforms: the native one and
    # the offscreen one used by --selftest.
    add_custom_command(TARGET frost POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:frost>/platforms
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${FROST_QT_PLUGIN_DIR}/platforms/qwindows$<$<CONFIG:Debug>:d>.dll
                ${FROST_QT_PLUGIN_DIR}/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll
                $<TARGET_FILE_DIR:frost>/platforms
        VERBATIM)
endif()
