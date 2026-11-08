# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_run (W1, ADR 0040): the Qt side of running the startup project. RunConsole (the Run
# tool window's console: tinted output, the input line, Stop, Rerun, the exit-code line),
# RunController (RunService's events on the GUI thread, the toolbar's startup project combo,
# the stop-and-rerun prompt) and the run actions in frost::input's registry. The service itself
# is Qt-free (frost::build). TestResultsTree (RUN-7) is the Unit Tests window's results tree over
# frost::run's test model.

frost_add_library(ui_run LAYER ui
    SOURCES
        src/run_actions.cpp
        src/run_console.cpp
        src/run_controller.cpp
        src/test_results_tree.cpp
    DEPS frost::run frost::build frost::input frost::core
    THIRD_PARTY Qt6::Widgets)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui_run PRIVATE
    include/frost/ui/run_console.hpp
    include/frost/ui/run_controller.hpp
    include/frost/ui/test_results_tree.hpp)
set_target_properties(frost_ui_run PROPERTIES AUTOMOC ON)
