# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_shell (ADR 0038): the main-window shell the app composes. ShellWindow with the menu bar
# built from frost::input's action registry, the main toolbar, editor tabs over the IEditorFactory
# seam (with the placeholder editor until m1/editor-view), ToolWindowManager v1, the status bar
# widgets, the popup framework, notifications and background tasks, and the shortcut dispatcher
# that routes key events to frost::input (ADR 0016). Layer ui; Qt Widgets.

frost_add_library(ui_shell LAYER ui
    SOURCES
        src/background_tasks.cpp
        src/editor_tabs.cpp
        src/main_toolbar.cpp
        src/notifications.cpp
        src/placeholder_editor.cpp
        src/popup.cpp
        src/shell_menus.cpp
        src/shell_settings.cpp
        src/shell_window.cpp
        src/shortcut_dispatcher.cpp
        src/status_bar.cpp
        src/tool_window_manager.cpp
    DEPS frost::ui frost::ui_input frost::input frost::text frost::io frost::core
    THIRD_PARTY Qt6::Widgets)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui_shell PRIVATE
    include/frost/ui/shell/background_tasks.hpp
    include/frost/ui/shell/editor_tabs.hpp
    include/frost/ui/shell/main_toolbar.hpp
    include/frost/ui/shell/notifications.hpp
    include/frost/ui/shell/popup.hpp
    include/frost/ui/shell/shell_menus.hpp
    include/frost/ui/shell/shell_window.hpp
    include/frost/ui/shell/shortcut_dispatcher.hpp
    include/frost/ui/shell/status_bar.hpp
    include/frost/ui/shell/tool_window_manager.hpp)
set_target_properties(frost_ui_shell PROPERTIES AUTOMOC ON)
