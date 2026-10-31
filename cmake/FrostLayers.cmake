# SPDX-License-Identifier: Apache-2.0
#
# FrostLayers.cmake: target helpers that enforce the module layering from
# PLAN.md section 3.1/3.2 at configure time.
#
# Every FrostIDE library belongs to a layer with a rank. A library may depend
# only on libraries of a strictly lower rank, or on libraries of its own layer.
# Only the "ui" and "app" layers may link Qt. A violation fails configure, so
# layering mistakes never reach a build.
#
#   frost_add_library(<name> LAYER <layer> [SOURCES ...] [DEPS frost libs...]
#                     [THIRD_PARTY imported targets...] [INTERFACE])
#   frost_add_executable(<name> LAYER <layer> SOURCES ... [DEPS ...] [THIRD_PARTY ...])
#   frost_add_test(<name> SOURCES ... [DEPS ...] [THIRD_PARTY ...] [LABELS ...] [QT])

include_guard(GLOBAL)
include(FrostCompilerOptions)

# Layer ranks, lowest first. Libraries of equal rank but different layers may
# not depend on each other.
set(FROST_LAYER_RANK_compat 0)
set(FROST_LAYER_RANK_core 1)
set(FROST_LAYER_RANK_sched 2)
set(FROST_LAYER_RANK_io 3)
set(FROST_LAYER_RANK_text 4)
set(FROST_LAYER_RANK_storage 4)
set(FROST_LAYER_RANK_rpc 4)
set(FROST_LAYER_RANK_syntax 5)
set(FROST_LAYER_RANK_settings 5)
set(FROST_LAYER_RANK_protocols 5)
set(FROST_LAYER_RANK_project 6)
set(FROST_LAYER_RANK_lang 7)
set(FROST_LAYER_RANK_build 7)
set(FROST_LAYER_RANK_debug 7)
set(FROST_LAYER_RANK_vcs 7)
set(FROST_LAYER_RANK_term 7)
set(FROST_LAYER_RANK_search 7)
set(FROST_LAYER_RANK_test_runner 7)
set(FROST_LAYER_RANK_agent 8)
set(FROST_LAYER_RANK_diag 8)
set(FROST_LAYER_RANK_update 8)
set(FROST_LAYER_RANK_ui 9)
set(FROST_LAYER_RANK_app 10)

# Fails configure unless every dependency of <target> (in <layer>) is allowed.
function(_frost_check_layering target layer deps third_party)
    if(NOT DEFINED FROST_LAYER_RANK_${layer})
        message(FATAL_ERROR "${target}: unknown layer '${layer}' (see cmake/FrostLayers.cmake)")
    endif()
    set(rank ${FROST_LAYER_RANK_${layer}})
    foreach(dep IN LISTS deps)
        if(NOT TARGET ${dep})
            message(FATAL_ERROR "${target}: dependency '${dep}' is not a target (declare it before use)")
        endif()
        get_target_property(dep_layer ${dep} FROST_LAYER)
        if(NOT dep_layer)
            message(FATAL_ERROR "${target}: '${dep}' is not a FrostIDE library; list it under THIRD_PARTY")
        endif()
        set(dep_rank ${FROST_LAYER_RANK_${dep_layer}})
        if(NOT (dep_layer STREQUAL layer OR dep_rank LESS rank))
            message(FATAL_ERROR
                "layering violation: ${target} (layer ${layer}, rank ${rank}) may not depend on "
                "${dep} (layer ${dep_layer}, rank ${dep_rank})")
        endif()
    endforeach()
    foreach(lib IN LISTS third_party)
        if(lib MATCHES "^Qt6?::" AND NOT layer MATCHES "^(ui|app)$")
            message(FATAL_ERROR "layering violation: ${target} (layer ${layer}) may not link Qt (${lib})")
        endif()
    endforeach()
endfunction()

