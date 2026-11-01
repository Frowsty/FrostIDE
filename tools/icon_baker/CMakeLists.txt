# SPDX-License-Identifier: Apache-2.0
#
# frost-icon-baker (ADR 0015 decision 7): SVG to FIA atlases at build time. The only FrostIDE
# target that uses Qt Svg, which is found here and never in the root CMakeLists.txt, so
# licensecheck sees Qt6Svg only in this icon-baker context (FrostLayers matches Qt6:: names
# before resolving them, so a directory-scoped find_package is enough).
# Budget: B28. The baker links ui_model only (never frost::ui), so UI edits do not re-bake, and
# it rewrites an atlas only when its bytes change.
find_package(Qt6 ${FROST_QT_VERSION} EXACT REQUIRED COMPONENTS Gui Svg)

frost_add_executable(frost-icon-baker LAYER ui
    SOURCES main.cpp svg_raster.cpp
    DEPS frost::ui_model
    THIRD_PARTY Qt6::Gui Qt6::Svg)
target_include_directories(frost-icon-baker PRIVATE ${FROST_SOURCE_DIR}/tools/common)
target_compile_definitions(frost-icon-baker PRIVATE FROST_QT_PLUGIN_DIR="${FROST_QT_PLUGIN_DIR}")
if(WIN32)
    # The offscreen platform plugin, next to the baker (it never relies on a deployed copy).
    add_custom_command(TARGET frost-icon-baker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:frost-icon-baker>/platforms
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${FROST_QT_PLUGIN_DIR}/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll
                $<TARGET_FILE_DIR:frost-icon-baker>/platforms
        VERBATIM)
endif()
