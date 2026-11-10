# SPDX-License-Identifier: Apache-2.0
# Offline signature/payload verification, HTTP client/policy, and safe updater rollback tests.
add_executable(update_test_app_helper update_test_app_helper.cpp)
frost_apply_compile_options(update_test_app_helper TEST NO_SANITIZE)
frost_copy_runtime_dlls(update_test_app_helper)

frost_add_test(update_unit_tests
    SOURCES
        verification_test.cpp
        parent_process_test.cpp
        payload_guard_test.cpp
        http_client_test.cpp
        safe_updater_test.cpp
    DEPS frost::update frost::io
    THIRD_PARTY unofficial-sodium::sodium)
target_compile_definitions(update_unit_tests PRIVATE
    FROST_UPDATE_TEST_HELPER_PATH="$<TARGET_FILE:update_test_app_helper>")
add_dependencies(update_unit_tests update_test_app_helper)

if(WIN32)
    target_link_libraries(update_unit_tests PRIVATE msi ws2_32)
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
