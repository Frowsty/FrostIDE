# The build-presets project of build_cmake_tests (CMK-1): two executables that the build presets
# build one at a time, and one that warns and fails at known lines (the Problems check). The test
# writes CMakePresets.json with the compilers it finds.
cmake_minimum_required(VERSION 3.21)
project(PresetsBuild C)

add_executable(first first.c)
add_executable(second second.c)
add_executable(broken EXCLUDE_FROM_ALL broken.c)
if(MSVC)
    target_compile_options(broken PRIVATE /W4)
else()
    target_compile_options(broken PRIVATE -Wall)
endif()
