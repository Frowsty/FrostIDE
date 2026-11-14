# SPDX-License-Identifier: Apache-2.0
#
# The automated journeys (PLAN.md section 7; FND-7a): the real FrostIDE driven through its automation
# endpoint (ADR 0080) against the pinned corpora with the pinned tools. Label corpus: they run in the
# -Corpus tier only (tools/verify.ps1 step 3b, tools/verify.sh --corpus), which sets
# FROST_CORPORA_ROOT; alone (frost_desktop) because they configure and build the shared corpus clone
# under its build lock. The journeys on a scratch copy of a fixture (journeys.trust_malicious_repo,
# journeys.j2_msvc_synthetic) need no corpus and are label integration. Each run leaves its evidence
# JSON (and the spawn logs it cites) under
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

# j3_frostengine_lua (FND-7c, FEC-10b): schema.export completion and Go to Definition / Go to C++
# Binding on the pinned FrostEngine clone, live module require() and runtime didChangeConfiguration,
# and Inspect Code against the lua-language-server --check oracle (j3_frostengine_lua.py's header).
add_test(NAME journeys.j3_frostengine_lua
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/j3_frostengine_lua.py
            --frost $<TARGET_FILE:frost> --config $<CONFIG>
            --evidence ${CMAKE_BINARY_DIR}/journey-evidence)
set_tests_properties(journeys.j3_frostengine_lua PROPERTIES
    LABELS "corpus;integration"
    TIMEOUT 7200
    COST 500
    RESOURCE_LOCK frost_desktop
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# j4 requires the Debug-only automation endpoint. A missing engine or failed authentication is a
# failure, never a skip; it shares the desktop/corpus build lock with the earlier journeys.
if(FROST_ENABLE_AUTOMATION_ENDPOINT AND (CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Debug"))
    add_test(NAME journeys.j4_engine_session CONFIGURATIONS Debug
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/j4_engine_session.py
                --frost $<TARGET_FILE:frost> --config $<CONFIG>
                --evidence ${CMAKE_BINARY_DIR}/journey-evidence)
    set_tests_properties(journeys.j4_engine_session PROPERTIES
        LABELS "corpus;integration"
        TIMEOUT 18000
        RESOURCE_LOCK frost_desktop
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()

# j2_msvc (FND-7b): open, edit, build and debug an MSVC solution with the real MSBuild and lldb-dap
# (j2_msvc.py's header). Windows only (Visual Studio's MSBuild) and Debug only (the endpoint and the
# spawn log). On the synthetic solution (a scratch copy of tests/project/fixtures/msvc_generated) it
# is an integration test of every Windows -Quick leg; on the pinned Notepad++ clone (R2a) it is in
# the corpus tier.
if(WIN32 AND FROST_ENABLE_AUTOMATION_ENDPOINT AND FROST_ENABLE_SPAWN_LOG)
    if(CMAKE_CONFIGURATION_TYPES)
        set(_frost_j2_msvc_configurations CONFIGURATIONS Debug)
    else()
        set(_frost_j2_msvc_configurations)
    endif()
    if(CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        foreach(_frost_j2_solution synthetic r2a)
            add_test(NAME journeys.j2_msvc_${_frost_j2_solution} ${_frost_j2_msvc_configurations}
                COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/j2_msvc.py
                        --frost $<TARGET_FILE:frost> --config $<CONFIG> --solution ${_frost_j2_solution}
                        --evidence ${CMAKE_BINARY_DIR}/journey-evidence)
        endforeach()
        set_tests_properties(journeys.j2_msvc_synthetic PROPERTIES
            LABELS integration
            TIMEOUT 3600
            RESOURCE_LOCK frost_desktop
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
        set_tests_properties(journeys.j2_msvc_r2a PROPERTIES
            LABELS "corpus;integration"
            TIMEOUT 7200
            RESOURCE_LOCK frost_desktop
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    endif()
endif()

# FND-8, the malicious-repository test (trust_malicious_repo.py's header): the real frost opens a
# copy of fixtures/malicious-repo before trust and only restricted mode may start anything.
# trust_canary is the planted binary (it marks every run of a copy). Static CRT on Windows, so a
# copy runs anywhere.
add_executable(trust_canary trust_canary.cpp)
frost_apply_compile_options(trust_canary TEST NO_SANITIZE)
if(WIN32)
    set_target_properties(trust_canary PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
endif()
# Only Debug frost has the endpoint and the spawn log, so the test exists in Debug only (the Release
# and sanitizer legs, which have neither, do not list it).
if(FROST_ENABLE_AUTOMATION_ENDPOINT AND FROST_ENABLE_SPAWN_LOG)
    if(CMAKE_CONFIGURATION_TYPES)
        set(_frost_trust_configurations CONFIGURATIONS Debug)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_frost_trust_configurations "")
    else()
        set(_frost_trust_configurations SKIP)
    endif()
    if(NOT _frost_trust_configurations STREQUAL "SKIP")
        add_test(NAME journeys.trust_malicious_repo ${_frost_trust_configurations}
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/trust_malicious_repo.py
                    --frost $<TARGET_FILE:frost> --config $<CONFIG> --canary $<TARGET_FILE:trust_canary>
                    --evidence ${CMAKE_BINARY_DIR}/journey-evidence)
        set_tests_properties(journeys.trust_malicious_repo PROPERTIES
            LABELS integration
            TIMEOUT 900
            # It runs frost (offscreen) and real tools after the grant: alone, like every frost run.
            RESOURCE_LOCK frost_desktop
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    endif()
endif()

# The journey helpers that need no frost, corpus or tool (evidence, spawn-log reading, oracle parsers).
add_test(NAME journeys.support
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_journey_support.py -v)
set_tests_properties(journeys.support PROPERTIES
    LABELS unit
    TIMEOUT 120
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
