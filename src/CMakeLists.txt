# SPDX-License-Identifier: Apache-2.0
#
# FrostIDE libraries, lowest layer first (cmake/FrostLayers.cmake checks the
# order). Qt-based layers are skipped when FROST_BUILD_UI is OFF.
add_subdirectory(compat)
add_subdirectory(core)
add_subdirectory(sched)
add_subdirectory(io)
add_subdirectory(text)
if(NOT FROST_FUZZ)
    # Crash reports and logging need crashpad, cpptrace and spdlog, which fuzz trees do not install.
    add_subdirectory(diag)
endif()
if(FROST_BUILD_UI)
    add_subdirectory(app)
endif()
