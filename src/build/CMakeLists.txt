# SPDX-License-Identifier: Apache-2.0
#
# frost::build (PLAN.md section 3.2: build drivers, run configurations), the W1 part (ADR 0032):
# finding MSBuild through vswhere, the MSBuild build driver (Build, Rebuild and Clean of a solution
# or one project, streamed output, cancellation of the whole process tree), the parser that turns
# MSBuild, cl, link, rc and clang-cl output into diagnostics, and the BuildService the UI binds to
# (active configuration, trust gate, Save All, one build at a time, resolved diagnostics; ADR 0035),
# and the RunService that builds and runs the startup project (MSBuild -getProperty, the Run
# console and the external console; ADR 0040). R3 adds the single-file compile (SLN-9: MSBuild's
# ClCompile target with SelectedFiles from -getItem) and the .vcxproj.user writer (SLN-11).
# Qt-free; depends on project (the SolutionModel and the trust store) and io (and core through
# them). The parser, the vswhere JSON reader, the command-line builder and the service are
# portable; the discovery and the driver return Unsupported off Windows.
# The M2 CMake part (ADR 0046): cmake and ninja discovery, the vcvarsall.bat environment capture,
# the GNU/Clang and CMake message parsers, and the CMakeService (profiles, background configure,
# File API model, reconfigure on change, cmake --build). It also depends on settings (the
# cmake.profiles records).
# SLN-14 (R3): the toolchain discovery core (toolchain.hpp): MSVC toolsets, platform toolsets and
# Windows SDKs, shared by the CMake and MSBuild paths.
# TODO(M4): FrostIDE.MSBuildLogger, the dotnet driver, full run configurations and localized
# output decoding (I12).

set(frost_build_sources
    src/build_diagnostics.cpp
    src/build_service.cpp
    src/cmake_service.cpp
    src/cmake_service_jobs.cpp
    src/cmake_service_profiles.cpp
    src/cmake_toolchain.cpp
    src/diagnostic.cpp
    src/dependencies.cpp
    src/json.cpp
    src/line_splitter.cpp
    src/managed_project.cpp
    src/msbuild_compile.cpp
    src/msbuild_driver.cpp
    src/msbuild_locator.cpp
    src/output_parsers.cpp
    src/run_service.cpp
    src/run_session.cpp
    src/run_target.cpp
    src/session.cpp
    src/toolchain.cpp
    src/vcxproj_user.cpp)

frost_add_library(build LAYER build
    SOURCES ${frost_build_sources}
    DEPS frost::project frost::settings frost::io frost::sched)
# The cmake and Ninja versions pinned in sdk/sdk.lock.json, so findCMakeTools can name the pinned
# SDK's tools (cmake_toolchain.hpp, step 2).
file(READ "${FROST_SOURCE_DIR}/sdk/sdk.lock.json" _frost_build_sdk_lock)
if(WIN32)
    set(_frost_build_lock_platform windows)
else()
    set(_frost_build_lock_platform linux)
endif()
string(JSON _frost_pinned_cmake_version GET "${_frost_build_sdk_lock}" ${_frost_build_lock_platform} cmake version)
string(JSON _frost_pinned_ninja_version GET "${_frost_build_sdk_lock}" ${_frost_build_lock_platform} ninja version)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FROST_SOURCE_DIR}/sdk/sdk.lock.json")
target_compile_definitions(frost_build PRIVATE FROST_PINNED_CMAKE_VERSION="${_frost_pinned_cmake_version}"
    FROST_PINNED_NINJA_VERSION="${_frost_pinned_ninja_version}")
# pugixml (ADR 0033), private as in frost::project: the .vcxproj.user writer (SLN-11) edits the
# per-user file's XML; no header above build sees it.
target_link_libraries(frost_build PRIVATE pugixml::pugixml)
if(WIN32)
    # shell32: SHGetFolderPathW (the Program Files (x86) folder that holds vswhere.exe); advapi32:
    # RegGetValueW (the Windows SDK root, toolchain.hpp).
    target_link_libraries(frost_build PRIVATE shell32 advapi32)
endif()
