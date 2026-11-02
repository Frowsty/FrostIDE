# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_vcs (M5, ADR 0048): the Qt side of git. GitController (frost::vcs::GitService on the GUI
# thread), the VCS file colours for the Solution Explorer, the File System view and the editor tabs,
# the Commit tool window, the side-by-side diff viewer, the editor's change markers with their
# popup and Rollback Lines, the branch widget and the Branches popup, the git console and the VCS
# actions. The workbench wires the widgets into the shell later. Layer ui; Qt Widgets.

frost_add_library(ui_vcs LAYER ui
    SOURCES
        src/branch_widget.cpp
        src/change_markers.cpp
        src/commit_window.cpp
        src/diff_viewer.cpp
        src/git_actions.cpp
        src/git_console.cpp
        src/git_controller.cpp
        src/vcs_colors.cpp
    DEPS
        frost::ui_editor frost::ui_shell frost::ui frost::ui_model frost::vcs frost::diff frost::text_editor
        frost::text frost::input frost::io frost::core
    THIRD_PARTY Qt6::Widgets)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui_vcs PRIVATE
    include/frost/ui/vcs/branch_widget.hpp
    include/frost/ui/vcs/change_markers.hpp
    include/frost/ui/vcs/commit_window.hpp
    include/frost/ui/vcs/diff_viewer.hpp
    include/frost/ui/vcs/git_console.hpp
    include/frost/ui/vcs/git_controller.hpp)
set_target_properties(frost_ui_vcs PROPERTIES AUTOMOC ON)
