# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::input (ADR 0016):
#   input_tests             the key model, syntax, layouts, matcher, keymaps, registry,
#                           service, Find Action and the fuzz-corpus replay, label "unit";
#   input_stress_tests      the 50k-stream matcher property, label "stress";
#   input_alloc_tests       the no-allocation check of the keystroke path; it replaces the
#                           global operator new and delete, so it is its own executable;
#   input_layout_os_tests   read-only checks of the Windows layout reader against the OS,
#                           label "integration";
#   input_perf_tests        timing reports (asserted only in optimised, unsanitised builds),
#                           label "perf";
#   input.NoLayoutSwitching[.DetectsPlanted]  the guard scripts: no code may switch, load or
#                           unload keyboard layouts or inject real input.
# No test switches the machine's keyboard layout or injects input: layouts are data and
# SimulatedKeyboard produces the events.
# TODO(M1): register input_keymap with frost_add_fuzz_target once FrostFuzz.cmake is on the base.

set(frost_input_test_support
    support/golden.cpp
    support/matcher_rig.cpp
    support/reachability.cpp
    support/synthetic_layouts.cpp
    support/test_actions.cpp
    fuzz/keymap_fuzz_core.cpp)

frost_add_test(input_tests
    SOURCES
        ${frost_input_test_support}
        action_registry_test.cpp
        double_tap_test.cpp
        find_action_test.cpp
        intellij_keymap_test.cpp
        keymap_format_test.cpp
        keymap_fuzz_replay_test.cpp
        keymap_reachability_test.cpp
        keymap_service_test.cpp
        layout_test.cpp
        shortcut_layouts_test.cpp
        shortcut_matcher_test.cpp
        stroke_syntax_test.cpp
    DEPS frost::input)

frost_add_test(input_stress_tests
    SOURCES
        ${frost_input_test_support}
        matcher_stress_test.cpp
    DEPS frost::input
    LABELS stress)

frost_add_test(input_alloc_tests
    SOURCES
        ${frost_input_test_support}
        alloc_test.cpp
    DEPS frost::input)

frost_add_test(input_layout_os_tests
    SOURCES layout_os_test.cpp
    DEPS frost::input
    LABELS integration)
if(WIN32)
    # MapVirtualKeyExW, VkKeyScanExW and GetKeyboardLayoutNameW: the read-only OS oracles.
    target_link_libraries(input_layout_os_tests PRIVATE user32)
endif()

frost_add_test(input_perf_tests
    SOURCES
        ${frost_input_test_support}
        perf_test.cpp
    DEPS frost::input
    LABELS perf)

foreach(test_target input_tests input_stress_tests input_alloc_tests input_layout_os_tests input_perf_tests)
    # tests/sched/sched_test_support.hpp (kAssertBudgets, environment) and tests/input/support.
    target_include_directories(${test_target} PRIVATE ${FROST_SOURCE_DIR}/tests)
    target_compile_definitions(${test_target} PRIVATE
        FROST_INPUT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/goldens"
        FROST_INPUT_FUZZ_CORPUS="${FROST_SOURCE_DIR}/tests/fuzz/corpus/input_keymap")
endforeach()

# The layout-switching guard: a source scan, and a check that the scan detects every
# planted name in tests/input/fixtures/layout_guard/.
add_test(NAME input.NoLayoutSwitching
    COMMAND ${CMAKE_COMMAND} -DFROST_SOURCE_DIR=${FROST_SOURCE_DIR} -DMODE=scan
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_layout_switch.cmake)
add_test(NAME input.NoLayoutSwitching.DetectsPlanted
    COMMAND ${CMAKE_COMMAND} -DFROST_SOURCE_DIR=${FROST_SOURCE_DIR} -DMODE=fixture
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_layout_switch.cmake)
set_tests_properties(input.NoLayoutSwitching input.NoLayoutSwitching.DetectsPlanted PROPERTIES LABELS unit)
