# SPDX-License-Identifier: Apache-2.0
#
# frost::term (PLAN.md section 2 "Terminal", section 3.2's src/term; ADR 0051): the integrated
# terminal's Qt-free core. The vendored libvterm 0.3.3 (third_party/libvterm, pinned in
# third_party/vendor.lock.json) as a small static C library, the pseudo-terminal layer (the system
# ConPTY on Windows, posix_openpt on Linux; children start through frost::io's launcher), the screen
# model over libvterm, the terminal sessions with their reader, writer and waiter threads, the shell
# detection with the terminal's trust rule, and the FakePtyLauncher double.
# Kitty enhancements beyond flags 1 and 4 and complete layout fidelity remain open (ADR 0051).

set(_frost_vterm_root "${FROST_SOURCE_DIR}/third_party/libvterm")

# libvterm from upstream bytes only (warnings off: its diagnostics are not ours to fix). Deliberately
# not frost_-prefixed: a third-party library for FrostLayers' THIRD_PARTY check.
add_library(vterm STATIC
    "${_frost_vterm_root}/src/encoding.c"
    "${_frost_vterm_root}/src/keyboard.c"
    "${_frost_vterm_root}/src/mouse.c"
    "${_frost_vterm_root}/src/parser.c"
    "${_frost_vterm_root}/src/pen.c"
    "${_frost_vterm_root}/src/screen.c"
    "${_frost_vterm_root}/src/state.c"
    "${_frost_vterm_root}/src/unicode.c"
    "${_frost_vterm_root}/src/vterm.c")
frost_apply_c_options(vterm VENDORED)
target_include_directories(vterm PRIVATE "${_frost_vterm_root}/src")
target_include_directories(vterm SYSTEM PUBLIC "${_frost_vterm_root}/include")

set(frost_term_sources
    src/keys.cpp
    src/pty_common.cpp
    src/session.cpp
    src/shell.cpp
    src/vt_screen.cpp)
if(WIN32)
    list(APPEND frost_term_sources src/pty_win.cpp)
else()
    list(APPEND frost_term_sources src/pty_posix.cpp)
endif()

frost_add_library(term LAYER term
    SOURCES ${frost_term_sources}
    DEPS frost::settings frost::io frost::core)
# libvterm is an implementation detail: no vterm.h in the public headers.
target_link_libraries(frost_term PRIVATE vterm)

# Bundled ConPTY sits beside each executable using frost::term. If the pinned SDK is
# absent, the runtime explicitly falls back to the system ConPTY.
function(frost_deploy_conpty target)
    if(NOT WIN32 OR NOT DEFINED ENV{FROST_SDK})
        return()
    endif()
    file(READ "${FROST_SOURCE_DIR}/sdk/sdk.lock.json" _frost_sdk_lock)
    string(JSON _frost_conpty_version GET "${_frost_sdk_lock}" windows conpty version)
    file(TO_CMAKE_PATH "$ENV{FROST_SDK}/conpty-${_frost_conpty_version}" _frost_conpty_root)
    set(_frost_conpty_dll "${_frost_conpty_root}/runtimes/win-x64/native/conpty.dll")
    set(_frost_conpty_host "${_frost_conpty_root}/build/native/runtimes/x64/OpenConsole.exe")
    if(EXISTS "${_frost_conpty_dll}" AND EXISTS "${_frost_conpty_host}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_frost_conpty_dll}" "$<TARGET_FILE_DIR:${target}>/conpty.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_frost_conpty_host}" "$<TARGET_FILE_DIR:${target}>/OpenConsole.exe"
            VERBATIM)
    endif()
endfunction()
