# SPDX-License-Identifier: Apache-2.0
# Offline signature and payload verification tests with an ephemeral fixture key.
frost_add_test(update_unit_tests SOURCES verification_test.cpp parent_process_test.cpp payload_guard_test.cpp
    DEPS frost::update THIRD_PARTY unofficial-sodium::sodium)
if(WIN32)
    target_link_libraries(update_unit_tests PRIVATE msi)
    if(NOT Python3_EXECUTABLE AND DEFINED ENV{FROST_SDK} AND EXISTS "$ENV{FROST_SDK}/py/Scripts/python.exe")
        set(Python3_EXECUTABLE "$ENV{FROST_SDK}/py/Scripts/python.exe")
    endif()
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    add_test(NAME UpdateParent.RealProcessExitBarrier
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/parent_process_test.py"
                "$<TARGET_FILE:update_unit_tests>")
    set_tests_properties(UpdateParent.RealProcessExitBarrier PROPERTIES LABELS integration TIMEOUT 45
        RESOURCE_LOCK frost_desktop)
endif()
