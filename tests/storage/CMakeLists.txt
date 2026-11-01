# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::storage (ADR 0014):
#   storage_test_support       static library: the fault/recording VFS shim and the retention oracle;
#   storage_unit_tests         wrapper, schema, stores, fingerprints, fault injection, label "unit";
#   storage_integration_tests  crash (kill -9 of a child) and concurrency tests, label "integration";
#   storage_stress_tests       the same at scale (FROST_STRESS_FULL=1 for the exit scale), label "stress";
#   storage_test_child         the helper program the crash tests launch and kill (not a test itself).
# Every target sees tests/ (support headers) and src/storage/src (private headers for white-box
# tests), and links SQLite directly as the raw-API oracle.
# frost_add_test cannot set a TIMEOUT and the test preset's limit is 300 s, so every stress case
# stays at about 120 s or less under ASan; FROST_STRESS_FULL adds shards instead of lengthening cases.

set(frost_storage_test_includes ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/storage/src
                                ${CMAKE_CURRENT_SOURCE_DIR})

add_library(storage_test_support STATIC support/fault_vfs.cpp)
target_link_libraries(storage_test_support PUBLIC frost::storage unofficial::sqlite3::sqlite3 GTest::gtest)
target_include_directories(storage_test_support PUBLIC ${frost_storage_test_includes})
frost_apply_compile_options(storage_test_support TEST)

frost_add_test(storage_unit_tests
    SOURCES
        build_test.cpp
        database_test.cpp
        fault_vfs_test.cpp
        fingerprint_test.cpp
        hot_exit_journal_test.cpp
        local_history_test.cpp
        retention_property_test.cpp
        schema_test.cpp
        sqlite_error_test.cpp
        statement_test.cpp
        store_test.cpp
        transaction_test.cpp
    DEPS frost::storage
    THIRD_PARTY unofficial::sqlite3::sqlite3 xxHash::xxhash)
target_link_libraries(storage_unit_tests PRIVATE storage_test_support)
target_compile_definitions(storage_unit_tests PRIVATE FROST_STORAGE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/goldens")

# The child the crash tests kill: built with the test flags, UTF-8 argv and code page on Windows.
add_executable(storage_test_child storage_test_child.cpp)
target_link_libraries(storage_test_child PRIVATE frost::storage unofficial::sqlite3::sqlite3)
target_include_directories(storage_test_child PRIVATE ${frost_storage_test_includes})
frost_apply_compile_options(storage_test_child TEST)
frost_copy_runtime_dlls(storage_test_child)
if(WIN32)
    target_sources(storage_test_child PRIVATE ${FROST_TEST_MANIFEST_RC})
    set_source_files_properties(${FROST_TEST_MANIFEST_RC} PROPERTIES OBJECT_DEPENDS ${FROST_MANIFEST_PATH})
    target_link_options(storage_test_child PRIVATE /MANIFEST:NO)
endif()

frost_add_test(storage_integration_tests
    SOURCES
        concurrency_test.cpp
        crash_test.cpp
    DEPS frost::storage
    THIRD_PARTY unofficial::sqlite3::sqlite3
    LABELS integration)
target_link_libraries(storage_integration_tests PRIVATE storage_test_support)
target_compile_definitions(storage_integration_tests PRIVATE
    FROST_STORAGE_TEST_CHILD="$<TARGET_FILE:storage_test_child>")
add_dependencies(storage_integration_tests storage_test_child)

frost_add_test(storage_stress_tests
    SOURCES
        measure_test.cpp
        stress_test.cpp
    DEPS frost::storage
    THIRD_PARTY unofficial::sqlite3::sqlite3
    LABELS stress)
target_link_libraries(storage_stress_tests PRIVATE storage_test_support)
target_compile_definitions(storage_stress_tests PRIVATE
    FROST_STORAGE_TEST_CHILD="$<TARGET_FILE:storage_test_child>")
add_dependencies(storage_stress_tests storage_test_child)
foreach(test_target storage_unit_tests storage_integration_tests storage_stress_tests)
    target_include_directories(${test_target} PRIVATE ${frost_storage_test_includes})
endforeach()