function(frost_add_library name)
    cmake_parse_arguments(ARG "INTERFACE" "LAYER" "SOURCES;DEPS;THIRD_PARTY" ${ARGN})
    set(target frost_${name})
    _frost_check_layering(${target} "${ARG_LAYER}" "${ARG_DEPS}" "${ARG_THIRD_PARTY}")
    if(ARG_INTERFACE)
        add_library(${target} INTERFACE)
        target_compile_features(${target} INTERFACE cxx_std_26)
        target_include_directories(${target} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(${target} INTERFACE ${ARG_DEPS} ${ARG_THIRD_PARTY})
    else()
        add_library(${target} STATIC ${ARG_SOURCES})
        target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(${target} PUBLIC ${ARG_DEPS} ${ARG_THIRD_PARTY})
        frost_apply_compile_options(${target} PRODUCTION)
    endif()
    set_target_properties(${target} PROPERTIES FROST_LAYER ${ARG_LAYER})
    add_library(frost::${name} ALIAS ${target})
endfunction()

function(frost_add_executable name)
    cmake_parse_arguments(ARG "WIN32" "LAYER;OUTPUT_NAME" "SOURCES;DEPS;THIRD_PARTY" ${ARGN})
    _frost_check_layering(${name} "${ARG_LAYER}" "${ARG_DEPS}" "${ARG_THIRD_PARTY}")
    if(ARG_WIN32)
        add_executable(${name} WIN32 ${ARG_SOURCES})
    else()
        add_executable(${name} ${ARG_SOURCES})
    endif()
    target_link_libraries(${name} PRIVATE ${ARG_DEPS} ${ARG_THIRD_PARTY})
    frost_apply_compile_options(${name} PRODUCTION)
    set_target_properties(${name} PROPERTIES FROST_LAYER ${ARG_LAYER})
    if(ARG_OUTPUT_NAME)
        set_target_properties(${name} PROPERTIES OUTPUT_NAME ${ARG_OUTPUT_NAME})
    endif()
endfunction()

# On Windows, copies the DLLs of imported shared libraries (Qt) next to
# <target> after each build so it runs from the build tree without PATH edits.
function(frost_copy_runtime_dlls target)
    if(WIN32)
        # `cmake -E true` swallows the arguments when the target has no runtime DLLs,
        # because copy_if_different with only a destination is an error.
        set(dlls "$<TARGET_RUNTIME_DLLS:${target}>")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:${dlls}>,copy_if_different,true>
                    ${dlls} $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endif()
endfunction()

# Test executables sit outside the layer graph (they may use any library) and
# are registered with CTest through gtest_discover_tests.
#
# QT marks tests that need Qt at runtime. They link tests/support/qt_test_main.cpp
# instead of gtest_main: it selects the offscreen platform and Qt's plugin
# directory before creating the QApplication, so ctest needs no environment setup.
function(frost_add_test name)
    cmake_parse_arguments(ARG "QT" "" "SOURCES;DEPS;THIRD_PARTY;LABELS" ${ARGN})
    if(ARG_QT)
        add_executable(${name} ${ARG_SOURCES} ${FROST_SOURCE_DIR}/tests/support/qt_test_main.cpp)
        target_link_libraries(${name} PRIVATE ${ARG_DEPS} ${ARG_THIRD_PARTY} Qt6::Widgets GTest::gtest GTest::gmock)
        target_compile_definitions(${name} PRIVATE FROST_QT_PLUGIN_DIR="${FROST_QT_PLUGIN_DIR}")
    else()
        add_executable(${name} ${ARG_SOURCES})
        target_link_libraries(${name} PRIVATE ${ARG_DEPS} ${ARG_THIRD_PARTY} GTest::gtest GTest::gmock GTest::gtest_main)
    endif()
    frost_apply_compile_options(${name} TEST)
    frost_copy_runtime_dlls(${name})
    if(WIN32)
        # Same process settings as frost.exe (UTF-8 code page, long paths, DPI awareness).
        target_sources(${name} PRIVATE ${FROST_TEST_MANIFEST_RC})
        target_link_options(${name} PRIVATE /MANIFEST:NO)
    endif()
    if(NOT ARG_LABELS)
        set(ARG_LABELS unit)
    endif()
    gtest_discover_tests(${name}
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 60
        PROPERTIES LABELS "${ARG_LABELS}")
endfunction()
