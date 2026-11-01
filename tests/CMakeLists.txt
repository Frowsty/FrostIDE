# SPDX-License-Identifier: Apache-2.0
#
# Test suites, one directory per module plus tests of the build (cmake), the
# repository tools (tools) and the toolchain (toolchain_probe). frost_add_test
# labels tests "unit" by default; process-level tests are "integration", long runs
# "stress", timing tests "perf". The verify scripts' debug leg excludes "stress"
# (-LE stress); the -Full legs run every label. tests/fuzz builds the corpus replays here
# (label unit); the libFuzzer targets themselves live in the separate fuzz trees (FROST_FUZZ,
# label fuzz).
add_subdirectory(compat)
add_subdirectory(core)
add_subdirectory(sched)
add_subdirectory(io)
add_subdirectory(text)
add_subdirectory(input)
add_subdirectory(storage)
add_subdirectory(project)
add_subdirectory(settings)
add_subdirectory(lang)
add_subdirectory(fuzz)
add_subdirectory(perf)
add_subdirectory(toolchain_probe)
add_subdirectory(cmake)
add_subdirectory(tools)
if(FROST_BUILD_UI)
    add_subdirectory(ui)
    add_subdirectory(app)
endif()
