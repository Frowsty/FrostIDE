# SPDX-License-Identifier: Apache-2.0
#
# Tests for the repository tools in tools/ (frost-lint, frost-licensecheck,
# frost-featurecheck, corpus_fetch, corpora, asset_fetch, symbolize.py, verify_support.py, frost_perf.py, the
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

set(_frost_tool_suites frost_lint licensecheck featurecheck corpus_fetch corpora asset_fetch budgets symbolize
    verify_support frost_perf verify_scripts build_lldb managed_server package_notices hooks)
if(WIN32)
    # Runs Windows PowerShell 5.1 (bootstrap.ps1 and env.ps1); nothing to test on Linux.
    list(APPEND _frost_tool_suites windows_scripts)
else()
    # tools/stage.sh on fixture trees (bash, ldd); the Windows stage is windows_scripts'.
    list(APPEND _frost_tool_suites stage_sh)
endif()
# The R0 exit criterion (test_staged_luals.py): this tree's Debug build staged with the bundled tools,
# frost run offscreen on a fresh profile, LuaLS started from the stage and its diagnostic shown.
# Integration: it stages a few hundred MB and runs frost, LuaLS and the crash handler (frost_desktop).
# A non-Debug tree skips (exit 77: the spawn log is in Debug frost only).
add_test(NAME tools.staged_luals
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_staged_luals.py -v)
set_tests_properties(tools.staged_luals PROPERTIES
    LABELS integration
    TIMEOUT 900
    RESOURCE_LOCK frost_desktop
    SKIP_RETURN_CODE 77
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;FROST_QT_ROOT=;FROST_CORPUS_ROOT=;FROST_PERF_ROOT=;FROST_CHANGED_FILES=;FROST_VERIFY_BASE=;FROST_MIRROR=;FROST_TEST_TREE=${CMAKE_BINARY_DIR};FROST_TEST_CONFIG=$<CONFIG>")

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
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;FROST_QT_ROOT=;FROST_CORPUS_ROOT=;FROST_CORPORA_ROOT=;FROST_PERF_ROOT=;FROST_CHANGED_FILES=;FROST_VERIFY_BASE=;FROST_MIRROR=;FROST_TEST_TREE=${CMAKE_BINARY_DIR};FROST_TEST_CONFIG=$<CONFIG>;FROST_CTEST=${CMAKE_CTEST_COMMAND}")
endforeach()

# The automation endpoint is in Debug test builds only (FND-7a, ADR 0080): the image that holds it
# (frost_app.dll on Windows, frost on Linux) is probed for its strings in every configuration.
if(FROST_BUILD_UI)
    if(WIN32)
        set(_frost_endpoint_image $<TARGET_FILE:frost_app_shared>)
    else()
        set(_frost_endpoint_image $<TARGET_FILE:frost>)
    endif()
    if(FROST_ENABLE_AUTOMATION_ENDPOINT)
        set(_frost_endpoint_enabled ON)
    else()
        set(_frost_endpoint_enabled OFF)
    endif()
    add_test(NAME tools.automation_release_probe
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_automation_release_probe.py
                "${_frost_endpoint_image}" $<CONFIG> ${_frost_endpoint_enabled} -v)
    set_tests_properties(tools.automation_release_probe PROPERTIES
        LABELS unit
        TIMEOUT 120
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()
