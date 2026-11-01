# SPDX-License-Identifier: Apache-2.0
#
# FrostEmbed.cmake: embeds text files (frost::syntax's tree-sitter queries, ADR 0023) into a
# target as byte arrays, without #embed (banned, CLAUDE.md) or string-literal length limits.
#
#   frost_embed_files(<target> FILES <file>...)
#
# For each file, configure writes <binary dir>/embedded/<identifier>.inc, where <identifier> is
# the file's path relative to the current source directory passed through
# string(MAKE_C_IDENTIFIER) ("queries/cpp/highlights.scm" -> queries_cpp_highlights_scm). The
# .inc holds the bytes as a comma-separated hex list, so a source embeds it as
#
#   constexpr unsigned char kBytes[] = {
#   #include "queries_cpp_highlights_scm.inc"
#   };
#
# The file is rewritten only when its content changes (file(CONFIGURE)), and each input is a
# CMAKE_CONFIGURE_DEPENDS of the directory, so editing it re-runs configure. The directory is
# added to the target's PRIVATE include path. Empty files are refused (an empty initializer
# list would not compile).

include_guard(GLOBAL)

function(frost_embed_files target)
    cmake_parse_arguments(ARG "" "" "FILES" ${ARGN})
    if(ARG_UNPARSED_ARGUMENTS OR NOT ARG_FILES)
        message(FATAL_ERROR "frost_embed_files(${target}): usage is frost_embed_files(<target> FILES <file>...)")
    endif()
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/embedded")
    foreach(file IN LISTS ARG_FILES)
        get_filename_component(abs "${file}" ABSOLUTE)
        file(RELATIVE_PATH rel "${CMAKE_CURRENT_SOURCE_DIR}" "${abs}")
        string(MAKE_C_IDENTIFIER "${rel}" name)
        file(READ "${abs}" hex HEX)
        if(hex STREQUAL "")
            message(FATAL_ERROR "frost_embed_files(${target}): ${rel} is empty")
        endif()
        # One "0xNN," per byte, 32 bytes per line.
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
        string(REGEX REPLACE "((0x..,){32})" "\\1\n" bytes "${bytes}")
        file(CONFIGURE OUTPUT "${out_dir}/${name}.inc" CONTENT "${bytes}\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${abs}")
    endforeach()
    target_include_directories(${target} PRIVATE "${out_dir}")
endfunction()
