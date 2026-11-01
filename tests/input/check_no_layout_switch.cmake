# SPDX-License-Identifier: Apache-2.0
#
# The keyboard-layout guard (ADR 0016 decision 13). FrostIDE and its tests only read the
# active keyboard layout; nothing may switch, load, activate or unload a layout, change the
# input-language settings, or (in the input code and its tests) inject real input, which
# reaches the user's desktop even from a hidden one.
#
#   cmake -DFROST_SOURCE_DIR=<repo> -DMODE=scan -P check_no_layout_switch.cmake
#     Scans src/**, tests/** and tools/** (except this script and the planted fixture in
#     tests/input/fixtures/layout_guard/) and fails on any banned name, or when fewer than
#     50 files were scanned (a vacuous scan).
#   cmake -DFROST_SOURCE_DIR=<repo> -DMODE=fixture -P check_no_layout_switch.cmake
#     Scans the planted fixture and fails unless every banned name is reported, so the
#     scanner is known to detect each one.
# Files are searched as hex, so binary files (fuzz corpora) are scanned completely.
cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FROST_SOURCE_DIR OR NOT DEFINED MODE)
    message(FATAL_ERROR "usage: cmake -DFROST_SOURCE_DIR=<repo> -DMODE=scan|fixture -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

# Banned in every scanned file: the APIs, messages, cmdlets and tools that change layouts.
set(banned_everywhere
    "LoadKeyboardLayout"
    "ActivateKeyboardLayout"
    "UnloadKeyboardLayout"
    "SPI_SETDEFAULTINPUTLANG"
    "WM_INPUTLANGCHANGEREQUEST"
    "InstallLayoutOrTip"
    "Set-WinUserLanguageList"
    "Set-WinDefaultInputMethodOverride"
    "setxkbmap"
    "xmodmap"
    "xkbcomp"
    "localectl"
    "gsettings set org.gnome.desktop.input-sources"
    "ibus engine"
    "org.kde.keyboard")
# Banned in the input code and its tests: real-input injection.
set(banned_in_input
    "SendInput"
    "keybd_event"
    "XTestFakeKeyEvent"
    "xdotool"
    "ydotool")
set(input_dirs "src/input/" "src/ui/input/" "tests/input/" "tests/ui/")
set(fixture_dir "tests/input/fixtures/layout_guard/")
set(this_script "tests/input/check_no_layout_switch.cmake")

# Sets <out> to TRUE when <needle> occurs in the file whose hex dump is <hex>.
function(_frost_contains hex needle out)
    string(HEX "${needle}" needle_hex)
    set(rest "${hex}")
    set(found FALSE)
    while(TRUE)
        string(FIND "${rest}" "${needle_hex}" pos)
        if(pos EQUAL -1)
            break()
        endif()
        # A match must start on a byte boundary (an even hex offset). `rest` always starts
        # on one, because it is only ever advanced by whole bytes.
        math(EXPR parity "${pos} % 2")
        if(parity EQUAL 0)
            set(found TRUE)
            break()
        endif()
        math(EXPR next "${pos} + 1")
        string(SUBSTRING "${rest}" ${next} -1 rest)
    endwhile()
    set(${out} ${found} PARENT_SCOPE)
endfunction()

# Appends "<file>: <name>" to <hits> for every banned name found in <rel>.
function(_frost_scan_file rel hits_var)
    file(READ "${FROST_SOURCE_DIR}/${rel}" hex HEX)
    set(names ${banned_everywhere})
    set(in_input FALSE)
    foreach(dir IN LISTS input_dirs)
        string(FIND "${rel}" "${dir}" at)
        if(at EQUAL 0)
            set(in_input TRUE)
        endif()
    endforeach()
    string(FIND "${rel}" "${fixture_dir}" at)
    if(in_input OR at EQUAL 0)
        list(APPEND names ${banned_in_input})
    endif()
    set(hits ${${hits_var}})
    foreach(name IN LISTS names)
        _frost_contains("${hex}" "${name}" found)
        if(found)
            list(APPEND hits "${rel}: ${name}")
        endif()
    endforeach()
    set(${hits_var} ${hits} PARENT_SCOPE)
endfunction()

if(MODE STREQUAL "scan")
    file(GLOB_RECURSE files RELATIVE "${FROST_SOURCE_DIR}"
        "${FROST_SOURCE_DIR}/src/*" "${FROST_SOURCE_DIR}/tests/*" "${FROST_SOURCE_DIR}/tools/*")
    set(hits)
    set(scanned 0)
    foreach(rel IN LISTS files)
        string(FIND "${rel}" "${fixture_dir}" at)
        if(at EQUAL 0 OR rel STREQUAL this_script)
            continue()
        endif()
        _frost_scan_file("${rel}" hits)
        math(EXPR scanned "${scanned} + 1")
    endforeach()
    if(hits)
        list(JOIN hits "\n  " text)
        message(FATAL_ERROR "keyboard-layout guard: banned names found (ADR 0016):\n  ${text}")
    endif()
    if(scanned LESS 50)
        message(FATAL_ERROR "keyboard-layout guard: only ${scanned} files scanned; the scan is vacuous")
    endif()
    message(STATUS "keyboard-layout guard: ${scanned} files clean")
elseif(MODE STREQUAL "fixture")
    file(GLOB_RECURSE files RELATIVE "${FROST_SOURCE_DIR}" "${FROST_SOURCE_DIR}/${fixture_dir}*")
    if(NOT files)
        message(FATAL_ERROR "keyboard-layout guard: no fixture files under ${fixture_dir}")
    endif()
    set(hits)
    foreach(rel IN LISTS files)
        _frost_scan_file("${rel}" hits)
    endforeach()
    set(missing)
    foreach(name IN LISTS banned_everywhere banned_in_input)
        set(reported FALSE)
        foreach(hit IN LISTS hits)
            string(FIND "${hit}" ": ${name}" at)
            if(NOT at EQUAL -1)
                string(LENGTH "${hit}" hit_length)
                string(LENGTH ": ${name}" tail_length)
                math(EXPR expected "${hit_length} - ${tail_length}")
                if(at EQUAL expected)
                    set(reported TRUE)
                endif()
            endif()
        endforeach()
        if(NOT reported)
            list(APPEND missing "${name}")
        endif()
    endforeach()
    if(missing)
        list(JOIN missing ", " text)
        message(FATAL_ERROR "keyboard-layout guard: the scanner missed planted names: ${text}")
    endif()
    list(LENGTH hits count)
    message(STATUS "keyboard-layout guard: all ${count} planted names detected")
else()
    message(FATAL_ERROR "MODE must be scan or fixture, not '${MODE}'")
endif()
