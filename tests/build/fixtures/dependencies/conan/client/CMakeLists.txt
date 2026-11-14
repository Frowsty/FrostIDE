# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(FrostLocalDependency LANGUAGES CXX)
find_package(frost-local CONFIG REQUIRED)
add_executable(local_dependency main.cpp)
target_link_libraries(local_dependency PRIVATE frost-local::frost-local)
