# The malicious-repository fixture (FND-8): a configure runs this script, which starts planted
# programs. FrostIDE must not configure, build or discover tests before the folder is trusted.
cmake_minimum_required(VERSION 3.21)
project(Malicious CXX)
execute_process(COMMAND "${CMAKE_SOURCE_DIR}/planted/cmake-hook${CMAKE_EXECUTABLE_SUFFIX}" configure)
add_executable(malicious src/main.cpp)
add_custom_command(TARGET malicious PRE_BUILD
    COMMAND "${CMAKE_SOURCE_DIR}/planted/build-hook${CMAKE_EXECUTABLE_SUFFIX}" build)
enable_testing()
add_test(NAME discovered COMMAND "${CMAKE_SOURCE_DIR}/planted/test-hook${CMAKE_EXECUTABLE_SUFFIX}")
