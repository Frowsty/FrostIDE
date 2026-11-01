# SPDX-License-Identifier: Apache-2.0
#
# FrostIDE libraries, lowest layer first (cmake/FrostLayers.cmake checks the
# order). Qt-based layers are skipped when FROST_BUILD_UI is OFF.
add_subdirectory(compat)
add_subdirectory(core)
add_subdirectory(sched)
add_subdirectory(io)
add_subdirectory(text)
add_subdirectory(input)
# frost::syntax builds in fuzz trees too: it needs no vcpkg port (tree-sitter is vendored, ADR 0023).
add_subdirectory(syntax)
# Fuzz trees install no vcpkg ports, so storage (SQLite, xxHash) is left out of them.
if(NOT FROST_FUZZ)
    add_subdirectory(storage)
endif()
if(FROST_BUILD_UI)
    add_subdirectory(ui)
    add_subdirectory(app)
endif()
