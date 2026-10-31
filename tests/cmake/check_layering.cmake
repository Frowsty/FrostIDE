# SPDX-License-Identifier: Apache-2.0
#
# Negative test for FrostLayers.cmake (run with cmake -P from tests/cmake). It
# configures tests/cmake/layering_fixture with the build's own toolchain, then
# requires that configure failed, that every planted violation produced its
# diagnostic, and that the valid ok_* targets produced none.
# Inputs (-D): FROST_SOURCE_DIR, WORK_DIR, GENERATOR, MAKE_PROGRAM, TOOLCHAIN.
file(REMOVE_RECURSE "${WORK_DIR}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${FROST_SOURCE_DIR}/tests/cmake/layering_fixture" -B "${WORK_DIR}"
            -G "${GENERATOR}" "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}" "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN}"
            "-DFROST_SOURCE_DIR=${FROST_SOURCE_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
string(REGEX REPLACE "[ \t\r\n]+" " " flat "${output}")
if(result EQUAL 0)
    message(FATAL_ERROR "configure succeeded although the fixture violates the layering:\n${output}")
endif()
set(expected
    "frost_bad_upward (layer core, rank 1) may not depend on frost::ok_io"
    "frost_bad_qt (layer io) may not link Qt"
    "frost_bad_smuggle: 'frost::ok_io' is a FrostIDE library"
    "frost_bad_wrapper (layer io) reaches Qt (Qt6::Core)"
    "frost_bad_raw (layer core) links frost_ok_io (layer io)")
foreach(text IN LISTS expected)
    string(FIND "${flat}" "${text}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "missing diagnostic: '${text}'\nconfigure output:\n${output}")
    endif()
endforeach()
# Diagnostics name their owner after "violation: " or as "<owner>: '"; a valid
# target may still appear as the *dependency* of a bad one.
string(REGEX MATCH "violation: frost_ok_[a-z]+|frost_ok_[a-z]+: '" valid_hit "${flat}")
if(valid_hit)
    message(FATAL_ERROR "a valid target was reported (${valid_hit}):\n${output}")
endif()
message(STATUS "FrostLayers: all planted violations reported")
