# The tiny CMake project of build_cmake_tests (ADR 0046): a static library with a generated
# source, an include directory and a define, and an executable that links it.
cmake_minimum_required(VERSION 3.21)
project(Tiny C)

include(cmake/warnings.cmake)

set(tiny_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/version.c")
add_custom_command(
    OUTPUT "${tiny_generated}"
    COMMAND "${CMAKE_COMMAND}" "-DOUT=${tiny_generated}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake"
    VERBATIM)

add_library(core STATIC src/core.c include/core.h "${tiny_generated}")
target_include_directories(core PUBLIC include)
target_compile_definitions(core PUBLIC TINY_CORE=1)
tiny_warnings(core)

add_executable(app app/main.c)
target_link_libraries(app PRIVATE core)
tiny_warnings(app)
