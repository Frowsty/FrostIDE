# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_cmake (ADR 0046): the UI of an opened CMake folder. CMakeTreeModel (the Solution
# Explorer's Qt item model over frost::project's CMakeViewModel), CMakeController (the queued relay
# from frost::build's CMakeService to the UI thread, behind the small ICMakeBackend seam) and
# CMakeToolWindow (profile selector, Reload, Stop, error banner, configure output). The W1 workbench
# wires them into the Solution Explorer and the tool-window manager. Layer ui; Qt Widgets.

frost_add_library(ui_cmake LAYER ui
    SOURCES
        src/cmake_controller.cpp
        src/cmake_tool_window.cpp
        src/cmake_tree_model.cpp
    DEPS frost::ui frost::build frost::project frost::core
    THIRD_PARTY Qt6::Widgets)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui_cmake PRIVATE
    include/frost/ui/cmake/cmake_controller.hpp
    include/frost/ui/cmake/cmake_tool_window.hpp
    include/frost/ui/cmake/cmake_tree_model.hpp)
set_target_properties(frost_ui_cmake PROPERTIES AUTOMOC ON)
