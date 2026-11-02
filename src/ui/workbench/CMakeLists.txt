# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_workbench (ADR 0042): the shell's navigation slice over frost::ui_shell and the Qt-free
# frost::ui_nav models. The Workbench (project, Find Action, Go to File, Search Everywhere, recent
# files and locations, back/forward, sessions, the hot-exit restore prompt, Save All and autosave),
# the File System tool window, the document editor factory with its banners, and the merge view.
# Layer ui; Qt Widgets.

frost_add_library(ui_workbench LAYER ui
    SOURCES
        src/document_editor.cpp
        src/file_system_view.cpp
        src/merge_view.cpp
        src/workbench.cpp
    DEPS frost::ui_shell frost::ui_nav frost::ui frost::search frost::document frost::project frost::settings
         frost::storage frost::diff frost::text frost::input frost::io frost::sched frost::core
    THIRD_PARTY Qt6::Widgets)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui_workbench PRIVATE
    include/frost/ui/workbench/document_editor.hpp
    include/frost/ui/workbench/file_system_view.hpp
    include/frost/ui/workbench/merge_view.hpp
    include/frost/ui/workbench/workbench.hpp)
set_target_properties(frost_ui_workbench PROPERTIES AUTOMOC ON)
