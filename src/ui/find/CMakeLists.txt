# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_find (ADR 0045): Find and Replace in Files in the shell. FindSession runs frost::search's
# engine on the scheduler and streams its results to the GUI thread; FindPopup is the Find/Replace
# in Files popup (Ctrl+Shift+F, Ctrl+Shift+R); FindToolWindow is the Find tool window (Alt+3);
# FindFeature registers the actions and the tool window with the shell. Layer ui; Qt Widgets.

frost_add_library(ui_find LAYER ui
    SOURCES
        src/find_feature.cpp
        src/find_popup.cpp
        src/find_session.cpp
        src/find_tool_window.cpp
        src/find_util.cpp
    DEPS frost::ui_shell frost::ui frost::search frost::document frost::project frost::input frost::sched frost::io
         frost::core
    THIRD_PARTY Qt6::Widgets)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui_find PRIVATE
    include/frost/ui/find/find_feature.hpp
    include/frost/ui/find/find_popup.hpp
    include/frost/ui/find/find_session.hpp
    include/frost/ui/find/find_tool_window.hpp)
set_target_properties(frost_ui_find PROPERTIES AUTOMOC ON)
