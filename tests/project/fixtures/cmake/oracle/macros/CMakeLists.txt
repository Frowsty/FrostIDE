# The macros oracle of project_cmake_tests (CMK-2): a project without languages that prints, for
# `cmake --preset <name>`, every ORACLE_* cache variable, every ORACLE_ENV_* environment variable
# (as `cmake -E environment` sees the configure's environment), the binary directory and the
# install prefix. The test compares these lines with FrostIDE's resolution of the same preset.
cmake_minimum_required(VERSION 3.25)
project(MacrosOracle NONE)

get_cmake_property(_oracle_variables CACHE_VARIABLES)
list(SORT _oracle_variables)
foreach(_oracle_variable IN LISTS _oracle_variables)
    if(_oracle_variable MATCHES "^ORACLE_")
        message(STATUS "ORACLE-CACHE ${_oracle_variable}=${${_oracle_variable}}")
    endif()
endforeach()

execute_process(COMMAND "${CMAKE_COMMAND}" -E environment OUTPUT_VARIABLE _oracle_environment)
string(REPLACE "\r" "" _oracle_environment "${_oracle_environment}")
string(REPLACE "\n" ";" _oracle_environment "${_oracle_environment}")
foreach(_oracle_line IN LISTS _oracle_environment)
    if(_oracle_line MATCHES "^ORACLE_ENV_")
        message(STATUS "ORACLE-ENV ${_oracle_line}")
    endif()
endforeach()

message(STATUS "ORACLE-BINARY ${CMAKE_BINARY_DIR}")
message(STATUS "ORACLE-INSTALL ${CMAKE_INSTALL_PREFIX}")
