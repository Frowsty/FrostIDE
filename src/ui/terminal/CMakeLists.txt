# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_terminal (ADR 0051): the Qt side of the integrated terminal. TerminalView (one tab's
# cell grid, keys, IME, mouse, selection and clipboard over a frost::term::TerminalSession), the
# Terminal tool window with its tabs, and TerminalIntegration, which docks it in a ShellWindow
# (Alt+F12) and registers the terminal's actions. The terminal itself is Qt-free (frost::term).

frost_add_library(ui_terminal LAYER ui
    SOURCES
        src/terminal_colors.cpp
        src/terminal_tool_window.cpp
        src/terminal_view.cpp
    DEPS frost::ui_shell frost::ui frost::ui_model frost::term frost::settings frost::input frost::io frost::core
    THIRD_PARTY Qt6::Widgets)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui_terminal PRIVATE
    include/frost/ui/terminal/terminal_tool_window.hpp
    include/frost/ui/terminal/terminal_view.hpp)
set_target_properties(frost_ui_terminal PROPERTIES AUTOMOC ON)
