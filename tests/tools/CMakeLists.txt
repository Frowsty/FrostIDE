# SPDX-License-Identifier: Apache-2.0
#
# Tests for the repository tools in tools/ (frost-lint, frost-licensecheck,
# frost-featurecheck, corpus_fetch, asset_fetch, symbolize.py, verify_support.py, frost_perf.py, the
# verify scripts, the commit-msg hook and verify-stamps in tools/hooks, the lldb-dap build recipes'
# verify mode, and on Windows the PowerShell scripts bootstrap.ps1 and env.ps1) and for
# tests/perf/budgets.json. They are Python unittest suites over
# the fixtures in tests/tools/fixtures, registered with CTest (label "unit", and mod.tools from
# tests/CMakeLists.txt) so they run in both verify legs. Standard library only; Python 3.11+
# (tomllib). Each suite is one CTest test; git must be on PATH. test_verify_scripts runs PowerShell
# 5.1 on Windows and bash on Linux.

# Prefer the SDK's Python on Windows (tools/bootstrap.ps1 creates it), so ctest uses the
# same interpreter as tools/verify.ps1.
if(WIN32 AND NOT Python3_EXECUTABLE AND DEFINED ENV{FROST_SDK} AND EXISTS "$ENV{FROST_SDK}/py/Scripts/python.exe")
    set(Python3_EXECUTABLE "$ENV{FROST_SDK}/py/Scripts/python.exe")
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

# Real portable-PDB checks use a private build-tree profile and never create the real NuGet marker.
set(_frost_managed_test_dotnet "")
if(DEFINED ENV{DOTNET_ROOT} AND EXISTS "$ENV{DOTNET_ROOT}/dotnet${CMAKE_EXECUTABLE_SUFFIX}")
    set(_frost_managed_test_dotnet "$ENV{DOTNET_ROOT}/dotnet${CMAKE_EXECUTABLE_SUFFIX}")
endif()
add_test(NAME managed_pdb_guard
    COMMAND "${Python3_EXECUTABLE}" "${FROST_SOURCE_DIR}/tests/managed/test_pdb_guard.py")
set_tests_properties(managed_pdb_guard PROPERTIES
    LABELS integration
    RESOURCE_LOCK frost_desktop
    SKIP_RETURN_CODE 77
    ENVIRONMENT "FROST_MANAGED_TEST_DOTNET=${_frost_managed_test_dotnet};FROST_MANAGED_TEST_ROOT=${CMAKE_BINARY_DIR};PYTHONDONTWRITEBYTECODE=1")
# The Unity package's csc.rsp / compiler-argument model (ProjectContext.cs), built with the same SDK.
add_test(NAME unity_compiler_arguments
    COMMAND "${Python3_EXECUTABLE}" "${FROST_SOURCE_DIR}/tests/unity/test_compiler_arguments.py")
set_tests_properties(unity_compiler_arguments PROPERTIES
    LABELS integration
    RESOURCE_LOCK frost_desktop
    SKIP_RETURN_CODE 77
    ENVIRONMENT "FROST_MANAGED_TEST_DOTNET=${_frost_managed_test_dotnet};FROST_MANAGED_TEST_ROOT=${CMAKE_BINARY_DIR};PYTHONDONTWRITEBYTECODE=1")

set(_frost_tool_suites frost_lint licensecheck featurecheck corpus_fetch asset_fetch budgets symbolize
    verify_support frost_perf verify_scripts build_lldb managed_server package_notices hooks)
if(WIN32)
    # Runs Windows PowerShell 5.1 (bootstrap.ps1 and env.ps1); nothing to test on Linux.
    list(APPEND _frost_tool_suites windows_scripts)
endif()
# Each suite runs as a script (its directory goes first on sys.path) in the build directory: ctest
# labels only tests that run there with mod.tools (tests/CMakeLists.txt checks this).
foreach(suite IN LISTS _frost_tool_suites)
    add_test(NAME tools.${suite}
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_${suite}.py -v)
    set_tests_properties(tools.${suite} PROPERTIES
        LABELS unit
        TIMEOUT 120
        # Keep the developer's SDK and corpus locations, the perf results root and verify's own
        # settings (a running verify passes them to its children) out of the tests. FROST_TEST_TREE,
        # FROST_TEST_CONFIG and FROST_CTEST let test_verify_support check this tree's test labels.
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;FROST_QT_ROOT=;FROST_CORPUS_ROOT=;FROST_PERF_ROOT=;FROST_CHANGED_FILES=;FROST_VERIFY_BASE=;FROST_MIRROR=;FROST_TEST_TREE=${CMAKE_BINARY_DIR};FROST_TEST_CONFIG=$<CONFIG>;FROST_CTEST=${CMAKE_CTEST_COMMAND}")
endforeach()
