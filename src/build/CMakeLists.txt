# SPDX-License-Identifier: Apache-2.0
#
# frost::build (PLAN.md section 3.2: build drivers, run configurations), the W1 part (ADR 0032):
# finding MSBuild through vswhere, the MSBuild build driver (Build, Rebuild and Clean of a solution
# or one project, streamed output, cancellation of the whole process tree) and the parser that turns
# MSBuild, cl, link, rc and clang-cl output into diagnostics. Qt-free; depends on io (and core
# through it). The parser, the vswhere JSON reader and the command-line builder are portable; the
# discovery and the driver return Unsupported off Windows.
# TODO(M4): FrostIDE.MSBuildLogger, the CMake and dotnet drivers, run configurations and localized
# output decoding (I12).

set(frost_build_sources
    src/diagnostic.cpp
    src/json.cpp
    src/line_splitter.cpp
    src/msbuild_driver.cpp
    src/msbuild_locator.cpp
    src/session.cpp)

frost_add_library(build LAYER build
    SOURCES ${frost_build_sources}
    DEPS frost::io)
if(WIN32)
    # shell32: SHGetKnownFolderPath (the Program Files (x86) folder that holds vswhere.exe);
    # ole32: CoTaskMemFree for its result.
    target_link_libraries(frost_build PRIVATE shell32 ole32)
endif()
