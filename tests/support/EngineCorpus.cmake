# SPDX-License-Identifier: Apache-2.0
# Shared R6 corpus-target wiring: every real engine test uses the locked platform preset and tools.
include_guard(GLOBAL)

# Adds corpus pins, the independent protocol oracle, and private-desktop linkage to an existing target.
# Missing pins or tools fail configuration; this helper does not provision or launch the engine.
function(frost_configure_engine_corpus_target target)
    set(corpus_lock_file "${FROST_SOURCE_DIR}/tests/corpus/corpora.lock.json")
    set(sdk_lock_file "${FROST_SOURCE_DIR}/sdk/sdk.lock.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${corpus_lock_file}" "${sdk_lock_file}")
    file(READ "${corpus_lock_file}" corpus_lock)
    file(READ "${sdk_lock_file}" sdk_lock)
    string(JSON corpus_count LENGTH "${corpus_lock}" corpora)
    math(EXPR corpus_last "${corpus_count} - 1")
    if(WIN32)
        set(platform_key windows)
    else()
        set(platform_key linux)
    endif()
    set(engine_sha "")
    foreach(index RANGE ${corpus_last})
        string(JSON id GET "${corpus_lock}" corpora ${index} id)
        if(id STREQUAL "frostengine")
            string(JSON engine_sha GET "${corpus_lock}" corpora ${index} sha)
            string(JSON engine_preset GET "${corpus_lock}" corpora ${index} presets ${platform_key})
        endif()
    endforeach()
    if(NOT engine_sha MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "${corpus_lock_file} has no pinned FrostEngine SHA")
    endif()
    string(JSON vc_tools GET "${sdk_lock}" msvc vcToolsVersion)
    if(WIN32 AND NOT Python3_EXECUTABLE AND DEFINED ENV{FROST_SDK})
        set(Python3_EXECUTABLE "$ENV{FROST_SDK}/py/Scripts/python.exe")
    endif()
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" engine_python)
    target_include_directories(${target} PRIVATE "${FROST_SOURCE_DIR}/tests")
    target_compile_definitions(${target} PRIVATE
        FROST_CORPUS_FROSTENGINE_SHA="${engine_sha}"
        FROST_CORPUS_FROSTENGINE_PRESET="${engine_preset}"
        FROST_PINNED_VC_TOOLS="${vc_tools}"
        FROST_TEST_CMAKE="${CMAKE_COMMAND}"
        FROST_TEST_PYTHON="${engine_python}"
        FROST_ENGINE_SESSION_ORACLE="${FROST_SOURCE_DIR}/tests/journeys/engine_session_support.py")
    if(WIN32)
        target_link_libraries(${target} PRIVATE user32)
    endif()
endfunction()
