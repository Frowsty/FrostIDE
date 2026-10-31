# SPDX-License-Identifier: Apache-2.0
#
# FrostIDE libraries, lowest layer first (cmake/FrostLayers.cmake checks the
# order). Qt-based layers are skipped when FROST_BUILD_UI is OFF.
add_subdirectory(compat)
add_subdirectory(core)
# add_subdirectory(sched)  # enabled with the scheduler slice
# add_subdirectory(io)
if(FROST_BUILD_UI)
    add_subdirectory(app)
endif()
