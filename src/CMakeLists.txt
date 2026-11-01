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
# Fuzz trees install no vcpkg ports, so storage (SQLite, xxHash) and project (pugixml) are left
# out of them, and build with them (it depends on project).
if(NOT FROST_FUZZ)
    add_subdirectory(storage)
    add_subdirectory(project)
    add_subdirectory(build)
endif()
if(FROST_BUILD_UI)
    add_subdirectory(ui)
    add_subdirectory(app)
endif()
