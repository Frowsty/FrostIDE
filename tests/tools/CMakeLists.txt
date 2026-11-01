# SPDX-License-Identifier: Apache-2.0
#
# Tests for the repository tools in tools/ (frost-lint, frost-licensecheck,
# frost-featurecheck, corpus_fetch, asset_fetch, symbolize.py, and on Windows the PowerShell scripts
# bootstrap.ps1 and env.ps1) and for tests/perf/budgets.json. They are Python unittest suites over the
# fixtures in tests/tools/fixtures, registered with CTest (label "unit") so they run in
# both verify legs. Standard library only; Python 3.11+ (tomllib). Each suite is one
# CTest test; git must be on PATH.

# Prefer the SDK's Python on Windows (tools/bootstrap.ps1 creates it), so ctest uses the
# same interpreter as tools/verify.ps1.
if(WIN32 AND NOT Python3_EXECUTABLE AND DEFINED ENV{FROST_SDK} AND EXISTS "$ENV{FROST_SDK}/py/Scripts/python.exe")
    set(Python3_EXECUTABLE "$ENV{FROST_SDK}/py/Scripts/python.exe")
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

set(_frost_tool_suites frost_lint licensecheck featurecheck corpus_fetch asset_fetch budgets symbolize)
if(WIN32)
    # Runs Windows PowerShell 5.1 (bootstrap.ps1 and env.ps1); nothing to test on Linux.
    list(APPEND _frost_tool_suites windows_scripts)
endif()
foreach(suite IN LISTS _frost_tool_suites)
    add_test(NAME tools.${suite}
        COMMAND ${Python3_EXECUTABLE} -B -m unittest -v test_${suite}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(tools.${suite} PROPERTIES
        LABELS unit
        TIMEOUT 120
        # Keep the developer's SDK and corpus locations out of the tests.
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;FROST_QT_ROOT=;FROST_CORPUS_ROOT=")
endforeach()
