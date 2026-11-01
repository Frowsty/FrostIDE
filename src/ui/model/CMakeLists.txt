# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_model (ADR 0015): the Qt-free half of the UI layer. The theme key registry,
# the theme blob codec, WCAG contrast, the UI scale, the appearance settings seam, OS
# appearance resolution, and the icon atlas (FIA) codec, packer and selection rule.
# Self-contained (it needs only FrostLayers and frost::core/frost::io), so a fuzz tree can
# add this directory on its own.

set(frost_ui_model_sources
    src/appearance_settings.cpp
    src/contrast.cpp
    src/icon_atlas_format.cpp
    src/os_appearance.cpp
    src/rgba.cpp
    src/theme.cpp
    src/theme_blob.cpp)
if(WIN32)
    list(APPEND frost_ui_model_sources src/os_appearance_win.cpp)
endif()

frost_add_library(ui_model LAYER ui
    SOURCES ${frost_ui_model_sources}
    DEPS frost::core frost::io)
if(WIN32)
    # RegGetValueW (advapi32) and SystemParametersInfoW (user32) in os_appearance_win.cpp.
    target_link_libraries(frost_ui_model PRIVATE advapi32 user32)
endif()

# The "ui" layer may link Qt, so FrostLayers' own check would accept Qt here. This one keeps
# the model Qt-free: configure fails when frost_ui_model's direct or interface link libraries
# name a Qt target, or reach one through a non-FrostIDE wrapper target. (The model's tests are
# a non-Qt executable, so a Qt include in a model header also fails to compile.)
function(_frost_ui_model_reject_qt)
    set(queue "")
    foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(libs frost_ui_model ${property})
        if(libs)
            list(APPEND queue ${libs})
        endif()
    endforeach()
    set(seen "")
    while(queue)
        list(POP_FRONT queue item)
        if(item IN_LIST seen)
            continue()
        endif()
        list(APPEND seen "${item}")
        if(item MATCHES "Qt6?::")
            message(FATAL_ERROR "frost_ui_model must stay Qt-free (ADR 0015), but its link graph names ${item}")
        endif()
        if(item MATCHES "^\\$<" OR NOT TARGET "${item}")
            continue()
        endif()
        get_target_property(dep_layer "${item}" FROST_LAYER)
        if(dep_layer)
            # FrostIDE libraries below "ui" are already kept Qt-free by FrostLayers.
            continue()
        endif()
        get_target_property(libs "${item}" INTERFACE_LINK_LIBRARIES)
        if(libs)
            list(APPEND queue ${libs})
        endif()
    endwhile()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _frost_ui_model_reject_qt)
