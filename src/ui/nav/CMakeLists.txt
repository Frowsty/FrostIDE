# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_nav (ADR 0042): the Qt-free models of the shell's navigation slice. Navigation history,
# recent files and locations, session state with its JSON store (yyjson, MIT, ADR 0021), the
# document registry that canonicalizes paths and opens them through the SaveManager, the project's
# files (the gitignore-aware scan plus the solution's files), and the aligned panes of the merge
# view. Layer ui, but Qt-free: its tests are a non-Qt executable, so a Qt include fails to compile.
frost_add_library(ui_nav LAYER ui
    SOURCES
        src/document_registry.cpp
        src/merge_layout.cpp
        src/navigation_history.cpp
        src/paths.cpp
        src/project_files.cpp
        src/session.cpp
    DEPS frost::document frost::project frost::diff frost::text frost::io frost::sched frost::core)
target_link_libraries(frost_ui_nav PRIVATE yyjson::yyjson)
