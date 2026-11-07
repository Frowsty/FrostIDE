# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::io:
#   io_unit_tests          pure logic (including private helpers of src/io/src/internal.hpp), the
#                          doubles (MemFs, MemFsWatcher, ScriptedProcessLauncher with its async
#                          processes), the spawn-log decorator and SHA-256 (FND-8), the change
#                          coalescer, the reactor, the gitignore matcher and
#                          the tree scan on MemFs, label "unit";
#   io_integration_tests   the real file system, real watchers and real processes (blocking and
#                          async), and the gitignore matcher against git, label "integration";
#   io_stress_tests        watcher stress (many files, rename storms, overflow, churn), the async pipe
#                          fault suite and the sharded coalescer property seeds, label "stress"
#                          (FROST_STRESS_FULL=1: the full scale);
#   io_perf_tests          watcher latency and costs for ADR 0020, report only, label "perf";
#   io_test_child          the helper program the process tests launch (not a test itself).
# The tests include src/io/src for the private headers they test (internal.hpp, change_coalescer.hpp,
# watch_queue.hpp, watcher_testing.hpp), never reactor_impl.hpp (asio).

frost_add_test(io_unit_tests
    SOURCES
        change_coalescer_test.cpp
        ignore_rules_test.cpp
        mem_fs_test.cpp
        mem_fs_watcher_test.cpp
        os_error_test.cpp
        path_test.cpp
        process_spec_test.cpp
        reactor_test.cpp
        replacement_mode_test.cpp
        scripted_async_process_test.cpp
        scripted_process_test.cpp
        spawn_log_test.cpp
        temp_files_test.cpp
        tree_scan_test.cpp
        utf_test.cpp
        watch_queue_test.cpp
    DEPS frost::io frost::sched)
target_include_directories(io_unit_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/io/src)

# 200k more coalescer property seeds (the unit run covers the first 1k: 250 per event flavour),
# as 4 shards of 50k: each shard is one ctest entry running every flavour over its own seed range of
# 12,500 seeds (FROST_PROPERTY_SEEDS counts per flavour).
foreach(_frost_shard RANGE 3)
    math(EXPR _frost_offset "250 + ${_frost_shard} * 12500")
    add_test(NAME ChangeCoalescerPropertyShard.${_frost_shard}
        COMMAND io_unit_tests --gtest_filter=ChangeCoalescerProperty.*)
    set_tests_properties(ChangeCoalescerPropertyShard.${_frost_shard} PROPERTIES
        LABELS stress
        ENVIRONMENT "FROST_PROPERTY_SEEDS=12500;FROST_PROPERTY_SEED_OFFSET=${_frost_offset}")
endforeach()

if(WIN32)
    # CommandLineToArgvW, the oracle for the Windows quoting tests.
    target_link_libraries(io_unit_tests PRIVATE shell32)
endif()

# A plain console program built with the test flags; it uses only the OS, the C runtime and the
# C++ standard library (no frost code), so it is an independent oracle for argv, environment and
# working-directory handling.
add_executable(io_test_child io_test_child.cpp)
frost_apply_compile_options(io_test_child TEST)
# In ASan builds the child needs the ASan runtime DLL next to it like the tests do.
frost_copy_runtime_dlls(io_test_child)
if(WIN32)
    # shell32: CommandLineToArgvW; advapi32: the pipe-info mode's security descriptor calls;
    # user32: the desktop mode's window-station and desktop names and the window mode's window.
    target_link_libraries(io_test_child PRIVATE shell32 advapi32 user32)
endif()

add_executable(local_ipc_test_child local_ipc_test_child.cpp)
frost_apply_compile_options(local_ipc_test_child TEST)
target_link_libraries(local_ipc_test_child PRIVATE frost::io)
frost_copy_runtime_dlls(local_ipc_test_child)

# RESOURCE_LOCK frost_desktop, the integration label's default (frost_add_test's policy; ADR 0027
# decision 3): the process tests create private desktops and windows and start io_test_child on
# them, as the tests/app process tests do, so the executables holding the lock never overlap.
# io_unit_tests (pure logic and the doubles) takes none.
frost_add_test(io_integration_tests
    SOURCES
        async_process_test.cpp
        file_system_test.cpp
        file_watcher_test.cpp
        ignore_rules_git_test.cpp
        local_ipc_test.cpp
        loopback_stream_test.cpp
        polling_watcher_test.cpp
        process_test.cpp
        safe_write_test.cpp
    DEPS frost::io frost::sched
    LABELS integration)
# src/io/src: the tests name detail::kAtomicTempMarker, lower detail::maxWaitSliceMs() and use the
# watcher test hooks (watcher_testing.hpp).
target_include_directories(io_integration_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/io/src)
target_compile_definitions(io_integration_tests PRIVATE FROST_IO_TEST_CHILD="$<TARGET_FILE:io_test_child>"
    FROST_LOCAL_IPC_TEST_CHILD="$<TARGET_FILE:local_ipc_test_child>")
add_dependencies(io_integration_tests local_ipc_test_child)
add_dependencies(io_integration_tests io_test_child)
if(WIN32)
    # advapi32: the pipe test's token and SID calls; user32: tests/support/private_desktop.hpp and
    # the window test's EnumDesktopWindows.
    target_link_libraries(io_integration_tests PRIVATE advapi32 user32)
endif()

frost_add_test(io_stress_tests
    SOURCES
        async_process_fault_test.cpp
        watcher_stress_test.cpp
    DEPS frost::io frost::sched
    LABELS stress)
target_include_directories(io_stress_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/io/src)
target_compile_definitions(io_stress_tests PRIVATE FROST_IO_TEST_CHILD="$<TARGET_FILE:io_test_child>")
add_dependencies(io_stress_tests io_test_child)

frost_add_test(io_perf_tests
    SOURCES
        watcher_perf_test.cpp
    DEPS frost::io frost::sched
    LABELS perf)
target_include_directories(io_perf_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/io/src)
