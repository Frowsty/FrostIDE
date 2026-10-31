# SPDX-License-Identifier: Apache-2.0
#
# frost::perf_probes: the backend-neutral on-screen probes and their analysis,
# shared by the S1 spike (tests/perf/s1) and the later B1/B5/B6 harnesses
# (PLAN.md section 4, Measurement). Harness-only: frost.exe never links it.
#
# The analysis half (statistics, frame markers, pacing analysis, the session
# precheck policy, probe-validation scoring, the report writer) is portable and
# its unit tests run on both platforms. The capture half (DWM counters, Desktop
# Duplication, vblank waits, SystemSessionState, timing) is Win32/DXGI only.
# No Qt: the probes must observe any backend from outside. Layer diag (rank 8,
# self-diagnostics) because it only needs frost::core and the app-layer spike
# executables depend on it.

set(perf_probe_sources
    src/frame_analysis.cpp
    src/frame_marker.cpp
    src/json_writer.cpp
    src/probe_validation.cpp
    src/report.cpp
    src/session_state.cpp
    src/stats.cpp)
if(WIN32)
    list(APPEND perf_probe_sources
        src/win/screen_probes.cpp
        src/win/system_session_state.cpp
        src/win/timing.cpp)
endif()

frost_add_library(perf_probes LAYER diag
    SOURCES ${perf_probe_sources}
    DEPS frost::core)
if(WIN32)
    # Windows SDK import libraries only (no third-party code).
    target_link_libraries(frost_perf_probes PRIVATE d3d11 dxgi dwmapi powrprof wtsapi32 user32)
endif()

set(perf_probe_test_sources
    test/frame_analysis_test.cpp
    test/frame_marker_test.cpp
    test/json_writer_test.cpp
    test/probe_validation_test.cpp
    test/session_state_test.cpp
    test/stats_test.cpp)
if(WIN32)
    list(APPEND perf_probe_test_sources test/win_probes_test.cpp)
endif()
frost_add_test(perf_probe_tests
    SOURCES ${perf_probe_test_sources}
    DEPS frost::perf_probes)
