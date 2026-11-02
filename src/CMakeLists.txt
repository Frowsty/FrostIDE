# SPDX-License-Identifier: Apache-2.0
#
# FrostIDE libraries, lowest layer first (cmake/FrostLayers.cmake checks the
# order). Qt-based layers are skipped when FROST_BUILD_UI is OFF.
add_subdirectory(compat)
add_subdirectory(core)
add_subdirectory(sched)
add_subdirectory(io)
add_subdirectory(text)
# frost::diff (ADR 0019) needs only core and text, so fuzz trees build it too (its fuzz target).
add_subdirectory(diff)
# The editor model (ADR 0025) links RE2 and utf8proc from vcpkg; fuzz trees install no ports.
if(NOT FROST_FUZZ)
    add_subdirectory(text/editor)
endif()
add_subdirectory(input)
# frost::syntax builds in fuzz trees too: it needs no vcpkg port (tree-sitter is vendored, ADR 0023).
add_subdirectory(syntax)
# Fuzz trees install no vcpkg ports, so storage (SQLite, xxHash), settings (yyjson), project
# (pugixml), document (it links storage and settings, ADR 0028), build (it depends on project) and
# lang (on project, settings and yyjson) are left out of them.
if(NOT FROST_FUZZ)
    add_subdirectory(storage)
    add_subdirectory(settings)
    add_subdirectory(project)
    add_subdirectory(document)
    add_subdirectory(build)
    add_subdirectory(lang)
endif()
if(NOT FROST_FUZZ)
    # Crash reports and logging need crashpad, cpptrace and spdlog, which fuzz trees do not install.
    add_subdirectory(diag)
endif()
if(FROST_BUILD_UI)
    add_subdirectory(ui)
    add_subdirectory(app)
endif()
