# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::document (ADR 0028):
#   document_unit_tests         the SaveManager on MemFs with a ManualClock (byte-exact saves, disk
#                               full, external changes, merges, autosave in virtual time, local history,
#                               the large-file guard), hot exit on real SQLite journals in scratch
#                               directories, the journal labels, label "unit";
#   document_integration_tests  saves on the real file system (read-only targets, the 128 MiB refusal)
#                               and a kill -9 of document_test_child in the middle of its saves,
#                               label "integration";
#   document_test_child         the console helper the kill test launches (not a test itself).
# The tests see tests/ (support headers) and src/document/src (the private journal labels).

frost_add_test(document_unit_tests
    SOURCES
        autosave_test.cpp
        external_change_test.cpp
        hot_exit_test.cpp
        load_service_test.cpp
        save_manager_test.cpp
    DEPS frost::document)
target_include_directories(document_unit_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/document/src)

# The child the kill test terminates: saves two alternating contents in a loop through the SaveManager.
add_executable(document_test_child document_test_child.cpp)
target_link_libraries(document_test_child PRIVATE frost::document)
frost_apply_compile_options(document_test_child TEST)
frost_copy_runtime_dlls(document_test_child)
if(WIN32)
    target_sources(document_test_child PRIVATE ${FROST_TEST_MANIFEST_RC})
    set_source_files_properties(${FROST_TEST_MANIFEST_RC} PROPERTIES OBJECT_DEPENDS ${FROST_MANIFEST_PATH})
    target_link_options(document_test_child PRIVATE /MANIFEST:NO)
endif()

frost_add_test(document_integration_tests
    SOURCES
        real_disk_test.cpp
        refactoring_transaction_test.cpp
    DEPS frost::document
    LABELS integration)
target_include_directories(document_integration_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/io/src)
target_compile_definitions(document_integration_tests PRIVATE
    FROST_DOCUMENT_TEST_CHILD="$<TARGET_FILE:document_test_child>")
add_dependencies(document_integration_tests document_test_child)
