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
# Two passes enforce this: each helper call checks its declared DEPS and
# THIRD_PARTY lists, and a deferred pass at the end of configure walks every
# layered target's final link graph, so links added later with a raw
# target_link_libraries, or Qt reached through a wrapper target, are caught too.
# Setting FROST_LAYERS_REPORT_ALL reports violations with SEND_ERROR instead of
# FATAL_ERROR, so tests/cmake can see every diagnostic of a fixture in one run.
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

# Reports a layering violation: FATAL_ERROR normally, SEND_ERROR (configure
# continues, generation fails) when FROST_LAYERS_REPORT_ALL is set.
function(_frost_layer_error)
    if(FROST_LAYERS_REPORT_ALL)
        message(SEND_ERROR ${ARGN})
    else()
        message(FATAL_ERROR ${ARGN})
    endif()
endfunction()

# Sets <out> to TRUE if a target of <layer> may depend on one of <dep_layer>.
function(_frost_layer_allows layer dep_layer out)
    set(allowed FALSE)
    if(dep_layer STREQUAL layer OR FROST_LAYER_RANK_${dep_layer} LESS FROST_LAYER_RANK_${layer})
        set(allowed TRUE)
    endif()
    set(${out} ${allowed} PARENT_SCOPE)
endfunction()

# Sets <out> to the real target behind <name> (resolving ALIAS targets), or to
# an empty string when <name> is not a target.
function(_frost_resolve_target name out)
    set(real "")
    if(TARGET "${name}")
        get_target_property(real "${name}" ALIASED_TARGET)
        if(NOT real)
            set(real "${name}")
        endif()
    endif()
    set(${out} "${real}" PARENT_SCOPE)
endfunction()

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
            _frost_layer_error("${target}: '${dep}' is not a FrostIDE library; list it under THIRD_PARTY")
            continue()
        endif()
        _frost_layer_allows(${layer} ${dep_layer} allowed)
        if(NOT allowed)
            _frost_layer_error(
                "layering violation: ${target} (layer ${layer}, rank ${rank}) may not depend on "
                "${dep} (layer ${dep_layer}, rank ${FROST_LAYER_RANK_${dep_layer}})")
        endif()
    endforeach()
    foreach(lib IN LISTS third_party)
        # FrostIDE libraries listed as THIRD_PARTY would skip the rank check above.
        _frost_resolve_target("${lib}" real)
        set(lib_layer "")
        if(real)
            get_target_property(lib_layer "${real}" FROST_LAYER)
        endif()
        if(lib_layer OR lib MATCHES "^frost(::|_)")
            _frost_layer_error("${target}: '${lib}' is a FrostIDE library; list it under DEPS, not THIRD_PARTY")
        elseif(lib MATCHES "^Qt6?::" AND NOT layer MATCHES "^(ui|app)$")
            _frost_layer_error("layering violation: ${target} (layer ${layer}) may not link Qt (${lib})")
        endif()
    endforeach()
endfunction()

# Deferred whole-graph check (runs once, after every CMakeLists.txt has been
# processed). For each layered target it walks LINK_LIBRARIES and
# INTERFACE_LINK_LIBRARIES; non-FrostIDE targets (wrappers, imported targets)
# are followed through their INTERFACE_LINK_LIBRARIES, FrostIDE targets are
# rank-checked (their own edges are checked when they are the owner), and any
# Qt target reached from outside ui/app is a violation. Generator expressions
# other than $<LINK_ONLY:...> are not evaluated and are skipped.
function(_frost_check_layer_graph)
    get_property(targets GLOBAL PROPERTY FROST_LAYERED_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(layer ${target} FROST_LAYER)
        set(queue "")
        foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(libs ${target} ${property})
            if(libs)
                list(APPEND queue ${libs})
            endif()
        endforeach()
        set(seen "")
        while(queue)
            list(POP_FRONT queue item)
            if(item MATCHES "^\\$<LINK_ONLY:([^>]+)>$")
                set(item "${CMAKE_MATCH_1}")
            endif()
            if(item MATCHES "^\\$<")
                continue()
            endif()
            _frost_resolve_target("${item}" real)
            if(NOT real OR real IN_LIST seen OR real STREQUAL target)
                continue()
            endif()
            list(APPEND seen "${real}")
            if(item MATCHES "^Qt6?::" OR real MATCHES "^Qt6?::")
                if(NOT layer MATCHES "^(ui|app)$")
                    _frost_layer_error("layering violation: ${target} (layer ${layer}) reaches Qt (${item}) "
                                       "through its link graph")
                endif()
                continue()
            endif()
            get_target_property(dep_layer "${real}" FROST_LAYER)
            if(dep_layer)
                _frost_layer_allows(${layer} ${dep_layer} allowed)
                if(NOT allowed)
                    _frost_layer_error("layering violation: ${target} (layer ${layer}) links ${real} "
                                       "(layer ${dep_layer}) in its final link graph")
                endif()
                continue()
            endif()
            get_target_property(libs "${real}" INTERFACE_LINK_LIBRARIES)
            if(libs)
                list(APPEND queue ${libs})
            endif()
        endwhile()
    endforeach()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _frost_check_layer_graph)

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
    set_property(GLOBAL APPEND PROPERTY FROST_LAYERED_TARGETS ${target})
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
    set_property(GLOBAL APPEND PROPERTY FROST_LAYERED_TARGETS ${name})
    if(ARG_OUTPUT_NAME)
        set_target_properties(${name} PROPERTIES OUTPUT_NAME ${ARG_OUTPUT_NAME})
    endif()
endfunction()

# On Windows, copies the DLLs of imported shared libraries (Qt) next to
# <target> after each build so it runs from the build tree without PATH edits.
# In ASan builds it also copies clang's ASan runtime DLL (FROST_ASAN_RUNTIME_DLL,
# set by the root CMakeLists.txt), which is not on PATH.
function(frost_copy_runtime_dlls target)
    if(WIN32 AND FROST_ASAN_RUNTIME_DLL)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FROST_ASAN_RUNTIME_DLL}" $<TARGET_FILE_DIR:${target}>
            VERBATIM)
    endif()
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
        set_source_files_properties(${FROST_TEST_MANIFEST_RC} PROPERTIES OBJECT_DEPENDS ${FROST_MANIFEST_PATH})
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
