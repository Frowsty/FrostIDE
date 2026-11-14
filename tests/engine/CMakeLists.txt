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
    SOURCES asset_index_test.cpp engine_test.cpp command_policy_test.cpp project_content_test.cpp
            binding_index_test.cpp binding_queries_test.cpp binding_preview_test.cpp binding_lua_metadata_test.cpp
            engine_test_provider_test.cpp crash_artifact_test.cpp
    DEPS frost::engine frost::lang frost::text frost::sched frost::rpc frost::io frost::core
    LABELS unit)
target_include_directories(engine_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

# FER-2 exercises real loopback connections and independent reader/watchdog threads.
frost_add_test(engine_session_tests
    SOURCES session_test.cpp
    DEPS frost::engine frost::rpc frost::io frost::core
    THIRD_PARTY unofficial-sodium::sodium
    LABELS integration)
target_include_directories(engine_session_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
if(WIN32)
    target_link_libraries(engine_session_tests PRIVATE ws2_32)
endif()

include("${FROST_SOURCE_DIR}/tests/support/EngineCorpus.cmake")
frost_add_test(engine_session_corpus_tests
    SOURCES session_corpus_test.cpp
    DEPS frost::engine frost::build frost::rpc frost::io frost::core
    LABELS integration corpus)
frost_configure_engine_corpus_target(engine_session_corpus_tests)

# FER-10 compares discovery, selected/all/failed results and cancellation with the real CLI.
frost_add_test(engine_test_provider_corpus_tests
    SOURCES engine_test_provider_corpus_test.cpp
    DEPS frost::engine frost::build frost::rpc frost::io frost::core
    LABELS integration corpus)
frost_configure_engine_corpus_target(engine_test_provider_corpus_tests)

# FEC-13 compares usages with real providers and applies the reviewed preview only in an owned
# disposable clone before a full engine build/test/exporter oracle. The shared lock spans each case.
frost_add_test(binding_queries_corpus_tests
    SOURCES binding_queries_corpus_test.cpp binding_corpus_support.hpp
    DEPS frost::engine frost::build frost::lang frost::sched frost::text frost::io frost::rpc frost::core
    LABELS integration corpus clangd luals
    TIMEOUT 18000)
frost_configure_engine_corpus_target(binding_queries_corpus_tests)
target_compile_definitions(binding_queries_corpus_tests PRIVATE
    FROST_CLANG_BINARY="$<TARGET_FILE:frost-clang>"
    FROST_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    FROST_BINDING_CORPORA_SCRIPT="${FROST_SOURCE_DIR}/tools/corpora.py"
    FROST_BINDING_REPO_ROOT="${FROST_SOURCE_DIR}"
    FROST_BINDING_CORPUS_ARTIFACTS="${CMAKE_BINARY_DIR}/binding-corpus-artifacts")
add_dependencies(binding_queries_corpus_tests frost-clang)

# FER-16 uses the actual contained Windows engine CrashHandler helper and mutual-authentication
# source correlation; parser fixtures alone cannot establish native minidump acceptance.
if(WIN32)
    frost_add_test(engine_crash_artifact_corpus_tests
        SOURCES crash_artifact_corpus_test.cpp
        DEPS frost::engine frost::build frost::io frost::rpc frost::core
        THIRD_PARTY unofficial-sodium::sodium
        LABELS integration corpus
        TIMEOUT 18000)
    frost_configure_engine_corpus_target(engine_crash_artifact_corpus_tests)
endif()

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
