# SPDX-License-Identifier: Apache-2.0
#
# The automated journeys (PLAN.md section 7; FND-7a): the real FrostIDE driven through its automation
# endpoint (ADR 0080) against the pinned corpora with the pinned tools. Label corpus: they run in the
# -Corpus tier only (tools/verify.ps1 step 3b, tools/verify.sh --corpus), which sets
# FROST_CORPORA_ROOT; alone (frost_desktop) because they configure and build the shared corpus clone
# under its build lock. Each run leaves its evidence JSON (and the spawn logs it cites) under
# <build>/journey-evidence. journeys.support (label unit) tests their helpers in every leg.

if(NOT FROST_BUILD_UI)
    return()
endif()
if(WIN32 AND NOT Python3_EXECUTABLE AND DEFINED ENV{FROST_SDK} AND EXISTS "$ENV{FROST_SDK}/py/Scripts/python.exe")
    set(Python3_EXECUTABLE "$ENV{FROST_SDK}/py/Scripts/python.exe")
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

# j1_cmake: open, presets, configure, clangd on the owned database, Go to Definition in three
# modules, a seeded build error in Problems, fix, rebuild and clean (j1_cmake.py's header).
add_test(NAME journeys.j1_cmake
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/j1_cmake.py
            --frost $<TARGET_FILE:frost> --config $<CONFIG>
            --evidence ${CMAKE_BINARY_DIR}/journey-evidence)
set_tests_properties(journeys.j1_cmake PROPERTIES
    LABELS "corpus;integration"
    TIMEOUT 10800
    RESOURCE_LOCK frost_desktop
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# j2_run (RUN-2/RUN-3): the run configurations are the reply's EXECUTABLE targets; FrostEditor's,
# edited, runs from the toolbar (built once under the corpus build lock), streams its output, and
# Stop ends its whole process tree (j2_run.py's header). It imports j1_cmake.py's machinery.
add_test(NAME journeys.j2_run
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/j2_run.py
            --frost $<TARGET_FILE:frost> --config $<CONFIG>
            --evidence ${CMAKE_BINARY_DIR}/journey-evidence)
set_tests_properties(journeys.j2_run PROPERTIES
    LABELS "corpus;integration"
    TIMEOUT 18000
    RESOURCE_LOCK frost_desktop
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# The journey helpers that need no frost, corpus or tool (evidence, spawn-log reading, oracle parsers).
add_test(NAME journeys.support
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_journey_support.py -v)
set_tests_properties(journeys.support PROPERTIES
    LABELS unit
    TIMEOUT 120
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
