# SPDX-License-Identifier: Apache-2.0
#
# frost::run (RUN-1, PLAN.md Appendix C R2): the Qt-free run configuration model with its
# per-workspace JSON store (.frost/runConfigurations/) and the RunManager that starts configurations
# through io::IProcessLauncher, streams their output, reports their exit and stops whole process
# trees. It runs programs through frost::build's RunSession, so it sits one rank above build
# (cmake/FrostLayers.cmake); the run UI (RUN-3), the test runner (RUN-7) and the debugger build on
# it. RUN-2 adds the generated types (run_types: CMake targets from the File API codemodel, MSBuild
# Application projects, native programs) and RunConfigurationSet, which lays the user's stored
# edits over them.

frost_add_library(run LAYER run
    SOURCES
        src/run_configuration.cpp
        src/run_configuration_set.cpp
        src/run_manager.cpp
        src/run_types.cpp
    DEPS frost::build frost::settings frost::project frost::rpc frost::io frost::core)
