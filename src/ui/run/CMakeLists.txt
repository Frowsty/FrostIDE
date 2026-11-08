# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_run (W1, ADR 0040; RUN-3): the Qt side of running. RunConsole (one run's console: tinted
# output, the input line, Stop, Rerun, the exit-code line), RunToolWindow (the Run tool window v2:
# one tab per run), RunController (the run configurations of the open workspace on the toolbar's
# combo; native ones through frost::run's RunManager, MSBuild projects through RunService, CMake
# targets through the CMake integration; the stop-and-rerun prompt), RunConfigurationsDialog (Run >
# Edit Configurations...) and the run actions in frost::input's registry. The services themselves
# are Qt-free (frost::build, frost::run).

frost_add_library(ui_run LAYER ui
    SOURCES
        src/run_actions.cpp
        src/run_configurations_dialog.cpp
        src/run_console.cpp
        src/run_controller.cpp
        src/run_tool_window.cpp
    DEPS frost::run frost::build frost::input frost::io frost::core
    THIRD_PARTY Qt6::Widgets)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui_run PRIVATE
    include/frost/ui/run_configurations_dialog.hpp
    include/frost/ui/run_console.hpp
    include/frost/ui/run_controller.hpp
    include/frost/ui/run_tool_window.hpp)
set_target_properties(frost_ui_run PROPERTIES AUTOMOC ON)
