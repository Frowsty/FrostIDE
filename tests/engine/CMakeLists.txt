# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::engine (FEC-1, FER-1a), the IDE side of the FrostEngine contract:
#   engine_tests         exact handles, the schema.export reader's checks, source resolution and
#                        the program lookup of an unbuilt tree; label "unit".
#   engine_corpus_tests  (Windows) FrostEditor of the pinned FrostEngine clone, built with
#                        `cmake --build --target FrostEditor` under the corpus build lock: its
#                        schema.export validates and every source location is a line of the clone,
#                        and entity ids above 2^53 round-trip exactly through entity.list and
#                        entity.get on a scratch copy of the engine's TestProject; label corpus, so
#                        only the corpus tier (tools\verify.ps1 -Corpus) runs it. Windows only, as
#                        debug_corpus_tests (tests/debug): on Linux FrostEditor is most of an engine
#                        build, which the corpus tier never does (tools/corpora.py, "engine").
frost_add_test(engine_tests
    SOURCES engine_test.cpp
    DEPS frost::engine frost::rpc frost::io frost::core
    LABELS unit)
target_include_directories(engine_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

if(WIN32)
    set(_frost_engine_corpora_lock_file "${FROST_SOURCE_DIR}/tests/corpus/corpora.lock.json")
    set(_frost_engine_sdk_lock_file "${FROST_SOURCE_DIR}/sdk/sdk.lock.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_frost_engine_corpora_lock_file}" "${_frost_engine_sdk_lock_file}")
    file(READ "${_frost_engine_corpora_lock_file}" _frost_engine_corpora_lock)
    string(JSON _frost_engine_count LENGTH "${_frost_engine_corpora_lock}" corpora)
    math(EXPR _frost_engine_last "${_frost_engine_count} - 1")
    set(_frost_engine_corpus_sha "")
    foreach(_frost_engine_index RANGE ${_frost_engine_last})
        string(JSON _frost_engine_id GET "${_frost_engine_corpora_lock}" corpora ${_frost_engine_index} id)
        if(_frost_engine_id STREQUAL "frostengine")
            string(JSON _frost_engine_corpus_sha GET "${_frost_engine_corpora_lock}" corpora ${_frost_engine_index} sha)
            string(JSON _frost_engine_corpus_preset GET "${_frost_engine_corpora_lock}" corpora ${_frost_engine_index}
                   presets windows)
        endif()
    endforeach()
    if(NOT _frost_engine_corpus_sha MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "${_frost_engine_corpora_lock_file} pins no frostengine corpus")
    endif()
    file(READ "${_frost_engine_sdk_lock_file}" _frost_engine_sdk_lock)
    string(JSON _frost_engine_vc_tools GET "${_frost_engine_sdk_lock}" msvc vcToolsVersion)
    frost_add_test(engine_corpus_tests
        SOURCES engine_corpus_test.cpp
        DEPS frost::engine frost::build frost::io frost::rpc frost::core
        LABELS integration corpus)
    target_include_directories(engine_corpus_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
    target_compile_definitions(engine_corpus_tests PRIVATE
        FROST_CORPUS_FROSTENGINE_SHA="${_frost_engine_corpus_sha}"
        FROST_CORPUS_FROSTENGINE_PRESET="${_frost_engine_corpus_preset}"
        FROST_PINNED_VC_TOOLS="${_frost_engine_vc_tools}"
        FROST_TEST_CMAKE="${CMAKE_COMMAND}")
endif()
