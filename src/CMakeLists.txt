# SPDX-License-Identifier: Apache-2.0
#
# FrostIDE libraries, lowest layer first (cmake/FrostLayers.cmake checks the
# order). Qt-based layers are skipped when FROST_BUILD_UI is OFF.
add_subdirectory(compat)
add_subdirectory(core)
add_subdirectory(sched)
add_subdirectory(io)
add_subdirectory(text)
# The editor model (ADR 0025) links RE2 and utf8proc from vcpkg; fuzz trees install no ports.
if(NOT FROST_FUZZ)
    add_subdirectory(text/editor)
endif()
add_subdirectory(input)
if(FROST_BUILD_UI)
    add_subdirectory(ui)
    add_subdirectory(app)
endif()
