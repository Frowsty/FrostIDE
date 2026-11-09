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
#
# The test runner (RUN-7): the test model, the parsers of ctest's and Google Test's own output (the
# only sources of test results, frost-lint's truth rule) and TestRunner, which runs ctest and test
# programs through RunSession. pugixml (ADR 0033) reads ctest's JUnit report, privately.
# RUN-8 adds doctest: its case lists and XML reports (doctest_parsers.cpp, also with pugixml), and
# the runner jobs that list and run a ctest test's doctest cases directly.

frost_add_library(run LAYER run
    SOURCES
        src/run_configuration.cpp
        src/run_configuration_set.cpp
        src/run_manager.cpp
        src/run_types.cpp
        src/test_model.cpp
        src/ctest_parsers.cpp
        src/gtest_parsers.cpp
        src/doctest_parsers.cpp
        src/test_runner.cpp
    DEPS frost::build frost::settings frost::project frost::rpc frost::io frost::core)
target_link_libraries(frost_run PRIVATE pugixml::pugixml)
