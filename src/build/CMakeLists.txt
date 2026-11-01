# SPDX-License-Identifier: Apache-2.0
#
# frost::build (PLAN.md section 3.2: build drivers, run configurations), the W1 part (ADR 0032):
# finding MSBuild through vswhere, the MSBuild build driver (Build, Rebuild and Clean of a solution
# or one project, streamed output, cancellation of the whole process tree), the parser that turns
# MSBuild, cl, link, rc and clang-cl output into diagnostics, and the BuildService the UI binds to
# (active configuration, trust gate, Save All, one build at a time, resolved diagnostics; ADR 0035).
# Qt-free; depends on project (the SolutionModel and the trust store) and io (and core through
# them). The parser, the vswhere JSON reader, the command-line builder and the service are
# portable; the discovery and the driver return Unsupported off Windows.
# TODO(M4): FrostIDE.MSBuildLogger, the CMake and dotnet drivers, run configurations and localized
# output decoding (I12).

set(frost_build_sources
    src/build_diagnostics.cpp
    src/build_service.cpp
    src/diagnostic.cpp
    src/json.cpp
    src/line_splitter.cpp
    src/msbuild_driver.cpp
    src/msbuild_locator.cpp
    src/session.cpp)

frost_add_library(build LAYER build
    SOURCES ${frost_build_sources}
    DEPS frost::project frost::io)
if(WIN32)
    # shell32: SHGetKnownFolderPath (the Program Files (x86) folder that holds vswhere.exe);
    # ole32: CoTaskMemFree for its result.
    target_link_libraries(frost_build PRIVATE shell32 ole32)
endif()
