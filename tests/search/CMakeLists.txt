# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::search (ADR 0045): search_tests runs the Find in Files engine and Replace in
# Files on scratch trees on the real file system (UTF-8, UTF-8 BOM, UTF-16LE and windows-1252 files,
# a binary file, read-only targets) and over the tiny MSBuild fixture (tests/build/fixtures), label
# "unit": every test is fast and needs no other process.

frost_add_test(search_tests
    SOURCES
        find_in_files_test.cpp
        replace_in_files_test.cpp
    DEPS frost::search frost::document frost::project frost::storage frost::sched frost::io)
target_include_directories(search_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(search_tests PRIVATE
    FROST_TINY_SLN_DIR="${FROST_SOURCE_DIR}/tests/build/fixtures/tiny_sln")
