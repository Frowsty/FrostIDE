# SPDX-License-Identifier: Apache-2.0
#
# Test suites, one directory per module plus tests of the build (cmake), the
# repository tools (tools) and the toolchain (toolchain_probe). frost_add_test
# labels tests "unit" by default; process-level tests are "integration", long runs
# "stress", timing tests "perf". The verify scripts' debug leg excludes "stress"
# (-LE '^stress$'); the -Full legs run every label. tests/fuzz builds the corpus replays here
# (label unit); the libFuzzer targets themselves live in the separate fuzz trees (FROST_FUZZ,
# label fuzz). diff comes before fuzz because tests/fuzz reuses its support library.
#
# Every test also carries the module label mod.<directory> (mod.sched, mod.io, ...), which the
# WSL TSan quick leg selects by (tools/verify_support.py tsan; ADR 0027 decision 5).

# Adds tests/<dir> and labels every test in it and in its subdirectories mod.<dir>, including the
# tests gtest_discover_tests adds at ctest time. CMAKE_DIRECTORY_LABELS is function-scoped here, so
# it reaches <dir>'s whole subtree and nothing else. Side effect: appends the directory to the
# global FROST_TEST_DIRECTORIES, which the guard at the end of this file checks.
function(frost_add_test_directory dir)
    set(CMAKE_DIRECTORY_LABELS mod.${dir})
    add_subdirectory(${dir})
    set_property(GLOBAL APPEND PROPERTY FROST_TEST_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/${dir})
endfunction()

frost_add_test_directory(compat)
frost_add_test_directory(core)
frost_add_test_directory(sched)
frost_add_test_directory(io)
frost_add_test_directory(text)
frost_add_test_directory(diff)
frost_add_test_directory(input)
frost_add_test_directory(syntax)
frost_add_test_directory(storage)
frost_add_test_directory(settings)
frost_add_test_directory(project)
frost_add_test_directory(document)
frost_add_test_directory(build)
frost_add_test_directory(lang)
frost_add_test_directory(search)
if(NOT FROST_FUZZ)
    # frost::diag is not built in fuzz trees (src/CMakeLists.txt).
    frost_add_test_directory(diag)
endif()
frost_add_test_directory(fuzz)
frost_add_test_directory(perf)
frost_add_test_directory(toolchain_probe)
frost_add_test_directory(cmake)
frost_add_test_directory(tools)
if(FROST_BUILD_UI)
    frost_add_test_directory(ui)
    frost_add_test_directory(app)
endif()

# Every test directory must come in through frost_add_test_directory, or its tests would have
# no module label and the TSan quick leg could never select them.
get_property(_frost_test_dirs GLOBAL PROPERTY FROST_TEST_DIRECTORIES)
get_property(_frost_subdirs DIRECTORY PROPERTY SUBDIRECTORIES)
foreach(_frost_subdir IN LISTS _frost_subdirs)
    if(NOT _frost_subdir IN_LIST _frost_test_dirs)
        message(FATAL_ERROR "tests/CMakeLists.txt: ${_frost_subdir} was added with add_subdirectory; "
                            "use frost_add_test_directory so its tests get a mod.* label (ADR 0027)")
    endif()
endforeach()

# ctest gives a directory's labels only to the tests whose working directory is that directory's
# build directory (CMake 4.4.5, checked with a probe project for ADR 0027), so a test with another
# WORKING_DIRECTORY would silently lose its mod.* label. Fails the configure for one, in `dir` or
# below. Tests that gtest_discover_tests adds at ctest time are not visible here; frost_add_test
# leaves their working directory at the default, the build directory.
function(_frost_check_test_working_directories dir)
    get_property(binary DIRECTORY ${dir} PROPERTY BINARY_DIR)
    get_property(tests DIRECTORY ${dir} PROPERTY TESTS)
    foreach(test IN LISTS tests)
        get_test_property(${test} WORKING_DIRECTORY DIRECTORY ${dir} working)
        if(working)
            cmake_path(NORMAL_PATH working)
            cmake_path(NORMAL_PATH binary OUTPUT_VARIABLE expected)
            if(NOT working STREQUAL expected)
                message(FATAL_ERROR "tests/CMakeLists.txt: test ${test} runs in ${working}; ctest would not give "
                                    "it its directory's mod.* label, so it must run in ${expected} (ADR 0027)")
            endif()
        endif()
    endforeach()
    get_property(subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach(subdir IN LISTS subdirs)
        _frost_check_test_working_directories(${subdir})
    endforeach()
endfunction()
foreach(_frost_test_dir IN LISTS _frost_test_dirs)
    _frost_check_test_working_directories(${_frost_test_dir})
endforeach()
