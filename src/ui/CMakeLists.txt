# SPDX-License-Identifier: Apache-2.0
#
# The UI layer (FrostLayers "ui", rank 9; PLAN.md section 3.2): Qt-based building blocks the shell
# composes. frost::ui_input (ADR 0016) is the Qt adapter of frost::input; frost::ui_model (Qt-free),
# frost::ui_theme_compiler (Qt Core) and frost::ui (Qt Widgets: FrostStyle, AppearanceManager, the icon
# runtime, fonts, the OS appearance source and the island frames) are ADR 0015's. frost::ui_lang
# (ADR 0041) holds the language-feature widgets and their controller; it needs frost::lang, which
# fuzz trees leave out.
add_subdirectory(input)
add_subdirectory(model)
add_subdirectory(theme_compiler)

frost_add_library(ui LAYER ui
    SOURCES
        src/appearance_manager.cpp
        src/fonts.cpp
        src/frost_style.cpp
        src/icon_atlas_set.cpp
        src/icon_engine.cpp
        src/island_frame.cpp
        src/qt_os_appearance.cpp
    DEPS frost::ui_model frost::io frost::core
    THIRD_PARTY Qt6::Widgets)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui PRIVATE
    include/frost/ui/appearance_manager.hpp
    include/frost/ui/frost_style.hpp
    include/frost/ui/island_frame.hpp
    include/frost/ui/qt_os_appearance.hpp)
set_target_properties(frost_ui PROPERTIES AUTOMOC ON)

if(TARGET frost_lang)
    add_subdirectory(lang)
endif()
