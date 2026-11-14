# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_toolwindows (W1, ADR 0039): the build-side tool windows the shell docks (Solution
# Explorer, build output, the Problems list, the configuration combo and Build actions, the trust
# prompt, the Configuration Manager of SLN-7) and the view models behind them, which tests drive without a window. BuildController
# marshals frost::build::BuildService's callbacks to the GUI thread.

frost_add_library(ui_toolwindows LAYER ui
    SOURCES
        src/build_actions.cpp
        src/build_controller.cpp
        src/build_output.cpp
        src/configuration_manager_dialog.cpp
        src/engine_console.cpp
        src/engine_view.cpp
        src/file_manager.cpp
        src/problems.cpp
        src/solution_explorer.cpp
        src/solution_tree_model.cpp
        src/tool_icons.cpp
        src/trust_dialog.cpp
    DEPS frost::ui frost::build frost::project frost::input frost::io frost::core frost::engine
    THIRD_PARTY Qt6::Widgets)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui_toolwindows PRIVATE
    include/frost/ui/toolwindows/build_actions.hpp
    include/frost/ui/toolwindows/build_controller.hpp
    include/frost/ui/toolwindows/build_output.hpp
    include/frost/ui/toolwindows/configuration_manager_dialog.hpp
    include/frost/ui/toolwindows/engine_console.hpp
    include/frost/ui/toolwindows/engine_view.hpp
    include/frost/ui/toolwindows/problems.hpp
    include/frost/ui/toolwindows/solution_explorer.hpp
    include/frost/ui/toolwindows/solution_tree_model.hpp
    include/frost/ui/toolwindows/trust_dialog.hpp)
set_target_properties(frost_ui_toolwindows PROPERTIES AUTOMOC ON)
