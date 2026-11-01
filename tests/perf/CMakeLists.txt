# SPDX-License-Identifier: Apache-2.0
#
# Performance harness code (PLAN.md section 4). budgets.json beside this file is
# the source of truth for the budgets; this directory holds the code that
# measures them:
#   probes/  frost::perf_probes, the backend-neutral on-screen probes and their tests;
#   s1/      the M0 spike S1 (Windows only, ADR 0007). Its executables open windows on
#            the real desktop, so tests/perf/s1/run-s1.ps1 runs them in the interactive
#            session, never CTest; only the headless s1_editor_tests are CTest tests.
add_subdirectory(probes)
if(WIN32 AND FROST_BUILD_UI)
    add_subdirectory(s1)
endif()
