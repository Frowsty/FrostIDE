# SPDX-License-Identifier: Apache-2.0
#
# The UI layer (FrostLayers "ui", rank 9; PLAN.md section 3.2): Qt-based building blocks the shell
# composes. frost::ui_input (ADR 0016) is the Qt adapter of frost::input; frost::ui_model (Qt-free),
# frost::ui_theme_compiler (Qt Core) and frost::ui (Qt Widgets: FrostStyle, AppearanceManager, the icon
# runtime, fonts, the OS appearance source and the island frames) are ADR 0015's; frost::ui also holds
# qtr, the Qt wrapper of frost::i18n. frost::ui_shell (src/ui/shell) is the main-window shell (ADR 0038);
# frost::ui_editor (src/ui/editor, the editor widget) is ADR 0037's and builds on frost::ui.
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
        src/tr.cpp
    DEPS frost::ui_model frost::io frost::core
    THIRD_PARTY Qt6::Widgets)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui PRIVATE
    include/frost/ui/appearance_manager.hpp
    include/frost/ui/frost_style.hpp
    include/frost/ui/island_frame.hpp
    include/frost/ui/qt_os_appearance.hpp)
set_target_properties(frost_ui PROPERTIES AUTOMOC ON)

add_subdirectory(shell)
add_subdirectory(editor)
# The build-side tool windows (ADR 0039); after frost_ui, which they build on.
add_subdirectory(toolwindows)
