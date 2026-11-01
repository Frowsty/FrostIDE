# SPDX-License-Identifier: Apache-2.0
#
# FrostFuzz.cmake: fuzz targets (PLAN.md sections 6.1 and 6.3; ADR 0013). Every target exists in
# two forms, chosen by FROST_FUZZ (root CMakeLists.txt):
#   - in a fuzz tree (FROST_FUZZ ON, tests off; presets win-clangcl-fuzz and linux-clang-fuzz) a
#     libFuzzer executable, run by CTest fuzz.<name> (label fuzz) for FROST_FUZZ_SECONDS;
#   - in a normal tree with tests on, a corpus replay, run by CTest fuzz_replay.<name> (label
#     unit), so the committed corpus is a regression test in every -Quick on both OSes.
# TODO(M2): add the x64-windows-clangcl-fuzz triplet (PLAN.md) when a fuzz target first links a
# port such as pugixml; until then fuzz targets link only FrostIDE code and vcpkg is not installed
# in fuzz trees (VCPKG_MANIFEST_INSTALL=OFF).

include_guard(GLOBAL)
include(FrostLayers)

# frost_add_fuzz_target(<name> SOURCES <name>_harness.cpp <name>_fuzzer.cpp [more...]
#                       DEPS <libraries...> CORPUS <directory>)
#
# Parameters:
#   SOURCES  the harness, which defines frost::fuzz::initialize() and runInput()
#            (tests/fuzz/fuzz_harness.hpp) and keeps no state between inputs, and
#            <name>_fuzzer.cpp with libFuzzer's LLVMFuzzerInitialize/LLVMFuzzerTestOneInput.
#            Every source except <name>_fuzzer.cpp also goes into the replay.
#   DEPS     libraries linked PRIVATE (no gtest: the fuzz trees build without it).
#   CORPUS   the committed seed corpus, read-only for both forms.
#
# Side effects with FROST_FUZZ ON: creates executable <name>_fuzzer (TEST compile options, the
# ASan runtime DLL copied beside it, the test manifest on Windows, libFuzzer linked with
# -fsanitize=fuzzer on Linux or FROST_FUZZER_LINK_OPTIONS on Windows); makes the directories
# ${CMAKE_BINARY_DIR}/fuzz-artifacts/<name> (crash artifacts, kept across runs) and
# fuzz-work/<name> (new inputs: libFuzzer writes into its first corpus directory, so new inputs
# never touch the committed one) at configure time; registers CTest fuzz.<name> with TIMEOUT
# FROST_FUZZ_SECONDS + 120, which overrides the test presets' 300 s default, and its setup
# fixture fuzz.<name>.reset (cmake/FrostFuzzResetWork.cmake), which empties fuzz-work/<name>
# first, so each run starts from the committed corpus alone (both label fuzz; CTest adds the
# fixture whenever fuzz.<name> is selected).
#
# Side effects otherwise, when FROST_BUILD_TESTS is ON: creates executable <name>_replay from the
# harness sources and tests/fuzz/replay_main.cpp (TEST options, test manifest on Windows) and
# registers CTest fuzz_replay.<name> <CORPUS> (label unit). With tests off it does nothing.
function(frost_add_fuzz_target name)
    cmake_parse_arguments(ARG "" "CORPUS" "SOURCES;DEPS" ${ARGN})
    if(NOT ARG_CORPUS OR NOT IS_DIRECTORY "${ARG_CORPUS}")
        message(FATAL_ERROR "frost_add_fuzz_target(${name}): CORPUS '${ARG_CORPUS}' is not a directory")
    endif()
    set(fuzzer_source "")
    set(harness_sources "")
    foreach(source IN LISTS ARG_SOURCES)
        get_filename_component(file "${source}" NAME)
        if(file STREQUAL "${name}_fuzzer.cpp")
            set(fuzzer_source "${source}")
        else()
            list(APPEND harness_sources "${source}")
        endif()
    endforeach()
    if(NOT fuzzer_source OR NOT harness_sources)
        message(FATAL_ERROR "frost_add_fuzz_target(${name}): SOURCES needs ${name}_fuzzer.cpp and a harness")
    endif()

    if(FROST_FUZZ)
        set(target ${name}_fuzzer)
        add_executable(${target} ${harness_sources} ${fuzzer_source})
        target_link_libraries(${target} PRIVATE ${ARG_DEPS})
        target_include_directories(${target} PRIVATE ${FROST_SOURCE_DIR}/tests)
        frost_apply_compile_options(${target} TEST)
        frost_copy_runtime_dlls(${target})
        if(WIN32)
            target_sources(${target} PRIVATE ${FROST_TEST_MANIFEST_RC})
            set_source_files_properties(${FROST_TEST_MANIFEST_RC} PROPERTIES OBJECT_DEPENDS ${FROST_MANIFEST_PATH})
            target_link_options(${target} PRIVATE /MANIFEST:NO ${FROST_FUZZER_LINK_OPTIONS})
        else()
            target_link_options(${target} PRIVATE -fsanitize=fuzzer)
        endif()
        set(artifacts "${CMAKE_BINARY_DIR}/fuzz-artifacts/${name}")
        set(work "${CMAKE_BINARY_DIR}/fuzz-work/${name}")
        file(MAKE_DIRECTORY "${artifacts}" "${work}")
        add_test(NAME fuzz.${name}
            COMMAND ${target}
                -max_total_time=${FROST_FUZZ_SECONDS} -timeout=10 -rss_limit_mb=2048 -max_len=16384
                -print_final_stats=1 -artifact_prefix=${artifacts}/
                ${work} ${ARG_CORPUS})
        math(EXPR timeout "${FROST_FUZZ_SECONDS} + 120")
        set_tests_properties(fuzz.${name} PROPERTIES LABELS fuzz TIMEOUT ${timeout}
            FIXTURES_REQUIRED fuzz.${name}.work)
        # A hermetic smoke: without this, each run would start from every earlier run's inputs.
        add_test(NAME fuzz.${name}.reset
            COMMAND ${CMAKE_COMMAND} -DFROST_FUZZ_WORK=${work} -P ${FROST_SOURCE_DIR}/cmake/FrostFuzzResetWork.cmake)
        set_tests_properties(fuzz.${name}.reset PROPERTIES LABELS fuzz FIXTURES_SETUP fuzz.${name}.work)
    elseif(FROST_BUILD_TESTS)
        set(target ${name}_replay)
        add_executable(${target} ${harness_sources} ${FROST_SOURCE_DIR}/tests/fuzz/replay_main.cpp)
        target_link_libraries(${target} PRIVATE ${ARG_DEPS})
        target_include_directories(${target} PRIVATE ${FROST_SOURCE_DIR}/tests)
        frost_apply_compile_options(${target} TEST)
        frost_copy_runtime_dlls(${target})
        if(WIN32)
            target_sources(${target} PRIVATE ${FROST_TEST_MANIFEST_RC})
            set_source_files_properties(${FROST_TEST_MANIFEST_RC} PROPERTIES OBJECT_DEPENDS ${FROST_MANIFEST_PATH})
            target_link_options(${target} PRIVATE /MANIFEST:NO)
        endif()
        add_test(NAME fuzz_replay.${name} COMMAND ${target} ${ARG_CORPUS})
        set_tests_properties(fuzz_replay.${name} PROPERTIES LABELS unit)
    endif()
endfunction()
