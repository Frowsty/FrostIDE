# SPDX-License-Identifier: Apache-2.0
#
# Performance harness code (PLAN.md section 4). budgets.json beside this file is
# the source of truth for the budgets; this directory holds the code that
# measures them:
#   probes/  frost::perf_probes, the backend-neutral on-screen probes and their tests;
#   s1/      the M0 spike S1 executables (Windows only, ADR 0007). They open windows
#            on the real desktop, so they are run by tests/perf/s1/run-s1.ps1 in the
#            interactive session and are never registered with CTest.
add_subdirectory(probes)
if(WIN32 AND FROST_BUILD_UI)
    add_subdirectory(s1)
endif()
