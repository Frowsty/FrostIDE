# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::diag (ADR 0022):
#   diag_tests               pure logic and the doubles: the stall detector and watchdog on a
#                            virtual clock, freeze-report JSON, retention plans and pruning on
#                            MemFs, the cache-root resolver, the crash reporter's start-up with
#                            ScriptedProcessLauncher, the log backend over a file double; "unit";
#   diag_integration_tests   real processes and files: every fatal path of crash_child through a
#                            real crashpad handler and its minidump, the handler's isolation, the
#                            thread sampler against llvm-symbolizer, module identities against
#                            llvm-readobj, retention and logs on disk, and how crashpad was built;
#                            "integration";
#   diag_stress_tests        the thread sampler under heap and loader churn, and the watchdog's
#                            park/wake handshake over many rounds; "stress";
#   crash_child              the helper program the crash tests (and frost_process_tests) run.

frost_add_test(diag_tests
    SOURCES
        crash_reporter_test.cpp
        freeze_report_test.cpp
        log_backend_test.cpp
        paths_test.cpp
        retention_test.cpp
        stall_watchdog_test.cpp
    DEPS frost::diag frost::io frost::core)
target_include_directories(diag_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

# The crash helper: frost::diag's fatal paths exactly as frost's program sets them up, with a
# real crashpad handler copied beside it. crashpad's headers for its database and settings checks.
add_executable(crash_child crash_child.cpp crash_child_quick.cpp)
frost_apply_compile_options(crash_child TEST)
target_link_libraries(crash_child PRIVATE frost::diag frost::io frost::core crashpad::crashpad)
frost_copy_runtime_dlls(crash_child)
frost_deploy_crashpad_handler(crash_child)
if(WIN32)
    # advapi32: the pipe-security mode's GetSecurityInfo and SDDL conversion.
    target_link_libraries(crash_child PRIVATE advapi32)
endif()
# The faulting frames must stay where the tests look for them.
set_source_files_properties(crash_child.cpp crash_child_quick.cpp PROPERTIES COMPILE_OPTIONS
    "$<IF:$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>,/clang:-fno-optimize-sibling-calls,-fno-optimize-sibling-calls>")

set(frost_llvm_symbolizer "${FROST_LLVM_ROOT}/bin/llvm-symbolizer${CMAKE_EXECUTABLE_SUFFIX}")
set(frost_llvm_readobj "${FROST_LLVM_ROOT}/bin/llvm-readobj${CMAKE_EXECUTABLE_SUFFIX}")
foreach(tool IN ITEMS "${frost_llvm_symbolizer}" "${frost_llvm_readobj}")
    if(NOT EXISTS "${tool}")
        message(FATAL_ERROR "${tool} not found (the pinned LLVM ships it; run the bootstrap script)")
    endif()
endforeach()

frost_add_test(diag_integration_tests
    SOURCES
        crash_child_test.cpp
        diag_disk_test.cpp
        thread_sampler_test.cpp
    DEPS frost::diag frost::io frost::core
    LABELS integration)
target_include_directories(diag_integration_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(diag_integration_tests PRIVATE
    FROST_CRASH_CHILD="$<TARGET_FILE:crash_child>"
    FROST_CRASHPAD_HANDLER="${FROST_CRASHPAD_HANDLER_SOURCE}"
    FROST_CRASHPAD_LIBRARY_DIR="${FROST_CRASHPAD_LIBRARY_DIR}"
    FROST_LLVM_SYMBOLIZER="${frost_llvm_symbolizer}"
    FROST_LLVM_READOBJ="${frost_llvm_readobj}")
add_dependencies(diag_integration_tests crash_child)
# The sampled chain must keep its frames (thread_sampler_test.cpp).
set_source_files_properties(thread_sampler_test.cpp PROPERTIES COMPILE_OPTIONS
    "$<IF:$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>,/clang:-fno-optimize-sibling-calls,-fno-optimize-sibling-calls>")
if(WIN32)
    # advapi32: the pipe test's SID and SDDL calls; user32: tests/support/private_desktop.hpp.
    target_link_libraries(diag_integration_tests PRIVATE advapi32 user32)
endif()

frost_add_test(diag_stress_tests
    SOURCES diag_stress_test.cpp
    DEPS frost::diag frost::io frost::core
    LABELS stress)
target_include_directories(diag_stress_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
