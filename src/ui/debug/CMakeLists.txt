# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_debug (PLAN.md M4, ADR 0050): the Qt side of U1's debugger. DebugController (the Debug
# run hand-off, the session's events on the GUI thread, stepping, the execution point, the editors'
# breakpoint gutters), the Debug tool window (toolbar, threads and frames, Variables, Watches, the
# console) with the Evaluate Expression dialog, and the debugger's actions. The session itself is
# Qt-free (frost::debug); Debug builds and resolves through frost::build's RunService.

frost_add_library(ui_debug LAYER ui
    SOURCES
        src/debug_depth_panel.cpp
        src/debug_actions.cpp
        src/debug_controller.cpp
        src/debug_controller_native.cpp
        src/debug_controller_actions.cpp
        src/debug_models.cpp
        src/debug_retirement.cpp
        src/debug_session_registry.cpp
        src/debug_sessions_window.cpp
        src/mixed_debug_window.cpp
        src/debug_tool_window.cpp
    DEPS frost::ui_run frost::ui_editor frost::ui_shell frost::debug frost::build frost::input frost::core
    THIRD_PARTY Qt6::Widgets)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui_debug PRIVATE
    include/frost/ui/debug_controller.hpp
    include/frost/ui/debug_models.hpp
    include/frost/ui/debug_retirement.hpp
    include/frost/ui/debug_session_registry.hpp
    include/frost/ui/debug_sessions_window.hpp
    include/frost/ui/mixed_debug_window.hpp
    include/frost/ui/debug_tool_window.hpp)
set_target_properties(frost_ui_debug PROPERTIES AUTOMOC ON)
